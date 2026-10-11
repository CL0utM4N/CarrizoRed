//
//  kern_cr.cpp
//  CarrizoRed
//
//  Phase 1: observe only. Finds the iGPU, logs what the hardware reports, and
//  watches Apple's Polaris kexts load. Nothing is patched yet.
//

#include "kern_cr.hpp"
#include "kern_vbios.hpp"

#include <Headers/kern_api.hpp>
#include <Headers/kern_devinfo.hpp>
#include <Headers/kern_iokit.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <kern/thread.h>
#include <kern/debug.h>
#include <stdarg.h>

// Lilu's createTrampoline copies a wrapped function's first instructions verbatim ("assuming it is PIC") and jumps back
// after them; nothing is relocated. When the wrapper is more than 2 GB away (kext addresses change every boot) the copy
// covers at least 14 bytes, and a stack-protector prologue's `mov rax, [rip + __stack_chk_guard@GOT]` at +0xb then reads
// from the wrong address. Spoof boot 19's panic (AAPL,PanicInfo): WindowServer opening a framebuffer ran
// AMD9500Controller::hwInitializeHardwareSettings through such a trampoline, loaded "aryP12OS" as the guard pointer and
// took a general-protection fault, which never reached the screen. ATIController::start and initializeProjectInfo have
// the same prologue. Every wrapper with an original therefore goes through routeChecked, which refuses (and logs) any
// function with a RIP-relative operand or a relative branch in its first 14 bytes, whatever the jump size.
static bool routeChecked(KernelPatcher &patcher, size_t index, KernelPatcher::RouteRequest &request,
	mach_vm_address_t slide, size_t size) {
	if (request.org) {
		auto func = patcher.solveSymbol(index, request.symbol, slide, size);
		if (!func) {
			SYSLOG("cr", "route: %s not found (error %d)", request.symbol, patcher.getError());
			return false;
		}
		for (size_t copied = 0; copied < 14;) {
			hde64s hs {};
			size_t len = Disassembler::hdeDisasm(func + copied, &hs);
			if (!len || (hs.flags & F_ERROR)) {
				SYSLOG("cr", "route: REFUSED %s: cannot decode +0x%lx", request.symbol, copied);
				return false;
			}
			bool ripRelative = (hs.flags & F_MODRM) && hs.modrm_mod == 0 && hs.modrm_rm == 5;
			if (ripRelative || (hs.flags & F_RELATIVE)) {
				SYSLOG("cr", "route: REFUSED %s: %s instruction at +0x%lx would be copied unrelocated", request.symbol,
					ripRelative ? "RIP-relative" : "relative-branch", copied);
				return false;
			}
			copied += len;
		}
	}
	return patcher.routeMultiple(index, &request, 1, slide, size);
}

// Short summary of the bring-up trace, repeated by the watcher so it survives the console scrolling.
static char traceSummary[1024];

static void traceNote(const char *fmt, ...) {
	size_t len = strlen(traceSummary);
	if (len + 2 >= sizeof(traceSummary))
		return;
	va_list va;
	va_start(va, fmt);
	vsnprintf(traceSummary + len, sizeof(traceSummary) - len, fmt, va);
	va_end(va);
}

// Bring-up trace events. The wrappers run in whatever context Apple's code calls them from (possibly with a spinlock
// held or from an interrupt), where IOLog can deadlock, so they only append to this lock-free ring; the watcher thread
// prints new events and runs a watchdog over calls that never return.
struct TraceEvent {
	uint64_t ms;
	const char *name;
	uint32_t value;
	bool exit;
};

static constexpr size_t TraceRingSize = 512, MaxTraceStages = 64;
static TraceEvent traceRing[TraceRingSize];
static uint32_t traceHead;
static uint32_t stageOpen[MaxTraceStages];
static uint64_t stageEnterMs[MaxTraceStages];
static const char *stageNames[MaxTraceStages];
// Interrupt-handler (IH ring) activity, shown by the watcher.
static uint32_t ihHandlerCalls, ihReadPointerWrites, ihLastReadPointer;

// Register first-access log. Spoof boots 10-11 froze the whole machine ~20 s after AMD9500Controller::start returned,
// with no traced function running: most likely a periodic task touching a register that hangs the APU (e.g. one in a
// power-gated block). Every register offset is marked on first access; once the controller has been up for 2 s, each
// *new* offset is printed immediately, *before* the access (a read that hangs never returns), so the last line before
// a freeze names it. Lines from interrupts-off context are queued for the watcher unless -crreglogintsoff (IOLog there
// can itself deadlock). -crnoreglog disables this.
// Accesses to display-pipe register blocks the APU doesn't have (pipe >= displayPipes) are always printed (first 100
// offsets); -crpipeguard also drops those writes and returns 0 for those reads.
enum RegOp : uint32_t { RegRead, RegWrite, RegPcieRead, RegPcieWrite, RegIndRead, RegIndWrite, RegOpCount };
static const char *const regOpNames[RegOpCount] {"rd", "WR", "pcie-rd", "PCIE-WR", "ind-rd", "IND-WR"};
static uint8_t regSeen[RegOpCount][0x10000 / 8];
static uint64_t regLogFromMs;  // 0 = not yet (controller not started)
static uint32_t regLogged, regAccesses;
static bool regLogIntsOff, pipeGuard, regLogAll;
static uint32_t pipeLimit = 6, pipeAccesses, pipeLogged;
struct RegEvent {
	RegOp op;
	uint32_t offset, extra, value;
	int pipe;
};
static constexpr size_t RegRingSize = 64;
static RegEvent regRing[RegRingSize];
static uint32_t regRingHead;
static void printRegEvent(const RegEvent &event, const char *suffix);
// Power-management / config-space events on the iGPU (see patchPci).
static uint32_t pmEvents, pciConfigWrites;

// IH storm breaker. IH registers are dword offsets in BAR5, the same on Tonga (oss_3_0) and CZ (oss_3_0_1).
namespace IhReg {
	static constexpr uint32_t RB_CNTL = 0xE30, RB_BASE = 0xE31, RB_RPTR = 0xE32, RB_WPTR = 0xE33, RB_WPTR_ADDR_HI = 0xE34,
	                          RB_WPTR_ADDR_LO = 0xE35, CNTL = 0xE36, STATUS = 0xE38;
}
static volatile uint32_t *gpuMmio;  // BAR5, mapped by probeMMIO
static void *ihManager;                 // BaffinInterruptManager *
static mach_vm_address_t ihGetWptrWbAddress, ihIsUsingVram;
static bool ihStormBroken;
static uint64_t ihWindowStart;
static uint32_t ihWindowCalls;
static struct {
	uint64_t ms;
	uint32_t calls, rbCntl, rbBase, rptr, wptr, wptrAddrHi, wptrAddrLo, cntl, status, wbValue, size;
	uint64_t wbAddress, ringCpu, ringGpu;
	bool vram;
} ihSnapshot;

static uint64_t uptimeMs() {
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000000;
}

static void traceEvent(const char *name, bool exit, uint32_t value) {
	uint32_t index = __atomic_fetch_add(&traceHead, 1, __ATOMIC_RELAXED);
	auto &event = traceRing[index % TraceRingSize];
	event.ms = uptimeMs();
	event.name = name;
	event.value = value;
	event.exit = exit;
}


static const char *pathRadeonX4000[] {"/System/Library/Extensions/AMDRadeonX4000.kext/Contents/MacOS/AMDRadeonX4000"};
static const char *pathRadeonX4000HWServices[] {
	"/System/Library/Extensions/AMDRadeonX4000HWServices.kext/Contents/MacOS/AMDRadeonX4000HWServices"};
static const char *pathPolarisController[] {
	"/System/Library/Extensions/AMD9500Controller.kext/Contents/MacOS/AMD9500Controller"};
static const char *pathSupport[] {"/System/Library/Extensions/AMDSupport.kext/Contents/MacOS/AMDSupport"};
static const char *pathFramebuffer[] {"/System/Library/Extensions/AMDFramebuffer.kext/Contents/MacOS/AMDFramebuffer"};
static const char *pathAgpm[] {
	"/System/Library/Extensions/AppleGraphicsPowerManagement.kext/Contents/MacOS/AppleGraphicsPowerManagement"};
static const char *pathAgdp[] {"/System/Library/Extensions/AppleGraphicsControl.kext/Contents/PlugIns/AppleGraphicsDevicePolicy.kext/Contents/MacOS/AppleGraphicsDevicePolicy"};
static const char *pathPci[] {"/System/Library/Extensions/IOPCIFamily.kext/Contents/MacOS/IOPCIFamily"};

enum KextIndex : size_t {
	KextRadeonX4000,
	KextRadeonX4000HWServices,
	KextPolarisController,
	KextSupport,
	KextFramebuffer,
	KextAgpm,
	KextAgdp,
	KextPci,
	KextCount,
};

static KernelPatcher::KextInfo kexts[KextCount] {
	[KextRadeonX4000] = {"com.apple.kext.AMDRadeonX4000", pathRadeonX4000, arrsize(pathRadeonX4000), {}, {},
		KernelPatcher::KextInfo::Unloaded},
	[KextRadeonX4000HWServices] = {"com.apple.kext.AMDRadeonX4000HWServices", pathRadeonX4000HWServices,
		arrsize(pathRadeonX4000HWServices), {}, {}, KernelPatcher::KextInfo::Unloaded},
	[KextPolarisController] = {"com.apple.kext.AMD9500Controller", pathPolarisController,
		arrsize(pathPolarisController), {}, {}, KernelPatcher::KextInfo::Unloaded},
	[KextSupport] = {"com.apple.kext.AMDSupport", pathSupport, arrsize(pathSupport), {}, {},
		KernelPatcher::KextInfo::Unloaded},
	[KextFramebuffer] = {"com.apple.kext.AMDFramebuffer", pathFramebuffer, arrsize(pathFramebuffer), {}, {},
		KernelPatcher::KextInfo::Unloaded},
	[KextAgpm] = {"com.apple.driver.AGPM", pathAgpm, arrsize(pathAgpm), {}, {}, KernelPatcher::KextInfo::Unloaded},
	[KextAgdp] = {"com.apple.driver.AppleGraphicsDevicePolicy", pathAgdp, arrsize(pathAgdp), {}, {},
		KernelPatcher::KextInfo::Unloaded},
	// Already loaded (boot KC) by the time Lilu runs.
	[KextPci] = {"com.apple.iokit.IOPCIFamily", pathPci, arrsize(pathPci), {true}, {}, KernelPatcher::KextInfo::Unloaded},
};

CRDriver *CRDriver::callback = nullptr;

void CRDriver::init() {
	callback = this;
	SYSLOG("cr", "CarrizoRed starting on kernel %d.%d", getKernelVersion(), getKernelMinorVersion());

	lilu.onPatcherLoadForce(
		[](void *user, KernelPatcher &patcher) { static_cast<CRDriver *>(user)->processPatcher(patcher); }, this);
	lilu.onKextLoadForce(
		kexts, arrsize(kexts),
		[](void *user, KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size) {
			static_cast<CRDriver *>(user)->processKext(patcher, index, slide, size);
		},
		this);
}

void CRDriver::processPatcher(KernelPatcher &) {
	auto *devInfo = DeviceInfo::create();
	if (!devInfo) {
		SYSLOG("cr", "failed to create DeviceInfo");
		disableKextCallbacks();
		return;
	}

	// Lilu classifies the iGPU by its device-id property, so once we spoof a Polaris ID
	// via DeviceProperties it lands in videoExternal instead. Match on config space.
	IORegistryEntry *candidates[1 + 8] {devInfo->videoBuiltin};
	size_t candidateNum = 1;
	for (size_t i = 0; i < devInfo->videoExternal.size() && candidateNum < arrsize(candidates); i++)
		candidates[candidateNum++] = devInfo->videoExternal[i].video;

	for (size_t i = 0; i < candidateNum && !iGPU; i++) {
		auto *pci = OSDynamicCast(IOPCIDevice, candidates[i]);
		if (!pci)
			continue;
		auto vendor = WIOKit::readPCIConfigValue(pci, WIOKit::kIOPCIConfigVendorID);
		auto device = static_cast<uint16_t>(WIOKit::readPCIConfigValue(pci, WIOKit::kIOPCIConfigDeviceID));
		auto fam = familyForDevice(device);
		DBGLOG("cr", "candidate %s vendor 0x%04X device 0x%04X", safeString(pci->getName()), vendor, device);
		if (vendor != WIOKit::VendorID::ATIAMD || fam == ChipFamily::Unknown)
			continue;

		iGPU = pci;
		iGPU->retain();
		family = fam;
		deviceId = device;
	}

	DeviceInfo::deleter(devInfo);

	// DeviceInfo skips devices whose vendor-id *property* isn't a known GPU vendor, and the EFI overrides it to
	// 0xFFFF to keep AMDSupport off the iGPU until we patch it. Fall back to every IOPCIDevice's config space.
	if (!iGPU)
		findByConfigSpace();

	if (!iGPU) {
		SYSLOG("cr", "no supported Carrizo-family iGPU found, disabling");
		disableKextCallbacks();
		return;
	}

	gpuBus = iGPU->getBusNumber();
	gpuDevice = iGPU->getDeviceNumber();
	pipeLimit = displayPipes(family);
	pipeGuard = checkKernelArgument("-crpipeguard");
	regLogIntsOff = checkKernelArgument("-crreglogintsoff");
	regLogAll = checkKernelArgument("-crreglogall");

	logPCIInfo();
	if (checkKernelArgument("-crmacgpu"))
		setMacGpuProperties();
	if (!checkKernelArgument("-crnommio"))
		probeMMIO();

	// AMDSupport reads the VBIOS from "ATY,bin_image" before trying the (absent) PCI ROM.
	if (!checkKernelArgument("-crnovbios")) {
		bool ok = VBIOS::inject(iGPU, WIOKit::VendorID::ATIAMD, deviceId);
		iGPU->setProperty("CarrizoRed,vbios-injected", ok);
	}
}

void CRDriver::findByConfigSpace() {
	// Walk the device tree like DeviceInfo does (it has already waited for the PCI root to publish): this early in
	// boot the IOPCIDevices exist but may not be registered yet, so IOService matching could miss them.
	auto *iterator = IORegistryIterator::iterateOver(gIODTPlane, kIORegistryIterateRecursively);
	if (!iterator)
		return;
	while (auto *entry = iterator->getNextObject()) {
		auto *pci = OSDynamicCast(IOPCIDevice, entry);
		if (!pci)
			continue;
		auto vendor = WIOKit::readPCIConfigValue(pci, WIOKit::kIOPCIConfigVendorID);
		auto device = static_cast<uint16_t>(WIOKit::readPCIConfigValue(pci, WIOKit::kIOPCIConfigDeviceID));
		auto fam = familyForDevice(device);
		if (vendor != WIOKit::VendorID::ATIAMD || fam == ChipFamily::Unknown)
			continue;
		SYSLOG("cr", "found iGPU %s by config space (not listed by DeviceInfo)", safeString(pci->getName()));
		iGPU = pci;
		iGPU->retain();
		family = fam;
		deviceId = device;
		break;
	}
	iterator->release();
}

void CRDriver::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size) {
	for (size_t i = 0; i < arrsize(kexts); i++) {
		if (kexts[i].loadIndex != index)
			continue;
		SYSLOG("cr", "%s loaded: slide 0x%llX size 0x%lX", kexts[i].id, static_cast<uint64_t>(slide),
			static_cast<unsigned long>(size));
		if (i == KextPolarisController)
		{
			patchPolarisController(patcher, index, slide, size);
			traceController(patcher, index, slide, size);
			patchInterrupts(patcher, index, slide, size);
			startWatcher();
		}
		else if (i == KextPci)
			patchPci(patcher, index, slide, size);
		else if (i == KextSupport) {
			patchSupport(patcher, index, slide, size);
			patchPlls(patcher, index, slide, size);
			patchPolicy(patcher, index, slide, size);
			traceSupport(patcher, index, slide, size);
		}
		else if (i == KextFramebuffer)
			traceFramebuffer(patcher, index, slide, size);
		else if (i == KextAgpm) {
			blockProbe(patcher, index, slide, size, "__ZN14AGPMController5probeEP9IOServicePi", "-crnoagpm");
			blockProbe(patcher, index, slide, size, "__ZN4AGPM5probeEP9IOServicePi", "-crnoagpm");
		} else if (i == KextAgdp) {
			if (checkKernelArgument("-crmacgpu"))
				patchAgdpBoardId(patcher, i);
			if (checkKernelArgument("-crnoagdp")) {
				KernelPatcher::RouteRequest request {"__ZN32AppleGraphicsDeviceControlClient5startEP9IOService",
					wrapStartFail};
				if (!routeChecked(patcher, index, request, slide, size)) {
					SYSLOG("cr", "-crnoagdp: failed to route (error %d)", patcher.getError());
					patcher.clearError();
				}
			}
		}
		else if (i == KextRadeonX4000)
			blockProbe(patcher, index, slide, size, "__ZN37AMDRadeonX4000_AMDGraphicsAccelerator5probeEP9IOServicePi",
				"-crnoaccel");
		else if (i == KextRadeonX4000HWServices)
			blockProbe(patcher, index, slide, size,
				"__ZN42AMDRadeonX4000_AMDRadeonHWServicesAbstract5probeEP9IOServicePi", "-crnoaccel");
		return;
	}
}

// BaffinSharedController::getDisplayLimit() and ::getNumLineBuffers() return 6. AMD9500Controller::
// initializeProjectDependentResources() creates a timing generator, cursor, scaler, pipe control and gamma generator
// for each of getDisplayLimit() pipes and a line buffer for each of getNumLineBuffers(), all on fixed per-pipe register
// blocks. Carrizo has 3 pipes and Stoney 2, so limit both to the real count (docs/APPLE_KEXTS.md, "Pipes").
// BaffinSharedController::getDigLimit() returns 6; ATIController::doConnectionsProbe() compares it with the DIGs in use
// when deciding how many displays it can drive. Carrizo and Stoney have 3 (-crnodiglimit disables).
void CRDriver::patchPolarisController(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size) {
	if (checkKernelArgument("-crnopipes")) {
		SYSLOG("cr", "pipe limit patch disabled by -crnopipes");
	} else {
		KernelPatcher::RouteRequest requests[] {
			{"__ZN22BaffinSharedController15getDisplayLimitEv", wrapGetDisplayLimit},
			{"__ZN22BaffinSharedController17getNumLineBuffersEv", wrapGetNumLineBuffers},
		};
		if (patcher.routeMultiple(index, requests, slide, size)) {
			SYSLOG("cr", "AMD9500Controller limited to %u display pipes", displayPipes(family));
		} else {
			SYSLOG("cr", "failed to route AMD9500Controller pipe limits (error %d)", patcher.getError());
			patcher.clearError();
		}
	}

	if (!checkKernelArgument("-crnodiglimit")) {
		KernelPatcher::RouteRequest request {"__ZN22BaffinSharedController11getDigLimitEv", wrapGetDigLimit};
		if (routeChecked(patcher, index, request, slide, size)) {
			SYSLOG("cr", "AMD9500Controller limited to %u DIGs", DisplayDigs);
		} else {
			SYSLOG("cr", "failed to route getDigLimit (error %d)", patcher.getError());
			patcher.clearError();
		}
	}

	// See patchPlls.
	KernelPatcher::RouteRequest request {"__ZN17AMD9500Controller21hwSetCrtcToPixelClockEhhR11ClockParams",
		wrapSetCrtcToPixelClock, orgSetCrtcToPixelClock};
	if (!routeChecked(patcher, index, request, slide, size)) {
		SYSLOG("cr", "failed to route hwSetCrtcToPixelClock (error %d)", patcher.getError());
		patcher.clearError();
	}
}

// Pixel PLLs. AMD9500Controller creates its PLL assigner with a hard-coded 7 PLLs
// (BaffinSharedController::initializeProjectDependentResources → AtiPllAssignerFactory::createPllAssigner(..., 7));
// it hands out the lowest free index, and hwProgramPpllUsingBios passes the index unchanged to the VBIOS SetPixelClock
// table as ucPpll (ActiveConnectorInfo +0x2E). Carrizo/Stoney have two pixel PLLs, which Linux DC
// (command_table_helper_dce110.c) addresses as PLL0 → ATOM_PPLL0 (2) and PLL1 → ATOM_PPLL1 (0); ATOM 1 (PPLL2) isn't
// used on DCE 11.0. So:
//  - limit the assigner to 2 PLLs (-crnoplllimit disables);
//  - remap Apple index 0 → ATOM 0 (unchanged: the first spoof boot lit the panel with it) and 1 → ATOM 2 at
//    AtomBiosProxy::setPixelClock (-crnopllremap disables). Every call is printed with the crtc, both indexes and the
//    clock; indexes 2-5 are reported as invalid on the APU and passed through.
//  - apply the same remap in AMD9500Controller::hwSetCrtcToPixelClock(crtc, pll, params), which writes the index into
//    CRTCn_PIXEL_RATE_CNTL.PIXEL_RATE_SOURCE (assumed to use the ATOM encoding: 0 PPLL1, 1 PPLL2, 2 PPLL0) and picks
//    a Polaris PHYPLLx resync register by it (0x100-0x103, nothing on DCE 11.0; index 5 = 0x13E, Carrizo's
//    PHYPLL_PIXCLK_CNTL, is out of reach with 2 PLLs).
static uint8_t atomPllFor(uint8_t pll) { return pll == 1 && !checkKernelArgument("-crnopllremap") ? 2 : pll; }

void CRDriver::patchPlls(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size) {
	if (!checkKernelArgument("-crnoplllimit")) {
		KernelPatcher::RouteRequest request {
			"__ZN21AtiPllAssignerFactory17createPllAssignerEP13ATIControllerPP12ATIConnectorhh",
			wrapCreatePllAssigner, orgCreatePllAssigner};
		if (!routeChecked(patcher, index, request, slide, size)) {
			SYSLOG("cr", "failed to route createPllAssigner (error %d)", patcher.getError());
			patcher.clearError();
		}
	}
	KernelPatcher::RouteRequest request {"__ZN13AtomBiosProxy13setPixelClockEP11ClockParamsPK19ActiveConnectorInfo",
		wrapSetPixelClock, orgSetPixelClock};
	if (!routeChecked(patcher, index, request, slide, size)) {
		SYSLOG("cr", "failed to route AtomBiosProxy::setPixelClock (error %d)", patcher.getError());
		patcher.clearError();
	}
}

void *CRDriver::wrapCreatePllAssigner(void *controller, void *connectors, uint8_t numConnectors, uint8_t numPlls) {
	uint8_t plls = numPlls > DisplayPlls ? DisplayPlls : numPlls;
	IOLog("CarrizoRed: PLL assigner: %u connectors, %u PLLs (Apple asked for %u)\n", numConnectors, plls, numPlls);
	traceNote("plls=%u/%u ", plls, numPlls);
	return FunctionCast(wrapCreatePllAssigner, callback->orgCreatePllAssigner)(controller, connectors, numConnectors,
		plls);
}

uint64_t CRDriver::wrapSetPixelClock(void *that, void *clockParams, const void *connectorInfo) {
	static constexpr size_t InfoSize = 0x98, CrtcOffset = 0x2D, PllOffset = 0x2E;
	if (!clockParams || !connectorInfo)
		return FunctionCast(wrapSetPixelClock, callback->orgSetPixelClock)(that, clockParams, connectorInfo);
	uint8_t info[InfoSize];
	memcpy(info, connectorInfo, InfoSize);
	uint8_t crtc = info[CrtcOffset], pll = info[PllOffset], atomPll = atomPllFor(pll);
	info[PllOffset] = atomPll;
	uint32_t clock = 0;
	memcpy(&clock, static_cast<uint8_t *>(clockParams) + 0x08, sizeof(clock));
	const char *note = pll >= DisplayPlls && pll <= 5 ? " (INVALID on the APU)" : "";
	if (ml_get_interrupts_enabled())
		IOLog("CarrizoRed: setPixelClock crtc %u PLL %u -> ATOM %u, clock %u%s\n", crtc, pll, atomPll, clock, note);
	traceEvent("setPixelClock crtc<<16|pll<<8|atom", true, (crtc << 16) | (pll << 8) | atomPll);
	return FunctionCast(wrapSetPixelClock, callback->orgSetPixelClock)(that, clockParams, info);
}

void CRDriver::wrapSetCrtcToPixelClock(void *that, uint8_t crtc, uint8_t pll, void *clockParams) {
	uint8_t atomPll = atomPllFor(pll);
	traceEvent("setCrtcToPixelClock crtc<<16|pll<<8|atom", true, (crtc << 16) | (pll << 8) | atomPll);
	FunctionCast(wrapSetCrtcToPixelClock, callback->orgSetCrtcToPixelClock)(that, crtc, atomPll, clockParams);
}

// While Apple's display stack starts, print what attached below the iGPU (classes and names, 3 levels) to the verbose
// console whenever it changes, for 10 minutes. When the panel keeps showing the boot console but the GUI has started
// elsewhere, this is the only output that can be seen. Also published as "CarrizoRed,tree". -crnowatch stops the
// printing (the thread still requests the controller, see below).
void CRDriver::startWatcher() {
	thread_t thread;
	if (kernel_thread_start(watcherThread, this, &thread) == KERN_SUCCESS)
		thread_deallocate(thread);
	else
		SYSLOG("cr", "failed to start the registry watcher");
}

static uint32_t numberProperty(IORegistryEntry *entry, const char *key) {
	auto *obj = entry->getProperty(key);
	if (auto *num = OSDynamicCast(OSNumber, obj))
		return num->unsigned32BitValue();
	if (auto *data = OSDynamicCast(OSData, obj)) {
		uint32_t value = 0;
		memcpy(&value, data->getBytesNoCopy(), data->getLength() < sizeof(value) ? data->getLength() : sizeof(value));
		return value;
	}
	return 0;
}

// One line per framebuffer: connector type, whether a display is attached (an IODisplayConnect child), and the
// current mode's pixel count and clock. Other children: class (name), two levels deep.
static void describeChildren(IORegistryEntry *entry, int depth, char *buf, size_t size) {
	auto *iterator = entry->getChildIterator(gIOServicePlane);
	if (!iterator)
		return;
	int fb = 0;
	while (auto *child = OSDynamicCast(IORegistryEntry, iterator->getNextObject())) {
		size_t len = strlen(buf);
		if (len + 1 >= size)
			break;
		const char *cls = child->getMetaClass()->getClassName();
		if (!strcmp(cls, "AppleMCCSControlModule"))
			continue;
		if (!strcmp(cls, "AtiFbStub")) {
			IORegistryEntry *framebuffer = nullptr;
			bool display = false;
			if (auto *it = child->getChildIterator(gIOServicePlane)) {
				while (auto *grandchild = OSDynamicCast(IORegistryEntry, it->getNextObject())) {
					if (!framebuffer)
						framebuffer = grandchild;
					if (auto *it2 = grandchild->getChildIterator(gIOServicePlane)) {
						while (auto *leaf = OSDynamicCast(IORegistryEntry, it2->getNextObject()))
							if (!strcmp(leaf->getMetaClass()->getClassName(), "IODisplayConnect"))
								display = true;
						it2->release();
					}
				}
				it->release();
			}
			snprintf(buf + len, size - len, "%*sfb%d conn=0x%x display=%s px=%u clk=%u\n", depth * 2, "", fb++,
				numberProperty(child, "connector-type"), display ? "YES" : "no",
				framebuffer ? numberProperty(framebuffer, "IOFBCurrentPixelCount") : 0,
				framebuffer ? numberProperty(framebuffer, "IOFBCurrentPixelClock") : 0);
			continue;
		}
		snprintf(buf + len, size - len, "%*s%s (%s)\n", depth * 2, "", cls, safeString(child->getName()));
		if (depth < 2)
			describeChildren(child, depth + 1, buf, size);
	}
	iterator->release();
}

void CRDriver::watcherThread(void *arg, wait_result_t) {
	auto *self = static_cast<CRDriver *>(arg);
	static char last[1536], now[1536];
	bool print = !checkKernelArgument("-crnowatch");
	bool watchdog = !checkKernelArgument("-crnowatchdog");
	bool controllerRequested = checkKernelArgument("-crnoloadctl");
	// crpanicat=N: panic on purpose at t=N s. Every "freeze" so far happened as the GUI session started (IOConsoleUsers,
	// hidd), which is also when the kernel's verbose console stops drawing and WindowServer takes the screen; with only
	// display-less AMD framebuffers, nothing would replace the last console image, and our heartbeat (printed through
	// that console) would vanish too. A panic screen (debug=0x100) and the panic text in NVRAM prove the kernel was
	// still running; no panic means a real hang.
	uint32_t panicAt = 0;
	if (!PE_parse_boot_argn("crpanicat", &panicAt, sizeof(panicAt)))
		panicAt = 0;
	uint32_t printed = 0;
	last[0] = '\0';
	// Tick every 500 ms for 30 minutes.
	for (uint32_t tick = 0, t = 0; t <= 1800; tick++, t = tick / 2) {
		// New trace events (written lock-free by the wrappers).
		uint32_t head = __atomic_load_n(&traceHead, __ATOMIC_RELAXED);
		if (head - printed > TraceRingSize) {
			IOLog("CarrizoRed: (%u trace events dropped)\n", head - printed - static_cast<uint32_t>(TraceRingSize));
			printed = head - static_cast<uint32_t>(TraceRingSize);
		}
		for (; printed != head; printed++) {
			auto &event = traceRing[printed % TraceRingSize];
			if (event.exit)
				IOLog("CarrizoRed: <- %s = 0x%x\n", event.name, event.value);
			else
				IOLog("CarrizoRed: -> %s\n", event.name);
		}

		// Interrupt activity since the last tick: a storm shows as a fast-climbing handler count.
		{
			static uint32_t lastCalls;
			uint32_t calls = __atomic_load_n(&ihHandlerCalls, __ATOMIC_RELAXED);
			if (calls != lastCalls)
				IOLog("CarrizoRed: IH handler calls %u (+%u), rptr writes %u, last rptr 0x%x\n", calls, calls - lastCalls,
					__atomic_load_n(&ihReadPointerWrites, __ATOMIC_RELAXED), ihLastReadPointer);
			lastCalls = calls;
		}

		if (ihStormBroken && tick % 6 == 0) {
			auto &snap = ihSnapshot;
			IOLog("CarrizoRed: IH STORM broken at %llu ms after %u handler calls; ring in %s, size 0x%x\n", snap.ms,
				snap.calls, snap.vram ? "VRAM" : "system memory", snap.size);
			IOLog("CarrizoRed: IH RB_CNTL=%08x RB_BASE=%08x RPTR=%08x WPTR=%08x WPTR_ADDR=%02x:%08x CNTL=%08x STATUS=%08x\n",
				snap.rbCntl, snap.rbBase, snap.rptr, snap.wptr, snap.wptrAddrHi, snap.wptrAddrLo, snap.cntl, snap.status);
			IOLog("CarrizoRed: IH write-back as CPU sees it=%08x at %llx; ring cpu %llx gpu %llx\n", snap.wbValue,
				snap.wbAddress, snap.ringCpu, snap.ringGpu);
		}

		// Register-log lines queued from interrupts-off context.
		{
			static uint32_t regPrinted;
			uint32_t regHead = __atomic_load_n(&regRingHead, __ATOMIC_RELAXED);
			if (regHead - regPrinted > RegRingSize) {
				IOLog("CarrizoRed: (%u queued register lines dropped)\n",
					regHead - regPrinted - static_cast<uint32_t>(RegRingSize));
				regPrinted = regHead - static_cast<uint32_t>(RegRingSize);
			}
			for (; regPrinted != regHead; regPrinted++)
				printRegEvent(regRing[regPrinted % RegRingSize], " (ints off, queued)");
		}

		// Display/MC register changes since the last tick.
		if (gpuMmio)
			self->checkDisplayRegs(t);

		// Heartbeat once the register log is armed: shows whether register traffic continues up to a freeze.
		if (regLogFromMs && tick % 4 == 0)
			IOLog("CarrizoRed: hb t=%us regs %u ih %u missing-pipe %u pm %u cfgwr %u\n", t,
				__atomic_load_n(&regAccesses, __ATOMIC_RELAXED), __atomic_load_n(&ihHandlerCalls, __ATOMIC_RELAXED),
				__atomic_load_n(&pipeAccesses, __ATOMIC_RELAXED), __atomic_load_n(&pmEvents, __ATOMIC_RELAXED),
				__atomic_load_n(&pciConfigWrites, __ATOMIC_RELAXED));

		if (panicAt && t >= panicAt)
			self->panicReport(t, now);

		// Watchdog: a traced call that never returns (a hard hang with this CPU still alive) becomes a panic that
		// names it and stays on screen (debug=0x100). -crnowatchdog disables this.
		if (watchdog) {
			uint64_t ms = uptimeMs();
			for (size_t i = 0; i < MaxTraceStages; i++) {
				if (__atomic_load_n(&stageOpen[i], __ATOMIC_RELAXED) && ms - stageEnterMs[i] > 10000)
					panic("CarrizoRed watchdog: %s entered %llu ms ago and never returned (trace: %s)", stageNames[i],
						ms - stageEnterMs[i], traceSummary);
			}
		}

		if (tick % 2) {
			IOSleep(500);
			continue;
		}

		now[0] = '\0';
		describeChildren(self->iGPU, 0, now, sizeof(now));

		// AMD9500Controller's personality only matches once the iGPU has LoadController=true. On a Mac the graphics
		// policy asks the wrangler for that (AtiGpuWrangler::vendor_doDeviceAttribute, attribute 3, flag bit 0: set
		// LoadAMDSupport + LoadController, then registerService()); nothing asks for the iGPU, so the wrangler holds the
		// framebuffer slot and the GUI has no display. Do the same once the wrangler has attached. Acceleration
		// (flag bit 1: LoadHWServices + LoadAccelerator) is left for later. -crnoloadctl disables this.
		if (!controllerRequested && strstr(now, "AMD9500ControllerWrangler")) {
			controllerRequested = true;
			if (!self->iGPU->getProperty("LoadController")) {
				self->iGPU->setProperty("LoadAMDSupport", true);
				self->iGPU->setProperty("LoadController", true);
				self->iGPU->registerService();
				IOLog("CarrizoRed: t=%us set LoadController on the iGPU and re-registered it\n", t);
				traceNote("LoadController@%us ", t);
			}
		}

		// Every 15 s, or when the tree changes: launchd keeps scrolling the console once the GUI starts.
		if (print && (strcmp(now, last) || t % 15 == 0)) {
			strlcpy(last, now, sizeof(last));
			self->iGPU->setProperty("CarrizoRed,tree", now);
			IOLog("CarrizoRed: t=%us LoadController=%s, iGPU children:%s\n", t,
				self->iGPU->getProperty("LoadController") ? "set" : "not set", now[0] ? "" : " none");
			// Wrap the summary at spaces so a long line can't be overwritten by the next console line.
			if (!traceSummary[0])
				IOLog("CarrizoRed: trace: (controller not probed yet)\n");
			for (size_t pos = 0, len = strlen(traceSummary); pos < len;) {
				size_t chunk = len - pos > 100 ? 100 : len - pos;
				if (pos + chunk < len)
					while (chunk > 1 && traceSummary[pos + chunk - 1] != ' ')
						chunk--;
				IOLog("CarrizoRed: trace: %.*s\n", static_cast<int>(chunk), traceSummary + pos);
				pos += chunk;
			}
			for (char *line = now; *line;) {
				char *end = strchr(line, '\n');
				if (end)
					*end = '\0';
				IOLog("CarrizoRed:   %s\n", line);
				if (!end)
					break;
				line = end + 1;
			}
		}
		IOSleep(500);
	}
}

// ATIController::getConfigProperty(name, value, size) reads the controller's "aty_config" options. Apple leaves
// CFG_NON_ZERO_MC_LOC off for Polaris, whose VRAM starts at GPU address 0, so AMD9500Controller sets "ATY,vram_base" to 0
// and scans out from the wrong place (first spoof boot: purple lines on the panel). On the APU the carve-out sits at
// MC_VM_FB_LOCATION (0xF4_0000_0000 on the D509BA); with the flag set, Apple takes the base from
// AtiAsicInfo::getFbLocationBase() instead. -crnomcloc disables this.
void CRDriver::patchSupport(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size) {
	{
		KernelPatcher::RouteRequest request {"__ZN13ATIController17getConfigPropertyEPKcPvj", wrapGetConfigProperty,
			orgGetConfigProperty};
		if (routeChecked(patcher, index, request, slide, size)) {
			SYSLOG("cr", "aty_config overrides active");
		} else {
			SYSLOG("cr", "failed to route ATIController::getConfigProperty (error %d)", patcher.getError());
			patcher.clearError();
		}
	}

	// AMD9500Controller::powerUpHardware() runs ATIController::TestVRAM(region 7, false) and fails start-up with
	// 0xe00002bc when it returns false, which it does on Stoney (spoof boot 4). The APU's "VRAM" is a carve-out of
	// system RAM that Linux scans out from fine, so let start-up continue, but keep the real result in the trace.
	// -crnovramtest disables this.
	if (!checkKernelArgument("-crnovramtest")) {
		KernelPatcher::RouteRequest request {"__ZN13ATIController8TestVRAME13PCI_REG_INDEXb", wrapTestVram, orgTestVram};
		if (!routeChecked(patcher, index, request, slide, size)) {
			SYSLOG("cr", "failed to route ATIController::TestVRAM (error %d)", patcher.getError());
			patcher.clearError();
		}
	}
}

// -crnovramwrite: don't run the test at all (it writes patterns into the start of the carve-out through BAR0), to rule
// its writes out as the cause of the freeze ~20 s after start-up.
bool CRDriver::wrapTestVram(void *that, uint32_t region, bool flag) {
	if (checkKernelArgument("-crnovramwrite")) {
		IOLog("CarrizoRed: TestVRAM(region %u) skipped (-crnovramwrite), reporting success\n", region);
		traceNote("TestVRAM(%u)=skipped ", region);
		return true;
	}
	bool ret = FunctionCast(wrapTestVram, callback->orgTestVram)(that, region, flag);
	IOLog("CarrizoRed: TestVRAM(region %u) -> %s, reporting success\n", region, ret ? "passed" : "FAILED");
	traceNote("TestVRAM(%u)=%s->ok ", region, ret ? "pass" : "FAIL");
	return true;
}

// aty_config options CarrizoRed overrides (all booleans), each with a boot-arg that leaves Apple's value alone.
//  CFG_NON_ZERO_MC_LOC: see patchSupport.
//  CFG_NO_PP: no PowerPlay. Apple's PowerPlay drives the Polaris SMU7 power controller; Stoney has an SMU8, which
//  also controls the CPU and memory clocks. Spoof boots 5-7 froze the whole machine instantly as the GUI (and AGPM's
//  power management) started, outside every traced display function.
//  CFG_USE_STUTTER, CFG_USE_FEDS, CFG_USE_FBWRKLP: off. Apple tunes display stutter (memory self-refresh), FEDS and
//  the framebuffer work loop for a Polaris with its own GDDR5; on the APU the display shares DDR4 and the NB P-state
//  logic with the CPU, and Apple's watermark inputs come from a dGPU VRAM table the APU VBIOS doesn't have. Spoof
//  boots 10-12 froze ~20 s after start-up with no CPU register access, as a hardware-triggered hang would.
//  -crstutter, -crfeds, -crfbwrklp each restore Apple's value, to bisect.
//  CFG_NO_MSI (opt-in with -crnomsi): legacy interrupts instead of MSI for the display controller.
//  CFG_NO_AUDIO (opt-in with -crnoaudio): no HDMI/DP audio setup.
//  CFG_USE_TCON (opt-in with -crnotcon): forced off.
static const struct {
	const char *name;
	uint8_t value;
	const char *arg;
	bool optIn;  // apply only with arg (else: apply unless arg)
} configOverrides[] {
	{"CFG_NON_ZERO_MC_LOC", 1, "-crnomcloc", false},
	{"CFG_NO_PP", 1, "-crpp", false},
	{"CFG_USE_STUTTER", 0, "-crstutter", false},
	{"CFG_USE_FEDS", 0, "-crfeds", false},
	{"CFG_USE_FBWRKLP", 0, "-crfbwrklp", false},
	{"CFG_NO_MSI", 1, "-crnomsi", true},
	{"CFG_NO_AUDIO", 1, "-crnoaudio", true},
	{"CFG_USE_TCON", 0, "-crnotcon", true},
};

// Returns non-zero when the option was found; an overridden option counts as found.
uint64_t CRDriver::wrapGetConfigProperty(void *that, const char *name, void *value, uint32_t size) {
	auto ret = FunctionCast(wrapGetConfigProperty, callback->orgGetConfigProperty)(that, name, value, size);
	if (!name || !value || !size)
		return ret;
	for (auto &option : configOverrides) {
		if (!strcmp(name, option.name) && checkKernelArgument(option.arg) == option.optIn) {
			memset(value, 0, size);
			*static_cast<uint8_t *>(value) = option.value;
			traceEvent(option.name, true, option.value);
			if (!ret)
				ret = 1;
		}
	}
	// CFG_FB_LIMIT caps the framebuffer count ATIController::start uses (otherwise 6). Each framebuffer index is also a
	// CRTC index (cleanUpConnections → setDisplayEnabled → AMD9500Controller::shutDownDisplayHardware(index)), but with
	// the pipe limit only CRTCs 0..pipes-1 have timing generators: spoof boot 20 panicked "SiDisplay.cpp:1119 Assertion
	// failed: NULL != timingGenerator" for CRTC 2 when WindowServer opened a framebuffer. Limit framebuffers to the pipe
	// count too (-crnofblimit leaves Apple's value).
	if (!strcmp(name, "CFG_FB_LIMIT") && !checkKernelArgument("-crnofblimit") && !checkKernelArgument("-crnopipes")) {
		memset(value, 0, size);
		*static_cast<uint8_t *>(value) = displayPipes(callback->family);
		traceEvent("CFG_FB_LIMIT", true, displayPipes(callback->family));
		if (!ret)
			ret = 1;
	}
	return ret;
}

// Bring-up tracing (-crnotrace disables): log entry and result of ATIController::probe/start and of each
// AMD9500Controller initialisation stage on the console, so a controller that matches and then fails to start (and
// disappears again) shows which stage failed.
namespace {
	enum TraceKext { InController, InSupport, InFramebuffer };

	struct TracedStage {
		const char *symbol;
		const char *name;
		TraceKext kext;
		bool summary;  // note the result in the repeated trace summary (start-up stages)
		bool sync;     // thread context only: also print on entry and pause, so an instant hard hang still shows it
		mach_vm_address_t org;
	};

	TracedStage stages[] {
		// AMD9500Controller start-up (ATIController::start).
		{"__ZN17AMD9500Controller21initializeProjectInfoEv", "initializeProjectInfo", InController, true, true, 0},
		{"__ZN17AMD9500Controller19initializeResourcesEv", "initializeResources", InController, true, true, 0},
		{"__ZN17AMD9500Controller35initializeProjectDependentResourcesEv", "initializeProjectDependentResources", InController, true, true, 0},
		{"__ZN17AMD9500Controller18initializeHardwareEv", "initializeHardware", InController, true, true, 0},
		{"__ZN17AMD9500Controller19initializePowerPlayEv", "initializePowerPlay", InController, true, true, 0},
		{"__ZN17AMD9500Controller24initializeConnectionInfoEv", "initializeConnectionInfo", InController, true, true, 0},
		{"__ZN17AMD9500Controller13initializeICMEv", "initializeICM", InController, true, true, 0},
		{"__ZN17AMD9500Controller15powerUpHardwareEv", "powerUpHardware", InController, true, true, 0},
		{"__ZN17AMD9500Controller15detectPowerDownEv", "detectPowerDown", InController, true, false, 0},
		{"__ZN17ASIC_INFO__BAFFIN18populateDeviceInfoEv", "populateDeviceInfo", InController, true, false, 0},
		{"__ZN17ASIC_INFO__BAFFIN18populateFbLocationEv", "populateFbLocation", InController, true, false, 0},
		{"__ZN17ASIC_INFO__BAFFIN20populateMemoryConfigEv", "populateMemoryConfig", InController, true, false, 0},
		{"__ZN13AtomBiosProxy14initializeAsicEv", "initializeAsic", InSupport, true, true, 0},
		{"__ZN11AtiAsicInfo18initWithControllerEP13ATIController", "AsicInfo.initWithController", InSupport, true, false, 0},
		// Hardware programming that (judging by spoof boots 5-6) runs when WindowServer first opens the display; the
		// whole machine froze instantly there, as if a shared block (memory controller, SMU) was misprogrammed.
		{"__ZN17AMD9500Controller18hwInitializeFbBaseEv", "hwInitializeFbBase", InController, true, true, 0},
		{"__ZN17AMD9500Controller21hwInitializeFbMemSizeEv", "hwInitializeFbMemSize", InController, true, true, 0},
		{"__ZN17AMD9500Controller16hwInitializeDCLKEv", "hwInitializeDCLK", InController, true, true, 0},
		{"__ZN17AMD9500Controller28hwInitializeHardwareSettingsEv", "hwInitializeHardwareSettings", InController, true, true, 0},
		{"__ZN22BaffinSharedController19hwInitializeDisplayEv", "hwInitializeDisplay", InController, true, true, 0},
		{"__ZN17AMD9500Controller29hwInitializeDisplayBoundariesEh", "hwInitializeDisplayBoundaries", InController, false, true, 0},
		// Display path driven by WindowServer.
		{"__ZN14AMDFramebuffer4openEv", "fb.open", InFramebuffer, false, true, 0},
		{"__ZN14AMDFramebuffer16enableControllerEv", "fb.enableController", InFramebuffer, false, true, 0},
		{"__ZN14AMDFramebuffer14setDisplayModeEii", "fb.setDisplayMode", InFramebuffer, false, true, 0},
		{"__ZN14AMDFramebuffer15doSetPowerStateEj", "fb.doSetPowerState", InFramebuffer, false, true, 0},
		{"__ZN14AMDFramebuffer18setPanelPowerStateEj", "fb.setPanelPowerState", InFramebuffer, false, true, 0},
		{"__ZN14AMDFramebuffer19setOutputPowerStateEj", "fb.setOutputPowerState", InFramebuffer, false, true, 0},
		{"__ZN14AMDFramebuffer19setSystemPowerStateEj", "fb.setSystemPowerState", InFramebuffer, false, true, 0},
		// AGPM drives these.
		{"__ZN14AMDFramebuffer13setPowerLevelEj", "fb.setPowerLevel", InFramebuffer, false, true, 0},
		{"__ZN14AMDFramebuffer13setPowerLimitEj", "fb.setPowerLimit", InFramebuffer, false, true, 0},
		{"__ZN14AMDFramebuffer12setAttributeEjm", "fb.setAttribute", InFramebuffer, false, false, 0},
		{"__ZN14AMDFramebuffer25setAttributeForConnectionEijm", "fb.setAttributeForConnection", InFramebuffer, false, false, 0},
		{"__ZN14AMDFramebuffer12connectFlagsEiiPj", "fb.connectFlags", InFramebuffer, false, false, 0},
		{"__ZN14AMDFramebuffer21getStartupDisplayModeEPiS0_", "fb.getStartupDisplayMode", InFramebuffer, false, false, 0},
		{"__ZN14AMDFramebuffer13setGammaTableEjjjPv", "fb.setGammaTable", InFramebuffer, false, false, 0},
		{"__ZN18AtiTimingGenerator20waitForVerticalBlankEb", "tg.waitForVerticalBlank", InSupport, false, false, 0},
		{"__ZN18AtiTimingGenerator13waitForActiveEv", "tg.waitForActive", InSupport, false, false, 0},
		{"__ZN18AtiTimingGenerator21waitForGraphicsUpdateEv", "tg.waitForGraphicsUpdate", InSupport, false, false, 0},
		{"__ZN18AtiTimingGenerator11setupTimingEP28AtiDetailedTimingInformation", "tg.setupTiming", InSupport, false, false, 0},
		{"__ZN21BaffinTimingGenerator17waitForCrtcUpdateEv", "tg.waitForCrtcUpdate", InController, false, false, 0},
		{"__ZN21BaffinTimingGenerator10enableCrtcEbb", "tg.enableCrtc", InController, false, false, 0},
		{"__ZN21BaffinTimingGenerator9blankCrtcEbj", "tg.blankCrtc", InController, false, false, 0},
		{"__ZN21BaffinTimingGenerator13programTimingEv", "tg.programTiming", InController, false, false, 0},
		{"__ZN17AMD9500Controller19hwProgramPixelClockEhj", "hwProgramPixelClock", InController, false, false, 0},
		// ACPI display state and the Mac display MUX: on this laptop these would run the ASUS firmware's ACPI methods.
		{"__ZN13ATIController15setDisplayStateEP20IOACPIPlatformDevicej", "setDisplayState(acpi)", InSupport, true, true, 0},
		{"__ZN13ATIController20initializeMUXControlEv", "initializeMUXControl", InSupport, true, true, 0},
		{"__ZN13ATIController14setAppleRGBMUXEb", "setAppleRGBMUX", InSupport, true, true, 0},
	};

	// Six integer/pointer arguments cover every traced function. Events go to the ring (see traceEvent); the watcher
	// prints them, and panics naming the function if a call hasn't returned after 10 s. "sync" stages also print
	// directly and pause, because an instant whole-machine hang leaves the watcher no chance.
	template <size_t N>
	uint64_t wrapStage(void *a, void *b, void *c, void *d, void *e, void *f) {
		static_assert(N < MaxTraceStages, "too many traced stages");
		stageNames[N] = stages[N].name;
		stageEnterMs[N] = uptimeMs();
		__atomic_fetch_add(&stageOpen[N], 1, __ATOMIC_RELAXED);
		traceEvent(stages[N].name, false, 0);
		if (stages[N].sync) {
			IOLog("CarrizoRed: => %s\n", stages[N].name);
			IOSleep(30);
		}
		auto ret = FunctionCast(wrapStage<N>, stages[N].org)(a, b, c, d, e, f);
		traceEvent(stages[N].name, true, static_cast<uint32_t>(ret));
		__atomic_fetch_sub(&stageOpen[N], 1, __ATOMIC_RELAXED);
		if (stages[N].summary)
			traceNote("%s=%x ", stages[N].name, static_cast<uint32_t>(ret));
		return ret;
	}

#define CR_WRAP(n) reinterpret_cast<mach_vm_address_t>(wrapStage<n>)
	void routeStages(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size, TraceKext kext) {
		static_assert(arrsize(stages) == 46, "one wrapper per stage");
		mach_vm_address_t wrappers[] {
			CR_WRAP(0), CR_WRAP(1), CR_WRAP(2), CR_WRAP(3), CR_WRAP(4), CR_WRAP(5), CR_WRAP(6), CR_WRAP(7),
			CR_WRAP(8), CR_WRAP(9), CR_WRAP(10), CR_WRAP(11), CR_WRAP(12), CR_WRAP(13), CR_WRAP(14), CR_WRAP(15),
			CR_WRAP(16), CR_WRAP(17), CR_WRAP(18), CR_WRAP(19), CR_WRAP(20), CR_WRAP(21), CR_WRAP(22), CR_WRAP(23),
			CR_WRAP(24), CR_WRAP(25), CR_WRAP(26), CR_WRAP(27), CR_WRAP(28), CR_WRAP(29), CR_WRAP(30), CR_WRAP(31),
			CR_WRAP(32), CR_WRAP(33), CR_WRAP(34), CR_WRAP(35), CR_WRAP(36), CR_WRAP(37), CR_WRAP(38), CR_WRAP(39),
			CR_WRAP(40), CR_WRAP(41), CR_WRAP(42), CR_WRAP(43), CR_WRAP(44), CR_WRAP(45),
		};
		for (size_t i = 0; i < arrsize(stages); i++) {
			if (stages[i].kext != kext)
				continue;
			KernelPatcher::RouteRequest request {stages[i].symbol, wrappers[i], stages[i].org};
			if (!routeChecked(patcher, index, request, slide, size)) {
				SYSLOG("cr", "trace: failed to route %s (error %d)", stages[i].name, patcher.getError());
				patcher.clearError();
			}
		}
	}
#undef CR_WRAP
}

void CRDriver::traceController(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size) {
	if (!checkKernelArgument("-crnotrace"))
		routeStages(patcher, index, slide, size, InController);
}

void CRDriver::traceFramebuffer(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size) {
	if (checkKernelArgument("-crnotrace"))
		return;
	routeStages(patcher, index, slide, size, InFramebuffer);
	KernelPatcher::RouteRequest request {"__ZN14AMDFramebuffer16getApertureRangeEi", wrapGetApertureRange,
		orgGetApertureRange};
	if (!routeChecked(patcher, index, request, slide, size)) {
		SYSLOG("cr", "trace: failed to route getApertureRange (error %d)", patcher.getError());
		patcher.clearError();
	}
}

// The physical range IOFramebuffer and WindowServer map to draw. Spoof boots 5-10 froze the whole machine as the GUI
// started, outside every traced function and with the IH never enabled; a CPU access to a physical address nothing
// decodes (e.g. one built from ATY,vram_base = the GPU-side 0xF4_0000_0000 instead of BAR0 at 0xE000_0000) would do
// that. Print it before handing it out (thread context).
IODeviceMemory *CRDriver::wrapGetApertureRange(void *that, int aperture) {
	auto *mem = FunctionCast(wrapGetApertureRange, callback->orgGetApertureRange)(that, aperture);
	if (mem)
		IOLog("CarrizoRed: fb.getApertureRange(%d) -> phys 0x%llx length 0x%llx\n", aperture,
			static_cast<uint64_t>(mem->getPhysicalAddress()), static_cast<uint64_t>(mem->getLength()));
	else
		IOLog("CarrizoRed: fb.getApertureRange(%d) -> none\n", aperture);
	traceEvent("fb.getApertureRange", true, mem ? static_cast<uint32_t>(mem->getPhysicalAddress() >> 20) : 0);
	IOSleep(30);
	return mem;
}

void CRDriver::traceSupport(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size) {
	if (!checkKernelArgument("-crnoreglog")) {
		KernelPatcher::RouteRequest regRequests[] {
			{"__ZN16ARegisterService15readIndirectRegEjjj", wrapIndRead, orgIndRead},
			{"__ZN16ARegisterService16writeIndirectRegEjjjj", wrapIndWrite, orgIndWrite},
		};
		for (auto &request : regRequests) {
			if (!routeChecked(patcher, index, request, slide, size)) {
				SYSLOG("cr", "reglog: failed to route %s (error %d)", request.symbol, patcher.getError());
				patcher.clearError();
			}
		}
	}
	// ATIController::probe/start are also needed by -crnoctl and -crreglogall.
	bool trace = !checkKernelArgument("-crnotrace");
	if (!trace && !checkKernelArgument("-crnoctl") && !regLogAll)
		return;
	if (trace)
		routeStages(patcher, index, slide, size, InSupport);
	KernelPatcher::RouteRequest requests[] {
		{"__ZN13ATIController5probeEP9IOServicePi", wrapAtiProbe, orgAtiProbe},
		{"__ZN13ATIController5startEP9IOService", wrapAtiStart, orgAtiStart},
	};
	for (auto &request : requests) {
		if (!routeChecked(patcher, index, request, slide, size)) {
			SYSLOG("cr", "trace: failed to route %s (error %d)", request.symbol, patcher.getError());
			patcher.clearError();
		}
	}
}

// -crnoctl: AMD9500Controller (any ATIController) never matches, while the spoof, the wrangler, AMDSupport and AGDP
// still load. Tells whether the controller's start-up is what leads to the freeze ~20 s later.
// -crreglogall: register logging starts here (every access, with values), instead of 2 s after start.
IOService *CRDriver::wrapAtiProbe(IOService *that, IOService *provider, SInt32 *score) {
	if (checkKernelArgument("-crnoctl")) {
		IOLog("CarrizoRed: %s::probe -> NO MATCH (-crnoctl)\n", that->getMetaClass()->getClassName());
		traceNote("%s.probe=blocked ", that->getMetaClass()->getClassName());
		return nullptr;
	}
	if (regLogAll && !regLogFromMs) {
		regLogFromMs = uptimeMs();
		IOLog("CarrizoRed: logging every register access from probe (-crreglogall)\n");
	}
	IOLog("CarrizoRed: %s::probe ...\n", that->getMetaClass()->getClassName());
	auto *ret = FunctionCast(wrapAtiProbe, callback->orgAtiProbe)(that, provider, score);
	IOLog("CarrizoRed: %s::probe -> %s\n", that->getMetaClass()->getClassName(), ret ? "match" : "NO MATCH");
	traceNote("%s.probe=%s ", that->getMetaClass()->getClassName(), ret ? "match" : "NO");
	// ATIController::start can't be wrapped safely (see routeChecked), so the first-touch register log opens a fixed
	// time after a successful probe instead of 2 s after start returns (start takes about 1 s).
	if (ret && !regLogFromMs) {
		regLogFromMs = uptimeMs() + 5000;
		IOLog("CarrizoRed: logging registers first touched from probe + 5 s\n");
	}
	return ret;
}

bool CRDriver::wrapAtiStart(IOService *that, IOService *provider) {
	IOLog("CarrizoRed: %s::start ...\n", that->getMetaClass()->getClassName());
	bool ret = FunctionCast(wrapAtiStart, callback->orgAtiStart)(that, provider);
	IOLog("CarrizoRed: %s::start -> %s\n", that->getMetaClass()->getClassName(), ret ? "OK" : "FAILED");
	traceNote("%s.start=%s ", that->getMetaClass()->getClassName(), ret ? "OK" : "FAILED");
	if (ret && !regLogFromMs) {
		regLogFromMs = uptimeMs() + 2000;
		IOLog("CarrizoRed: logging registers first touched from now + 2 s\n");
	}
	return ret;
}

// -crnoaccel: keep Apple's acceleration stack (AMDRadeonX4000 accelerator, HWServices/CAIL) off the iGPU so the display
// controller (AMD9500Controller) can be brought up on its own. Their probe() reports no match.
// Keep a driver off by making its probe() report no match, when the given boot-arg is present:
//  -crnoaccel: AMDRadeonX4000 accelerator and HWServices/CAIL, so the display controller comes up on its own.
//  -crnoagpm: AGPM (AGPMController on the platform device, AGPM per GPU), Apple's GPU power management.
// Spoof boots 6-8 froze the whole machine instantly as the GUI started, outside every traced display function; AGPM,
// AGDP (-crnoagdp, see processKext) and MSI (-crnomsi) all become active then, so they can be removed one at a time.
void CRDriver::blockProbe(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size, const char *probe,
	const char *arg) {
	if (!checkKernelArgument(arg))
		return;
	KernelPatcher::RouteRequest request {probe, wrapProbeNoMatch};
	if (routeChecked(patcher, index, request, slide, size)) {
		SYSLOG("cr", "%s: %s will not match", arg, probe);
	} else {
		SYSLOG("cr", "%s: failed to route %s (error %d)", arg, probe, patcher.getError());
		patcher.clearError();
	}
}

bool CRDriver::wrapStartFail(IOService *, IOService *) { return false; }

// -crmacgpu: make the iGPU look like a Mac's own GPU to Apple's policy code.
// AGDP picks a per-board configuration from ConfigMap by the platform's "board-id". For the iMac19,1 SMBIOS that is
// Config3 (GFX0: EDID override index 0, FeatureControl 12; IGPU: unload), written for that iMac's panel and GPUs.
// Renaming the property it reads (the only "board-id" string in the binary, Sequoia 24H32) to "board-ix" makes the
// lookup fail, so AGDP uses its Default config ("none"). Same approach as WhateverGreen's agdpmod=pikera.
void CRDriver::patchAgdpBoardId(KernelPatcher &patcher, size_t kext) {
	static const uint8_t find[] {'b', 'o', 'a', 'r', 'd', '-', 'i', 'd', 0};
	static const uint8_t replace[] {'b', 'o', 'a', 'r', 'd', '-', 'i', 'x', 0};
	KernelPatcher::LookupPatch patch {&kexts[kext], find, replace, sizeof(find), 1};
	patcher.applyLookupPatch(&patch);
	if (patcher.getError() == KernelPatcher::Error::NoError) {
		SYSLOG("cr", "-crmacgpu: AGDP board-id lookup neutralised");
	} else {
		SYSLOG("cr", "-crmacgpu: AGDP board-id patch failed (error %d)", patcher.getError());
		patcher.clearError();
	}
}

// -crmacgpu: the properties a Mac's own GPU has (the iGPU has none): "built-in" and AAPL,slot-name "built-in", so
// Apple's code doesn't treat it as removable (the controller personality is IOPCITunnelCompatible and ATIController
// has eGPU paths), and a "model". The device keeps its ACPI name (VGA): AGDP and AGPM attach per-name policies to
// GFX0/IGPU.
void CRDriver::setMacGpuProperties() {
	uint8_t builtIn[] {0x00};
	iGPU->setProperty("built-in", builtIn, sizeof(builtIn));
	char slotName[] = "built-in";
	iGPU->setProperty("AAPL,slot-name", slotName, sizeof(slotName));
	const char *model = family == ChipFamily::Stoney ? "AMD Radeon R5 Graphics" : "AMD Radeon R7 Graphics";
	iGPU->setProperty("model", const_cast<char *>(model), static_cast<unsigned>(strlen(model) + 1));
	SYSLOG("cr", "-crmacgpu: built-in, AAPL,slot-name and model (%s) set", model);
}

// Display interrupts go through the IH ring, managed by BaffinInterruptManager. Polaris (Tonga IH) can acknowledge
// entries with a doorbell; Carrizo/Stoney (CZ IH) only through IH_RB_RPTR (Linux tonga_ih_set_rptr vs
// cz_ih_set_rptr). An entry that is never acknowledged keeps the interrupt asserted: spoof boot 9 (legacy INTx) froze
// as soon as the controller loaded, boots 5-8 (MSI) when the GUI started. Count handler calls and read-pointer
// updates for the watcher, and with -crnoih keep the IH disabled entirely to confirm it.
void CRDriver::patchInterrupts(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size) {
	KernelPatcher::RouteRequest requests[] {
		{"__ZN22BaffinInterruptManager18setHardwareEnabledEb", wrapIhSetHardwareEnabled, orgIhSetHardwareEnabled},
		{"__ZN22BaffinInterruptManager7handlerEP8OSObjectPvP9IOServicei", wrapIhHandler, orgIhHandler},
		{"__ZN22BaffinInterruptManager16setRBReadPointerEj", wrapIhSetReadPointer, orgIhSetReadPointer},
	};
	for (auto &request : requests) {
		if (!routeChecked(patcher, index, request, slide, size)) {
			SYSLOG("cr", "interrupts: failed to route %s (error %d)", request.symbol, patcher.getError());
			patcher.clearError();
		}
	}
	stormBreak = !checkKernelArgument("-crnostormbreak");
	if (!checkKernelArgument("-crnoreglog")) {
		KernelPatcher::RouteRequest regRequests[] {
			{"__ZN21BaffinRegisterService9readReg32Ej", wrapRegRead32, orgRegRead32},
			{"__ZN21BaffinRegisterService10writeReg32Ejj", wrapRegWrite32, orgRegWrite32},
			{"__ZN21BaffinRegisterService10readPCIe32Ej", wrapPcieRead32, orgPcieRead32},
			{"__ZN21BaffinRegisterService11writePCIe32Ejj", wrapPcieWrite32, orgPcieWrite32},
			{"__ZN21BaffinRegisterService11hwReadReg32Ej", wrapHwRead32, orgHwRead32},
			{"__ZN21BaffinRegisterService9readReg16Ej", wrapRead16, orgRead16},
			{"__ZN21BaffinRegisterService8readReg8Ej", wrapRead8, orgRead8},
			{"__ZN21BaffinRegisterService10writeReg16Ejt", wrapWrite16, orgWrite16},
			{"__ZN21BaffinRegisterService9writeReg8Ejh", wrapWrite8, orgWrite8},
		};
		for (auto &request : regRequests) {
			if (!routeChecked(patcher, index, request, slide, size)) {
				SYSLOG("cr", "reglog: failed to route %s (error %d)", request.symbol, patcher.getError());
				patcher.clearError();
			}
		}
	}
	if (!checkKernelArgument("-crnopcitrace")) {
		KernelPatcher::RouteRequest pciRequests[] {
			{"__ZN21BaffinRegisterService12writePCIReg8Ejh", wrapWritePCIReg8, orgWritePCIReg8},
			{"__ZN21BaffinRegisterService13writePCIReg16Ejt", wrapWritePCIReg16, orgWritePCIReg16},
			{"__ZN21BaffinRegisterService13writePCIReg32Ejj", wrapWritePCIReg32, orgWritePCIReg32},
		};
		for (auto &request : pciRequests) {
			if (!routeChecked(patcher, index, request, slide, size)) {
				SYSLOG("cr", "pci: failed to route %s (error %d)", request.symbol, patcher.getError());
				patcher.clearError();
			}
		}
	}
	ihGetWptrWbAddress = patcher.solveSymbol(index, "__ZN22BaffinInterruptManager23getWPTRWriteBackAddressEv", slide, size);
	ihIsUsingVram = patcher.solveSymbol(index, "__ZN22BaffinInterruptManager24isUsingVRAMForRingBufferEv", slide, size);
	patcher.clearError();
}

static void printRegEvent(const RegEvent &event, const char *suffix) {
	bool write = event.op == RegWrite || event.op == RegPcieWrite || event.op == RegIndWrite;
	char value[16] = "";
	if (write)
		snprintf(value, sizeof(value), " = 0x%x", event.value);
	char pipe[48] = "";
	if (event.pipe >= 0)
		snprintf(pipe, sizeof(pipe), " [PIPE %d, APU has %u%s]", event.pipe, pipeLimit, pipeGuard ? ", blocked" : "");
	IOLog("CarrizoRed: new reg %s 0x%x/0x%x%s%s%s\n", regOpNames[event.op], event.offset, event.extra, value, pipe,
		suffix);
}

// Called before every access. Returns RegBlocked when -crpipeguard blocks it, RegPrinted when a line was printed
// (the read wrappers then print the value read).
enum : int { RegBlocked = 1, RegPrinted = 2 };

static int regAccess(RegOp op, uint32_t offset, uint32_t extra, uint32_t value) {
	__atomic_fetch_add(&regAccesses, 1, __ATOMIC_RELAXED);
	bool direct = op == RegRead || op == RegWrite;
	int pipe = direct ? pipeOfRegister(offset >> 2) : -1;
	bool outside = pipe >= static_cast<int>(pipeLimit);
	if (outside)
		__atomic_fetch_add(&pipeAccesses, 1, __ATOMIC_RELAXED);
	int block = outside && pipeGuard ? RegBlocked : 0;
	uint32_t key = (direct ? offset >> 2 : offset ^ (extra << 4)) & 0xFFFF;
	uint8_t bit = static_cast<uint8_t>(1u << (key & 7));
	uint8_t old = __atomic_fetch_or(&regSeen[op][key >> 3], bit, __ATOMIC_RELAXED);
	// -crreglogall prints every access, not only the first one per offset.
	if ((old & bit) && !regLogAll)
		return block;
	if (outside && !regLogAll) {
		if (__atomic_fetch_add(&pipeLogged, 1, __ATOMIC_RELAXED) >= 100)
			return block;
	} else {
		uint64_t from = regLogFromMs;
		if (!from || uptimeMs() < from || regLogged > 400)
			return block;
		regLogged++;
	}
	RegEvent event {op, offset, extra, value, outside ? pipe : -1};
	if (ml_get_interrupts_enabled() || regLogIntsOff) {
		printRegEvent(event, ml_get_interrupts_enabled() ? "" : " (ints off)");
		IODelay(3000);
		return block | RegPrinted;
	}
	regRing[__atomic_fetch_add(&regRingHead, 1, __ATOMIC_RELAXED) % RegRingSize] = event;
	return block;
}

// Value of a read whose access line was printed (-crreglogall only).
static uint32_t regReadDone(int flags, uint32_t value) {
	if ((flags & RegPrinted) && regLogAll)
		IOLog("CarrizoRed:     = 0x%x\n", value);
	return value;
}

// readReg32 reads through hwReadReg32 (vtable +0x160), so with -crreglogall each such read shows twice; the inner one
// is marked "/0x1".
uint32_t CRDriver::wrapHwRead32(void *that, uint32_t offset) {
	int flags = regAccess(RegRead, offset, 1, 0);
	if (flags & RegBlocked)
		return 0;
	return regReadDone(flags, FunctionCast(wrapHwRead32, callback->orgHwRead32)(that, offset));
}

uint32_t CRDriver::wrapRead16(void *that, uint32_t offset) {
	int flags = regAccess(RegRead, offset & ~3u, 16, 0);
	if (flags & RegBlocked)
		return 0;
	return regReadDone(flags, FunctionCast(wrapRead16, callback->orgRead16)(that, offset));
}

uint32_t CRDriver::wrapRead8(void *that, uint32_t offset) {
	int flags = regAccess(RegRead, offset & ~3u, 8, 0);
	if (flags & RegBlocked)
		return 0;
	return regReadDone(flags, FunctionCast(wrapRead8, callback->orgRead8)(that, offset));
}

void CRDriver::wrapWrite16(void *that, uint32_t offset, uint32_t value) {
	if (!(regAccess(RegWrite, offset & ~3u, 16, value) & RegBlocked))
		FunctionCast(wrapWrite16, callback->orgWrite16)(that, offset, value);
}

void CRDriver::wrapWrite8(void *that, uint32_t offset, uint32_t value) {
	if (!(regAccess(RegWrite, offset & ~3u, 8, value) & RegBlocked))
		FunctionCast(wrapWrite8, callback->orgWrite8)(that, offset, value);
}

uint32_t CRDriver::wrapRegRead32(void *that, uint32_t offset) {
	int flags = regAccess(RegRead, offset, 0, 0);
	if (flags & RegBlocked)
		return 0;
	return regReadDone(flags, FunctionCast(wrapRegRead32, callback->orgRegRead32)(that, offset));
}

void CRDriver::wrapRegWrite32(void *that, uint32_t offset, uint32_t value) {
	if (!(regAccess(RegWrite, offset, 0, value) & RegBlocked))
		FunctionCast(wrapRegWrite32, callback->orgRegWrite32)(that, offset, value);
}

uint32_t CRDriver::wrapPcieRead32(void *that, uint32_t offset) {
	int flags = regAccess(RegPcieRead, offset, 0, 0);
	return regReadDone(flags, FunctionCast(wrapPcieRead32, callback->orgPcieRead32)(that, offset));
}

void CRDriver::wrapPcieWrite32(void *that, uint32_t offset, uint32_t value) {
	regAccess(RegPcieWrite, offset, 0, value);
	FunctionCast(wrapPcieWrite32, callback->orgPcieWrite32)(that, offset, value);
}

uint32_t CRDriver::wrapIndRead(void *that, uint32_t index, uint32_t data, uint32_t offset) {
	int flags = regAccess(RegIndRead, offset, index, 0);
	return regReadDone(flags, FunctionCast(wrapIndRead, callback->orgIndRead)(that, index, data, offset));
}

void CRDriver::wrapIndWrite(void *that, uint32_t index, uint32_t data, uint32_t offset, uint32_t value) {
	regAccess(RegIndWrite, offset, index, value);
	FunctionCast(wrapIndWrite, callback->orgIndWrite)(that, index, data, offset, value);
}

void CRDriver::wrapIhSetHardwareEnabled(void *that, bool enable) {
	bool off = enable && checkKernelArgument("-crnoih");
	traceEvent(off ? "IH enable (forced off by -crnoih)" : enable ? "IH enable" : "IH disable", true, enable);
	FunctionCast(wrapIhSetHardwareEnabled, callback->orgIhSetHardwareEnabled)(that, enable && !off);
}

// Storm breaker: more than 500 handler calls within 100 ms means the IH keeps re-raising an interrupt the driver
// believes it has consumed. Disable the ring and interrupts directly (so the machine survives instead of freezing),
// record the IH state and the write-pointer write-back the CPU sees, and ignore further calls. -crnostormbreak
// disables this.
void CRDriver::wrapIhHandler(void *that, void *owner, void *arg, void *nub, int source) {
	uint32_t calls = __atomic_add_fetch(&ihHandlerCalls, 1, __ATOMIC_RELAXED);
	if (ihStormBroken)
		return;
	uint64_t ms = uptimeMs();
	if (ms - ihWindowStart > 100) {
		ihWindowStart = ms;
		ihWindowCalls = 0;
	}
	if (++ihWindowCalls > 500 && gpuMmio && callback->stormBreak) {
		auto *mmio = gpuMmio;
		ihSnapshot.ms = ms;
		ihSnapshot.calls = calls;
		ihSnapshot.rbCntl = mmio[IhReg::RB_CNTL];
		ihSnapshot.rbBase = mmio[IhReg::RB_BASE];
		ihSnapshot.rptr = mmio[IhReg::RB_RPTR];
		ihSnapshot.wptr = mmio[IhReg::RB_WPTR];
		ihSnapshot.wptrAddrHi = mmio[IhReg::RB_WPTR_ADDR_HI];
		ihSnapshot.wptrAddrLo = mmio[IhReg::RB_WPTR_ADDR_LO];
		ihSnapshot.cntl = mmio[IhReg::CNTL];
		ihSnapshot.status = mmio[IhReg::STATUS];
		if (auto *manager = static_cast<uint8_t *>(ihManager)) {
			// BaffinInterruptManager fields (initializeHardware): +0x3c size, +0x40 ring CPU address, +0x48 GPU address.
			ihSnapshot.size = *reinterpret_cast<uint32_t *>(manager + 0x3C);
			ihSnapshot.ringCpu = *reinterpret_cast<uint64_t *>(manager + 0x40);
			ihSnapshot.ringGpu = *reinterpret_cast<uint64_t *>(manager + 0x48);
			if (ihGetWptrWbAddress) {
				auto *wb = reinterpret_cast<volatile uint32_t *(*)(void *)>(ihGetWptrWbAddress)(manager);
				ihSnapshot.wbAddress = reinterpret_cast<uint64_t>(wb);
				ihSnapshot.wbValue = wb ? *wb : 0xFFFFFFFF;
			}
			if (ihIsUsingVram)
				ihSnapshot.vram = reinterpret_cast<bool (*)(void *)>(ihIsUsingVram)(manager);
		}
		mmio[IhReg::CNTL] = ihSnapshot.cntl & ~1u;      // ENABLE_INTR off
		mmio[IhReg::RB_CNTL] = ihSnapshot.rbCntl & ~1u; // RB_ENABLE off
		ihStormBroken = true;
		return;
	}
	FunctionCast(wrapIhHandler, callback->orgIhHandler)(that, owner, arg, nub, source);
}

void CRDriver::wrapIhSetReadPointer(void *that, uint32_t pointer) {
	__atomic_fetch_add(&ihReadPointerWrites, 1, __ATOMIC_RELAXED);
	ihLastReadPointer = pointer;
	ihManager = that;
	FunctionCast(wrapIhSetReadPointer, callback->orgIhSetReadPointer)(that, pointer);
}

// Power, graphics-policy and PCI config-space tracing. Spoof boot 13 froze ~20 s after AMD9500Controller::start (2-3 s
// after the GUI session started) with Apple's display code idle: 40 register accesses in total, none after start-up,
// no interrupts, no mode set, no framebuffer opened. So the freeze comes from something that reaches the GPU without
// display-register traffic. Candidates traced here, each printed synchronously (with a pause) so the last line before
// a freeze names it:
//  - PCI power management of the iGPU (00:01.0) and its HDMI audio function (00:01.1): IOPCIDevice::setPowerState,
//    IOPCIBridge::setDevicePowerState, IOPCIDevice::setPCIPowerState (which writes PMCSR through
//    extendedConfigWrite32), and every config-space write to them (IOPCIDevice::configWrite*/extendedConfigWrite*);
//  - Apple's GPU policy (AGDP/AGDC) asking AMDSupport for something: AtiDeviceControl::vendor_doDeviceAttribute and
//    AtiGpuWrangler::vendor_doDeviceAttribute (attribute, value, result);
//  - config writes Apple's controller makes itself (BaffinRegisterService::writePCIReg*);
//  - ACPI display state / MUX calls (traced stages setDisplayState, initializeMUXControl, setAppleRGBMUX).
// -crkeepgpuon: refuse to take the iGPU below the PCI "on" state (IOPCIBridge::setDevicePowerState /
// IOPCIDevice::setPCIPowerState with a state below kIOPCIDeviceOnState report success without doing anything).
static constexpr unsigned long PciOnState = 2;  // kIOPCIDeviceOnState (0 off, 1 doze, 2 on, 3 paused)
static bool keepGpuOn;

// Thread context and interrupts enabled: print and pause (sleep allowed only when the caller says so). Otherwise only
// the trace ring (name + value) is safe.
static void pmLog(bool canSleep, const char *tag, uint32_t value, const char *fmt, ...) {
	__atomic_fetch_add(&pmEvents, 1, __ATOMIC_RELAXED);
	if (!ml_get_interrupts_enabled() || ml_at_interrupt_context()) {
		traceEvent(tag, true, value);
		return;
	}
	char line[192];
	va_list va;
	va_start(va, fmt);
	vsnprintf(line, sizeof(line), fmt, va);
	va_end(va);
	IOLog("CarrizoRed: %s\n", line);
	if (canSleep)
		IOSleep(30);
	else
		IODelay(2000);
}

bool CRDriver::isGpuFunction(IOPCIDevice *device) {
	auto *self = callback;
	if (!self || !device || !self->iGPU)
		return false;
	return device == self->iGPU ||
	       (device->getBusNumber() == self->gpuBus && device->getDeviceNumber() == self->gpuDevice);
}

void CRDriver::patchPci(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size) {
	keepGpuOn = checkKernelArgument("-crkeepgpuon");
	if (checkKernelArgument("-crnopcitrace") && !keepGpuOn)
		return;
	KernelPatcher::RouteRequest requests[] {
		{"__ZN11IOPCIDevice13setPowerStateEmP9IOService", wrapPciSetPowerState, orgPciSetPowerState},
		{"__ZN11IOPCIDevice16setPCIPowerStateEhj", wrapPciSetPCIPowerState, orgPciSetPCIPowerState},
		{"__ZN11IOPCIBridge19setDevicePowerStateEP11IOPCIDevicejmm", wrapBridgeSetDevicePowerState,
			orgBridgeSetDevicePowerState},
		{"__ZN11IOPCIDevice12configWrite8Ehh", wrapConfigWrite8, orgConfigWrite8},
		{"__ZN11IOPCIDevice13configWrite16Eht", wrapConfigWrite16, orgConfigWrite16},
		{"__ZN11IOPCIDevice13configWrite32Ehj", wrapConfigWrite32, orgConfigWrite32},
		{"__ZN11IOPCIDevice20extendedConfigWrite8Eyh", wrapExtConfigWrite8, orgExtConfigWrite8},
		{"__ZN11IOPCIDevice21extendedConfigWrite16Eyt", wrapExtConfigWrite16, orgExtConfigWrite16},
		{"__ZN11IOPCIDevice21extendedConfigWrite32Eyj", wrapExtConfigWrite32, orgExtConfigWrite32},
	};
	size_t routed = 0;
	for (auto &request : requests) {
		if (routeChecked(patcher, index, request, slide, size)) {
			routed++;
		} else {
			SYSLOG("cr", "pci: failed to route %s (error %d)", request.symbol, patcher.getError());
			patcher.clearError();
		}
	}
	SYSLOG("cr", "pci: %lu/%lu IOPCIFamily routes%s", routed, arrsize(requests), keepGpuOn ? ", -crkeepgpuon" : "");
}

IOReturn CRDriver::wrapPciSetPowerState(IOPCIDevice *that, unsigned long state, IOService *whatDevice) {
	auto org = FunctionCast(wrapPciSetPowerState, callback->orgPciSetPowerState);
	if (!isGpuFunction(that))
		return org(that, state, whatDevice);
	uint8_t fn = that->getFunctionNumber();
	pmLog(true, "pci.setPowerState", static_cast<uint32_t>(state), "=> PCI fn%u setPowerState(%lu) from %s", fn,
		state, whatDevice ? whatDevice->getMetaClass()->getClassName() : "?");
	auto ret = org(that, state, whatDevice);
	pmLog(true, "pci.setPowerState ret", ret, "<= PCI fn%u setPowerState(%lu) = 0x%x", fn, state, ret);
	return ret;
}

IOReturn CRDriver::wrapPciSetPCIPowerState(IOPCIDevice *that, uint8_t state, uint32_t options) {
	auto org = FunctionCast(wrapPciSetPCIPowerState, callback->orgPciSetPCIPowerState);
	if (!isGpuFunction(that))
		return org(that, state, options);
	uint8_t fn = that->getFunctionNumber();
	if (keepGpuOn && fn == 0 && state < PciOnState) {
		pmLog(false, "pci.setPCIPowerState BLOCKED", state, "PCI fn%u setPCIPowerState(%u, 0x%x) BLOCKED (-crkeepgpuon)",
			fn, state, options);
		return kIOReturnSuccess;
	}
	pmLog(false, "pci.setPCIPowerState", state, "=> PCI fn%u setPCIPowerState(%u, 0x%x)", fn, state, options);
	auto ret = org(that, state, options);
	pmLog(false, "pci.setPCIPowerState ret", ret, "<= PCI fn%u setPCIPowerState(%u) = 0x%x", fn, state, ret);
	return ret;
}

IOReturn CRDriver::wrapBridgeSetDevicePowerState(IOService *that, IOPCIDevice *device, uint32_t options,
	unsigned long prevState, unsigned long newState) {
	auto org = FunctionCast(wrapBridgeSetDevicePowerState, callback->orgBridgeSetDevicePowerState);
	if (!isGpuFunction(device))
		return org(that, device, options, prevState, newState);
	uint8_t fn = device->getFunctionNumber();
	if (keepGpuOn && fn == 0 && newState < PciOnState) {
		pmLog(false, "bridge.setDevicePowerState BLOCKED", static_cast<uint32_t>(newState),
			"PCI fn%u setDevicePowerState(%lu -> %lu, 0x%x) BLOCKED (-crkeepgpuon)", fn, prevState, newState, options);
		return kIOReturnSuccess;
	}
	pmLog(false, "bridge.setDevicePowerState", static_cast<uint32_t>(newState),
		"=> PCI fn%u setDevicePowerState(%lu -> %lu, 0x%x)", fn, prevState, newState, options);
	auto ret = org(that, device, options, prevState, newState);
	pmLog(false, "bridge.setDevicePowerState ret", ret, "<= PCI fn%u setDevicePowerState(-> %lu) = 0x%x", fn, newState,
		ret);
	return ret;
}

// Config-space writes to the iGPU functions: the first 200 are printed, all are counted (heartbeat "cfgwr").
static void configWriteNote(IOPCIDevice *device, uint64_t offset, uint32_t value, unsigned bits) {
	uint32_t n = __atomic_fetch_add(&pciConfigWrites, 1, __ATOMIC_RELAXED);
	if (n >= 200)
		return;
	// The old value (read before the write goes out) shows what actually changes, e.g. whether bus mastering is new.
	uint32_t old = offset < 0x100 ? device->configRead32(static_cast<uint8_t>(offset & ~3ull)) : 0xFFFFFFFF;
	pmLog(false, "pci.configWrite off<<16|fn", static_cast<uint32_t>(offset << 16) | device->getFunctionNumber(),
		"PCI fn%u config write%u 0x%llx = 0x%x (dword was 0x%08x)", device->getFunctionNumber(), bits, offset, value,
		old);
}

void CRDriver::wrapConfigWrite8(IOPCIDevice *that, uint8_t offset, uint8_t value) {
	if (isGpuFunction(that))
		configWriteNote(that, offset, value, 8);
	FunctionCast(wrapConfigWrite8, callback->orgConfigWrite8)(that, offset, value);
}

void CRDriver::wrapConfigWrite16(IOPCIDevice *that, uint8_t offset, uint16_t value) {
	if (isGpuFunction(that))
		configWriteNote(that, offset, value, 16);
	FunctionCast(wrapConfigWrite16, callback->orgConfigWrite16)(that, offset, value);
}

void CRDriver::wrapConfigWrite32(IOPCIDevice *that, uint8_t offset, uint32_t value) {
	if (isGpuFunction(that))
		configWriteNote(that, offset, value, 32);
	FunctionCast(wrapConfigWrite32, callback->orgConfigWrite32)(that, offset, value);
}

void CRDriver::wrapExtConfigWrite8(IOPCIDevice *that, uint64_t offset, uint8_t value) {
	if (isGpuFunction(that))
		configWriteNote(that, offset, value, 8);
	FunctionCast(wrapExtConfigWrite8, callback->orgExtConfigWrite8)(that, offset, value);
}

void CRDriver::wrapExtConfigWrite16(IOPCIDevice *that, uint64_t offset, uint16_t value) {
	if (isGpuFunction(that))
		configWriteNote(that, offset, value, 16);
	FunctionCast(wrapExtConfigWrite16, callback->orgExtConfigWrite16)(that, offset, value);
}

void CRDriver::wrapExtConfigWrite32(IOPCIDevice *that, uint64_t offset, uint32_t value) {
	if (isGpuFunction(that))
		configWriteNote(that, offset, value, 32);
	FunctionCast(wrapExtConfigWrite32, callback->orgExtConfigWrite32)(that, offset, value);
}

// AGDP/AGDC → AMDSupport requests (thread context: user clients and the policy's work loop).
void CRDriver::patchPolicy(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size) {
	{
		KernelPatcher::RouteRequest request {"__ZN21AtiFbInterruptManager28installInterruptsOnPciDeviceEv",
			wrapInstallInterrupts, orgInstallInterrupts};
		if (!routeChecked(patcher, index, request, slide, size)) {
			SYSLOG("cr", "failed to route installInterruptsOnPciDevice (error %d)", patcher.getError());
			patcher.clearError();
		}
	}
	if (checkKernelArgument("-crnotrace"))
		return;
	KernelPatcher::RouteRequest requests[] {
		{"__ZN16AtiDeviceControl24vendor_doDeviceAttributeEjPmmS0_S0_P25IOExternalMethodArguments",
			wrapDeviceControlAttribute, orgDeviceControlAttribute},
		{"__ZN14AtiGpuWrangler24vendor_doDeviceAttributeEjPmmS0_S0_P25IOExternalMethodArguments",
			wrapWranglerAttribute, orgWranglerAttribute},
	};
	for (auto &request : requests) {
		if (!routeChecked(patcher, index, request, slide, size)) {
			SYSLOG("cr", "policy: failed to route %s (error %d)", request.symbol, patcher.getError());
			patcher.clearError();
		}
	}
}

// AtiFbInterruptManager::installInterruptsOnPciDevice() (AMDSupport) is the controller's whole OS-side interrupt
// set-up: pick the MSI index (getInterruptType = kIOInterruptTypePCIMessaged) unless CFG_NO_MSI, registerInterrupt(index,
// manager, BaffinInterruptManager::handler) and enableInterrupt(index). Spoof boot 9 with legacy interrupts froze as
// soon as the controller loaded, MSI boots ~20 s later, although the GPU never delivered an interrupt to the handler.
// -crnoints skips it (no interrupt registered or enabled); otherwise the PCI command register and MSI capability are
// printed afterwards (enable, address, data: where the GPU would write its interrupt messages).
static void printMsiState(IOPCIDevice *device, const char *when) {
	uint16_t command = device->configRead16(0x04);
	uint8_t cap = device->configRead8(0x34) & 0xFC;
	for (int guard = 0; cap && guard < 48; guard++) {
		uint8_t id = device->configRead8(cap);
		if (id == 0x05) {  // MSI
			uint16_t control = device->configRead16(cap + 2);
			uint32_t addressLo = device->configRead32(cap + 4), addressHi = 0;
			uint16_t data;
			if (control & 0x80) {  // 64-bit address
				addressHi = device->configRead32(cap + 8);
				data = device->configRead16(cap + 12);
			} else {
				data = device->configRead16(cap + 8);
			}
			IOLog("CarrizoRed: %s: PCI command 0x%04x, MSI cap 0x%x control 0x%04x (%s) address 0x%08x%08x data 0x%04x\n",
				when, command, cap, control, (control & 1) ? "ENABLED" : "off", addressHi, addressLo, data);
			return;
		}
		cap = device->configRead8(cap + 1) & 0xFC;
	}
	IOLog("CarrizoRed: %s: PCI command 0x%04x, no MSI capability\n", when, command);
}

IOReturn CRDriver::wrapInstallInterrupts(void *that) {
	auto *gpu = callback->iGPU;
	if (checkKernelArgument("-crnoints")) {
		IOLog("CarrizoRed: installInterruptsOnPciDevice skipped (-crnoints): no interrupt registered or enabled\n");
		traceNote("ints=skipped ");
		if (gpu)
			printMsiState(gpu, "-crnoints");
		IOSleep(30);
		return kIOReturnSuccess;
	}
	if (gpu)
		printMsiState(gpu, "before interrupt install");
	auto ret = FunctionCast(wrapInstallInterrupts, callback->orgInstallInterrupts)(that);
	// AtiFbInterruptManager: +0x31 installed, +0x34 interrupt index (see installInterruptsOnPciDevice).
	auto *manager = static_cast<uint8_t *>(that);
	IOLog("CarrizoRed: installInterruptsOnPciDevice = 0x%x, index %u%s\n", ret, *reinterpret_cast<uint32_t *>(manager + 0x34),
		manager[0x31] ? "" : " (not installed)");
	traceNote("ints=%x/%u ", ret, *reinterpret_cast<uint32_t *>(manager + 0x34));
	if (gpu)
		printMsiState(gpu, "after interrupt install");
	IOSleep(30);
	return ret;
}

static uint64_t peekValue(const uint64_t *value) { return value ? *value : 0; }

IOReturn CRDriver::wrapDeviceControlAttribute(void *that, uint32_t attribute, uint64_t *value, uint64_t size,
	uint64_t *arg4, uint64_t *arg5, void *arguments) {
	pmLog(true, "agdc.deviceControl attr", attribute, "=> AtiDeviceControl attribute 0x%x value 0x%llx size %llu",
		attribute, peekValue(value), size);
	auto ret = FunctionCast(wrapDeviceControlAttribute, callback->orgDeviceControlAttribute)(that, attribute, value,
		size, arg4, arg5, arguments);
	pmLog(true, "agdc.deviceControl ret", ret, "<= AtiDeviceControl attribute 0x%x = 0x%x, value 0x%llx", attribute,
		ret, peekValue(value));
	return ret;
}

IOReturn CRDriver::wrapWranglerAttribute(void *that, uint32_t attribute, uint64_t *value, uint64_t size,
	uint64_t *arg4, uint64_t *arg5, void *arguments) {
	pmLog(true, "agdc.wrangler attr", attribute, "=> AtiGpuWrangler attribute 0x%x value 0x%llx size %llu", attribute,
		peekValue(value), size);
	auto ret = FunctionCast(wrapWranglerAttribute, callback->orgWranglerAttribute)(that, attribute, value, size, arg4,
		arg5, arguments);
	pmLog(true, "agdc.wrangler ret", ret, "<= AtiGpuWrangler attribute 0x%x = 0x%x, value 0x%llx", attribute, ret,
		peekValue(value));
	return ret;
}

// Apple's controller writing config space itself (BaffinRegisterService::writePCIReg*, offset into config space).
void CRDriver::wrapWritePCIReg8(void *that, uint32_t offset, uint8_t value) {
	pmLog(false, "ctrl.writePCIReg8", offset, "controller config write8 0x%x = 0x%x", offset, value);
	FunctionCast(wrapWritePCIReg8, callback->orgWritePCIReg8)(that, offset, value);
}

void CRDriver::wrapWritePCIReg16(void *that, uint32_t offset, uint16_t value) {
	pmLog(false, "ctrl.writePCIReg16", offset, "controller config write16 0x%x = 0x%x", offset, value);
	FunctionCast(wrapWritePCIReg16, callback->orgWritePCIReg16)(that, offset, value);
}

void CRDriver::wrapWritePCIReg32(void *that, uint32_t offset, uint32_t value) {
	pmLog(false, "ctrl.writePCIReg32", offset, "controller config write32 0x%x = 0x%x", offset, value);
	FunctionCast(wrapWritePCIReg32, callback->orgWritePCIReg32)(that, offset, value);
}

IOService *CRDriver::wrapProbeNoMatch(IOService *, IOService *, SInt32 *) { return nullptr; }

uint32_t CRDriver::wrapGetDisplayLimit(void *) { return displayPipes(callback->family); }

uint8_t CRDriver::wrapGetNumLineBuffers(void *) { return displayPipes(callback->family); }

uint32_t CRDriver::wrapGetDigLimit(void *) { return DisplayDigs; }

void CRDriver::logPCIInfo() {
	revision = static_cast<uint8_t>(WIOKit::readPCIConfigValue(iGPU, WIOKit::kIOPCIConfigRevisionID));
	auto subVendor = WIOKit::readPCIConfigValue(iGPU, WIOKit::kIOPCIConfigSubSystemVendorID, 0, 16);
	auto subDevice = WIOKit::readPCIConfigValue(iGPU, WIOKit::kIOPCIConfigSubSystemID, 0, 16);

	uint32_t propDeviceId = 0;
	WIOKit::getOSDataValue(iGPU, "device-id", propDeviceId);

	SYSLOG("cr", "found %s iGPU at %s: 1002:%04X rev 0x%02X subsys %04X:%04X (device-id property 0x%04X)",
		familyName(family), safeString(iGPU->getName()), deviceId, revision, subVendor, subDevice, propDeviceId & 0xFFFF);

	if (auto *bar0 = iGPU->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0))
		SYSLOG("cr", "BAR0 (FB aperture) 0x%llX size 0x%llX", static_cast<uint64_t>(bar0->getPhysicalAddress()),
			static_cast<uint64_t>(bar0->getLength()));
	if (auto *bar5 = iGPU->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5))
		SYSLOG("cr", "BAR5 (MMIO) 0x%llX size 0x%llX", static_cast<uint64_t>(bar5->getPhysicalAddress()),
			static_cast<uint64_t>(bar5->getLength()));

	iGPU->setProperty("CarrizoRed,family", familyName(family));
	iGPU->setProperty("CarrizoRed,revision", revision, 8);
}

void CRDriver::probeMMIO() {
	auto *map = iGPU->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
	if (!map) {
		SYSLOG("cr", "failed to map MMIO BAR, skipping register probe");
		return;
	}

	auto *mmio = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
	// Keep the mapping: the IH storm breaker writes registers directly (see wrapIhHandler), and the watcher reads the
	// display registers (checkDisplayRegs).
	gpuMmio = mmio;
	auto memSize = mmio[Reg::CONFIG_MEMSIZE];
	auto fbLocation = mmio[Reg::MC_VM_FB_LOCATION];
	auto fbOffset = mmio[Reg::MC_VM_FB_OFFSET];
	auto grbmStatus = mmio[Reg::GRBM_STATUS];
	auto gbAddrConfig = mmio[Reg::GB_ADDR_CONFIG];
	auto shaderArrayConfig = mmio[Reg::CC_GC_SHADER_ARRAY_CONFIG];
	auto rbBackendDisable = mmio[Reg::CC_RB_BACKEND_DISABLE];

	// Same sanity rule as gmc_v8_0_mc_init(): some firmware leaves garbage in the upper half.
	if ((memSize & 0xFFFF0000) && (memSize & 0xFFFF))
		memSize &= 0xFFFF;

	uint64_t fbBase = static_cast<uint64_t>(fbLocation & 0xFFFF) << 24;
	uint64_t fbTop = (static_cast<uint64_t>(fbLocation >> 16) << 24) | 0xFFFFFF;
	uint64_t carveoutPhys = static_cast<uint64_t>(fbOffset) << 22;

	SYSLOG("cr", "UMA carve-out %u MB at system 0x%llX, GPU FB range 0x%llX-0x%llX", memSize, carveoutPhys, fbBase,
		fbTop);
	SYSLOG("cr", "GRBM_STATUS 0x%08X GB_ADDR_CONFIG 0x%08X CC_GC_SHADER_ARRAY_CONFIG 0x%08X CC_RB_BACKEND_DISABLE 0x%08X",
		grbmStatus, gbAddrConfig, shaderArrayConfig, rbBackendDisable);

	iGPU->setProperty("CarrizoRed,vram-mb", memSize, 32);
	iGPU->setProperty("CarrizoRed,carveout-phys", carveoutPhys, 64);
	iGPU->setProperty("CarrizoRed,gb-addr-config", gbAddrConfig, 32);

	initDisplayRegs();
}

// Display and memory-controller registers watched from boot (firmware/GOP values) through Apple's bring-up: FB
// location, HDP aperture, DMIF, and per pipe the scanout surface, watermarks, stutter and NB P-state control. The
// watcher prints every change (first 8 per register), so the values Apple leaves behind before a freeze are on screen
// and can be compared with the GOP's and Linux's. Per-pipe registers are only read while that pipe is powered
// (PIPEn_PG_STATUS), and only for the APU's real pipes.
namespace {
	struct WatchedRegDef {
		const char *name;
		uint32_t offset;
	};

	const WatchedRegDef globalRegDefs[] {
		{"MC_VM_FB_LOCATION", Reg::MC_VM_FB_LOCATION},
		{"MC_VM_FB_OFFSET", Reg::MC_VM_FB_OFFSET},
		{"MC_VM_SYSTEM_APERTURE_LOW", Reg::MC_VM_SYSTEM_APERTURE_LOW_ADDR},
		{"MC_VM_SYSTEM_APERTURE_HIGH", Reg::MC_VM_SYSTEM_APERTURE_HIGH_ADDR},
		{"HDP_NONSURFACE_BASE", Reg::HDP_NONSURFACE_BASE},
		{"HDP_NONSURFACE_INFO", Reg::HDP_NONSURFACE_INFO},
		{"HDP_NONSURFACE_SIZE", Reg::HDP_NONSURFACE_SIZE},
		{"DMIF_ADDR_CONFIG", Reg::DMIF_ADDR_CONFIG},
		{"DMIF_CONTROL", Reg::DMIF_CONTROL},
		{"DMIF_STATUS", Reg::DMIF_STATUS},
		{"DMIF_ARBITRATION_CONTROL", Reg::DMIF_ARBITRATION_CONTROL},
		{"DMIF_ADDR_CALC", Reg::DMIF_ADDR_CALC},
		{"MCIF_CONTROL", Reg::MCIF_CONTROL},
		{"DCI_MEM_PWR_STATUS", Reg::DCI_MEM_PWR_STATUS},
		{"DCI_CLK_CNTL", Reg::DCI_CLK_CNTL},
		{"PIXCLK0_RESYNC_CNTL", Reg::PIXCLK0_RESYNC_CNTL},
		{"PIXCLK1_RESYNC_CNTL", Reg::PIXCLK1_RESYNC_CNTL},
		{"PHYPLL_PIXCLK_CNTL", Reg::PHYPLL_PIXCLK_CNTL},
		{"CRTC0_PIXEL_RATE_CNTL", Reg::CRTC0_PIXEL_RATE_CNTL},
		{"CRTC1_PIXEL_RATE_CNTL", Reg::CRTC0_PIXEL_RATE_CNTL + 4},
	};

	const WatchedRegDef pipeRegDefs[] {
		{"GRPH_ENABLE", Reg::GRPH_ENABLE},
		{"GRPH_CONTROL", Reg::GRPH_CONTROL},
		{"GRPH_PRIMARY_SURFACE_ADDRESS", Reg::GRPH_PRIMARY_SURFACE_ADDRESS},
		{"GRPH_PRIMARY_SURFACE_ADDRESS_HIGH", Reg::GRPH_PRIMARY_SURFACE_ADDRESS_HIGH},
		{"DPG_PIPE_ARBITRATION_CONTROL1", Reg::DPG_PIPE_ARBITRATION_CONTROL1},
		{"DPG_PIPE_ARBITRATION_CONTROL2", Reg::DPG_PIPE_ARBITRATION_CONTROL2},
		{"DPG_WATERMARK_MASK_CONTROL", Reg::DPG_WATERMARK_MASK_CONTROL},
		{"DPG_PIPE_URGENCY_CONTROL", Reg::DPG_PIPE_URGENCY_CONTROL},
		{"DPG_PIPE_DPM_CONTROL", Reg::DPG_PIPE_DPM_CONTROL},
		{"DPG_PIPE_STUTTER_CONTROL", Reg::DPG_PIPE_STUTTER_CONTROL},
		{"DPG_PIPE_NB_PSTATE_CHANGE_CONTROL", Reg::DPG_PIPE_NB_PSTATE_CHANGE_CONTROL},
		{"DPG_PIPE_STUTTER_CONTROL_NONLPTCH", Reg::DPG_PIPE_STUTTER_CONTROL_NONLPTCH},
		{"CRTC_CONTROL", Reg::CRTC_CONTROL},
	};

	constexpr size_t MaxWatchedPipes = 3;
	constexpr size_t MaxWatchedRegs = sizeof(globalRegDefs) / sizeof(globalRegDefs[0]) +
	                                  MaxWatchedPipes * (sizeof(pipeRegDefs) / sizeof(pipeRegDefs[0]));
	constexpr uint32_t MaxPrintedChanges = 8;

	struct WatchedReg {
		const char *name;
		int pipe;  // -1 = display-wide
		uint32_t offset, value, changes;
		bool valid;  // value read (pipe powered)
	};
	WatchedReg watchedRegs[MaxWatchedRegs];
	size_t watchedRegCount;

	bool pipePowered(uint32_t pipe) { return (gpuMmio[Reg::PIPE0_PG_STATUS + 3 * pipe] >> 30) == 0; }
}

void CRDriver::initDisplayRegs() {
	for (auto &def : globalRegDefs)
		watchedRegs[watchedRegCount++] = {def.name, -1, def.offset, 0, 0, false};
	uint32_t pipes = displayPipes(family);
	for (uint32_t pipe = 0; pipe < pipes && pipe < MaxWatchedPipes; pipe++)
		for (auto &def : pipeRegDefs)
			watchedRegs[watchedRegCount++] = {def.name, static_cast<int>(pipe), def.offset + pipeRegisterBase(pipe), 0, 0,
				false};

	for (size_t i = 0; i < watchedRegCount; i++) {
		auto &reg = watchedRegs[i];
		if (reg.pipe >= 0 && !pipePowered(reg.pipe))
			continue;
		reg.value = gpuMmio[reg.offset];
		reg.valid = true;
	}
	for (uint32_t pipe = 0; pipe < pipes && pipe < MaxWatchedPipes; pipe++)
		SYSLOG("cr", "boot: PIPE%u_PG_STATUS 0x%08X", pipe, gpuMmio[Reg::PIPE0_PG_STATUS + 3 * pipe]);
	for (size_t i = 0; i < watchedRegCount; i++) {
		auto &reg = watchedRegs[i];
		if (!reg.valid)
			SYSLOG("cr", "boot: p%d %s 0x%X: pipe powered off", reg.pipe, reg.name, reg.offset);
		else if (reg.pipe < 0)
			SYSLOG("cr", "boot: %s 0x%X = 0x%08X", reg.name, reg.offset, reg.value);
		else
			SYSLOG("cr", "boot: p%d %s 0x%X = 0x%08X", reg.pipe, reg.name, reg.offset, reg.value);
	}
}

// crpanicat=N black-box report. Spoof boot 21 went to a black panel with no panic in NVRAM, after IOFramebuffer saved a
// 1366x768 / 76.30 MHz "saved-config", i.e. Apple's controller set a mode. With the screen black, the panic text in
// NVRAM (AAPL,PanicInfo, read with OpenShell dmpstore) is the only way out: counters, the iGPU tree (framebuffers,
// connectors, mode), the current display/clock registers, the trace summary and the last trace events (setPixelClock,
// setCrtcToPixelClock, framebuffer calls). Register values are read fresh (pipes only when powered).
void CRDriver::panicReport(uint32_t t, const char *tree) {
	static char report[4096];
	size_t len = 0;
	auto add = [&](const char *fmt, ...) {
		if (len + 1 >= sizeof(report))
			return;
		va_list va;
		va_start(va, fmt);
		int n = vsnprintf(report + len, sizeof(report) - len, fmt, va);
		va_end(va);
		if (n > 0)
			len += static_cast<size_t>(n) < sizeof(report) - len ? static_cast<size_t>(n) : sizeof(report) - len - 1;
	};
	add("t=%us regs %u ih %u missing-pipe %u pm %u cfgwr %u\n", t, __atomic_load_n(&regAccesses, __ATOMIC_RELAXED),
		__atomic_load_n(&ihHandlerCalls, __ATOMIC_RELAXED), __atomic_load_n(&pipeAccesses, __ATOMIC_RELAXED),
		__atomic_load_n(&pmEvents, __ATOMIC_RELAXED), __atomic_load_n(&pciConfigWrites, __ATOMIC_RELAXED));
	add("tree:\n%s", tree ? tree : "");
	if (gpuMmio) {
		add("regs:");
		bool powered[MaxWatchedPipes] {};
		for (uint32_t pipe = 0; pipe < MaxWatchedPipes && pipe < displayPipes(family); pipe++)
			powered[pipe] = pipePowered(pipe);
		for (size_t i = 0; i < watchedRegCount; i++) {
			auto &reg = watchedRegs[i];
			if (reg.pipe >= 0 && !powered[reg.pipe])
				continue;
			add(" %x=%x", reg.offset, gpuMmio[reg.offset]);
		}
		add("\n");
	}
	add("trace: %s\nlast events:", traceSummary);
	uint32_t head = __atomic_load_n(&traceHead, __ATOMIC_RELAXED);
	for (uint32_t i = head > 24 ? head - 24 : 0; i != head; i++) {
		auto &event = traceRing[i % TraceRingSize];
		if (event.name)
			add(" %s%s=%x", event.exit ? "" : ">", event.name, event.value);
	}
	panic("CarrizoRed: deliberate panic (crpanicat), the kernel was still running.\n%s", report);
}

void CRDriver::checkDisplayRegs(uint32_t t) {
	bool powered[MaxWatchedPipes];
	for (uint32_t pipe = 0; pipe < MaxWatchedPipes && pipe < displayPipes(family); pipe++)
		powered[pipe] = pipePowered(pipe);
	for (size_t i = 0; i < watchedRegCount; i++) {
		auto &reg = watchedRegs[i];
		if (reg.pipe >= 0 && !powered[reg.pipe]) {
			if (reg.valid && reg.changes++ < MaxPrintedChanges)
				IOLog("CarrizoRed: t=%us p%d %s: pipe powered off\n", t, reg.pipe, reg.name);
			reg.valid = false;
			continue;
		}
		uint32_t value = gpuMmio[reg.offset];
		if (reg.valid && value == reg.value)
			continue;
		uint32_t changes = reg.changes++;
		if (changes < MaxPrintedChanges) {
			char was[24] = " (was off)";
			if (reg.valid)
				snprintf(was, sizeof(was), " (was 0x%08X)", reg.value);
			char pipe[8] = "";
			if (reg.pipe >= 0)
				snprintf(pipe, sizeof(pipe), "p%d ", reg.pipe);
			IOLog("CarrizoRed: t=%us %s%s 0x%X = 0x%08X%s%s\n", t, pipe, reg.name, reg.offset, value, was,
				changes + 1 == MaxPrintedChanges ? ", further changes not shown" : "");
		}
		reg.value = value;
		reg.valid = true;
	}
}

void CRDriver::disableKextCallbacks() {
	for (auto &kext : kexts)
		kext.switchOff();
}
