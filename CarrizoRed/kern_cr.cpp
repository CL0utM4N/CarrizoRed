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
static char traceSummary[2560];
static uint16_t softPPTableSize, softPPTableRev;  // see setSoftPowerPlayTable

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

static constexpr size_t TraceRingSize = 512, MaxTraceStages = 128;
static constexpr uint64_t WatchdogMs = 60000;
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
static bool lbFixOff;    // -crnolbfix, see wrapRegWrite32
static uint32_t lbFixes;
// DP colour depth of the link the firmware left running (DP_PIXEL_FORMAT.DP_COMPONENT_DEPTH: 0 = 6, 1 = 8, 2 = 10,
// 3 = 12 bpc), 0xFF = unknown; see initDisplayRegs. -crnobpccap disables the cap.
static uint8_t bootDpDepth = 0xFF;
static bool bpcCapOff;
static uint32_t bpcCaps;
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
static void restoreTick(uint32_t t);  // -crrestore, defined with the display-path diff
static bool restoreOn;
static bool assrOff;  // -crnoassr, see wrapAuxWriteBytes
static uint32_t restoreDone, restoreWrites, restoreHolds;
static void underflowTick();  // pipe 0 underflow sampling, defined with the display-path diff
static uint32_t underflowSamples, xdmaUnderflowMax, dmifUnderflows, blndUnderflows, lbLevelMin = ~0U, lbLevelMax;
// Power-management / config-space events on the iGPU (see patchPci).
static uint32_t pmEvents, pciConfigWrites;

// Every VBIOS command table Apple runs (AtiAtomBiosParserService::executeBiosCommandTable): table index, the first 16
// parameter bytes going in, the result and the first 8 bytes coming back. Spoof boot 22: eDP link training failed at
// clock recovery although the firmware trained the same panel at boot, so what Apple passes the encoder / transmitter
// tables (DIGxEncoderControl 1.4, DIG1TransmitterControl 1.5 on Stoney) is the next thing to see. AUX transactions
// (ProcessAuxChannelTransaction, table 0x4E) are counted and kept in their own small ring; they would flood the main one.
// Consecutive identical calls are merged into one entry with a repeat count, so the ring covers whole sequences
// (training retries repeat each call several times).
struct AtomCall {
	uint16_t table;
	uint8_t ok;
	uint16_t repeat;
	uint32_t in[4], out[2];
};
static constexpr uint32_t AtomRingSize = 32, AtomAuxTable = 0x4E, AtomTransmitterTable = 0x4C, AtomEncoderTable = 0x04;
static bool dpExtClkOff;    // -crnodpextclk, see wrapExecuteBiosCommandTable
static bool edpPowerOnOff;  // -crnoedppoweron, see wrapExecuteBiosCommandTable

// eDP block state at the moment Apple starts training pattern 1 (DPCD 0x102 written with TPS1): panel power
// sequencer, DIG0, DP0, pixel rate, backlight. The black box otherwise only sees the state after Apple gave up.
static const uint32_t trainSnapRegs[] {0x481B, 0x481C, 0x4A00, 0x4A47, 0x4AA0, 0x4AA3, 0x4AA4, 0x4AAF, 0x140, 0x4820};
static uint32_t trainSnap[sizeof(trainSnapRegs) / sizeof(trainSnapRegs[0])], trainSnaps;
static AtomCall atomRing[AtomRingSize];
static uint32_t atomHead, atomCalls, auxCalls, auxFailures;

// DPCD transcript: every native AUX read/write Apple's DisplayPort code makes (AuxChannelProxy::readBytes/writeBytes),
// with address, length, direction, the first 8 bytes (written, or read back) and the result; identical consecutive
// transactions merged. Link training is visible here: link rate 0x100, lane count 0x101, pattern 0x102, lane settings
// 0x103.., sink status 0x202.. (lane CR/EQ/lock bits, adjust requests).
struct DpcdTx {
	uint32_t address;
	uint8_t length, write, ok;
	uint16_t repeat;
	uint8_t data[8];
};
static constexpr uint32_t DpcdRingSize = 40;
// Entries printed in the black box (the newest), sized so the whole report fits AAPL,PanicInfo.
static constexpr uint32_t AtomPrinted = 4, DpcdPrinted = 4;
static DpcdTx dpcdRing[DpcdRingSize];
static uint32_t dpcdHead, dpcdCount;

template <typename Ring, typename Entry>
static void ringPushMerged(Ring *ring, uint32_t size, uint32_t &head, const Entry &entry, bool (*same)(const Ring &,
	const Entry &)) {
	uint32_t h = __atomic_load_n(&head, __ATOMIC_RELAXED);
	if (h && same(ring[(h - 1) % size], entry) && ring[(h - 1) % size].repeat < 0xFFFF) {
		ring[(h - 1) % size].repeat++;
		return;
	}
	ring[__atomic_fetch_add(&head, 1, __ATOMIC_RELAXED) % size] = entry;
}

template <typename Add>
static void atomReport(Add &add) {
	add("atom: %u calls (%u aux, %u aux failed); idx:table ok(0x80=patched) in[0..3] -> out[0..1] xN\n", atomCalls,
		auxCalls, auxFailures);
	uint32_t head = __atomic_load_n(&atomHead, __ATOMIC_RELAXED);
	for (uint32_t i = head > AtomPrinted ? head - AtomPrinted : 0; i != head; i++) {
		auto &call = atomRing[i % AtomRingSize];
		add(" %u:%x %x %x %x %x %x -> %x %x x%u\n", i, call.table, call.ok, call.in[0], call.in[1], call.in[2],
			call.in[3], call.out[0], call.out[1], call.repeat);
	}
	add("at TPS1 (%u times):", trainSnaps);
	for (size_t i = 0; i < sizeof(trainSnapRegs) / sizeof(trainSnapRegs[0]); i++)
		add(" %x=%x", trainSnapRegs[i], trainSnap[i]);
	add("\ndpcd: %u transactions; addr R/W len ret: bytes xN\n", dpcdCount);
	head = __atomic_load_n(&dpcdHead, __ATOMIC_RELAXED);
	for (uint32_t i = head > DpcdPrinted ? head - DpcdPrinted : 0; i != head; i++) {
		auto &tx = dpcdRing[i % DpcdRingSize];
		add(" %x %c%u %x:", tx.address, tx.write ? 'W' : 'R', tx.length, tx.ok);
		for (uint32_t b = 0; b < tx.length && b < sizeof(tx.data); b++)
			add(" %02x", tx.data[b]);
		add(" x%u\n", tx.repeat);
	}
}

// IH storm breaker. IH registers are dword offsets in BAR5, the same on Tonga (oss_3_0) and CZ (oss_3_0_1).
namespace IhReg {
	static constexpr uint32_t RB_CNTL = 0xE30, RB_BASE = 0xE31, RB_RPTR = 0xE32, RB_WPTR = 0xE33, RB_WPTR_ADDR_HI = 0xE34,
	                          RB_WPTR_ADDR_LO = 0xE35, CNTL = 0xE36, STATUS = 0xE38;
}
static volatile uint32_t *gpuMmio;  // BAR5, mapped by probeMMIO
static OSData *vbiosImage;            // our reference to the iGPU's "ATY,bin_image", see wrapCailEarlyInitialize
static IORegistryEntry *vbiosOwner;

static void keepVbiosImage(IORegistryEntry *gpu) {
	vbiosImage = OSDynamicCast(OSData, gpu->getProperty(VBIOS::ImageProperty));
	if (vbiosImage) {
		vbiosImage->retain();
		vbiosOwner = gpu;
	}
}
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
static const char *pathRadeonX4000HWLibs[] {"/System/Library/Extensions/AMDRadeonX4000HWServices.kext/Contents/PlugIns/"
										   "AMDRadeonX4000HWLibs.kext/Contents/MacOS/AMDRadeonX4000HWLibs"};
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
	KextRadeonX4000HWLibs,
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
	[KextRadeonX4000HWLibs] = {"com.apple.kext.AMDRadeonX4000HWLibs", pathRadeonX4000HWLibs,
		arrsize(pathRadeonX4000HWLibs), {}, {}, KernelPatcher::KextInfo::Unloaded},
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
	lbFixOff = checkKernelArgument("-crnolbfix");
	bpcCapOff = checkKernelArgument("-crnobpccap");
	restoreOn = checkKernelArgument("-crrestore");
	assrOff = checkKernelArgument("-crnoassr");

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
	keepVbiosImage(iGPU);
	setSoftPowerPlayTable();
}

// PowerPlay's table parser (HWLibs PP_Tables_Initialize) gets its table from the PP_PhmSoftPowerPlayTable registry key,
// else, on a pre-Raven APU, from the VBIOS through PECI_RetrieveBiosDataTable(PowerPlayInfo). On the D509BA that path
// appears to fail: PHM_Initialize returns PP_Result_Failed before any later back-end step (nvram36/39), although the
// VBIOS has the table (PowerPlayInfo rev 6.1, 495 bytes: a classic PPLib table, which the Dummy back end's CZ clock-info
// callback decodes). Hand it the VBIOS's own table as the soft table. Only for the Dummy back end; -crnosoftpptable
// disables it.
void CRDriver::setSoftPowerPlayTable() {
	if (checkKernelArgument("-crnoppdummy") || checkKernelArgument("-crnosoftpptable") ||
		iGPU->getProperty("PP_PhmSoftPowerPlayTable"))
		return;
	auto *image = OSDynamicCast(OSData, iGPU->getProperty(VBIOS::ImageProperty));
	uint16_t size = 0;
	const uint8_t *table = image ? VBIOS::findDataTable(static_cast<const uint8_t *>(image->getBytesNoCopy()),
		image->getLength(), VBIOS::PowerPlayInfoTable, size) : nullptr;
	if (!table) {
		SYSLOG("cr", "no PowerPlayInfo table in the VBIOS, PowerPlay will look for it itself");
		iGPU->setProperty("CarrizoRed,soft-pptable", "none");
		return;
	}
	iGPU->setProperty("PP_PhmSoftPowerPlayTable", const_cast<uint8_t *>(table), size);
	iGPU->setProperty("CarrizoRed,soft-pptable", size, 16);
	softPPTableSize = size;
	softPPTableRev = static_cast<uint16_t>(table[2] << 8 | table[3]);
	SYSLOG("cr", "PP_PhmSoftPowerPlayTable: VBIOS PowerPlayInfo, %u bytes, rev %u.%u", size, table[2], table[3]);
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
		// The acceleration kexts load late (IOKit matching), if at all: note it, for the black box.
		if (i == KextRadeonX4000 || i == KextRadeonX4000HWServices || i == KextRadeonX4000HWLibs)
			traceNote("kext:%s ", i == KextRadeonX4000 ? "X4000" : i == KextRadeonX4000HWServices ? "HWServices" : "HWLibs");
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
		else if (i == KextRadeonX4000) {
			blockProbe(patcher, index, slide, size, "__ZN37AMDRadeonX4000_AMDGraphicsAccelerator5probeEP9IOServicePi",
				"-crnoaccel");
			if (!checkKernelArgument("-crnoaccel")) {
				patchAccelerator(patcher, index, slide, size);
				traceKext(patcher, index, slide, size, i);
			}
		} else if (i == KextRadeonX4000HWServices) {
			blockProbe(patcher, index, slide, size,
				"__ZN42AMDRadeonX4000_AMDRadeonHWServicesAbstract5probeEP9IOServicePi", "-crnoaccel");
			if (!checkKernelArgument("-crnoaccel"))
				traceKext(patcher, index, slide, size, i);
		} else if (i == KextRadeonX4000HWLibs && !checkKernelArgument("-crnoaccel"))
			traceKext(patcher, index, slide, size, i);
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

// NVRAM breadcrumb. The black box only reaches NVRAM through a panic; the second accelerated boot (nvram31) ended in a
// hard reset (no panic text, straight back to the boot menu), which leaves nothing. While trace events keep arriving,
// write the latest progress to the CR-progress variable (Apple's GUID, 7C436110-...) and force a sync: at most every
// 500 ms and 160 times per boot. -crnobreadcrumb disables it.
static uint32_t breadcrumbWrites, breadcrumbRefused;
static bool breadcrumbNoOptions;

static void writeBreadcrumb(uint32_t t) {
	static IORegistryEntry *options;
	static uint32_t lastHead = ~0U, lastIh;
	uint32_t &writes = breadcrumbWrites;
	static char text[1000];
	uint32_t head = __atomic_load_n(&traceHead, __ATOMIC_RELAXED);
	uint32_t ih = __atomic_load_n(&ihHandlerCalls, __ATOMIC_RELAXED);
	if (writes >= 160 || (head == lastHead && (ih == lastIh || t % 5)))
		return;
	if (!options && !(options = IORegistryEntry::fromPath("/options", gIODTPlane))) {
		breadcrumbNoOptions = true;
		return;
	}
	lastHead = head;
	lastIh = ih;
	size_t len = static_cast<size_t>(snprintf(text, sizeof(text), "t=%u w=%u ev=%u ih=%u last:", t, writes, head, ih));
	// Newest 10 events, oldest first.
	for (uint32_t i = head > 10 ? head - 10 : 0; i != head && len < sizeof(text); i++) {
		auto &event = traceRing[i % TraceRingSize];
		len += static_cast<size_t>(snprintf(text + len, sizeof(text) - len, " %s%s=%x", event.exit ? "" : ">",
			event.name ? event.name : "?", event.value));
	}
	// Then as much of the end of the trace summary as fits.
	if (len + 8 < sizeof(text)) {
		size_t summary = strlen(traceSummary), room = sizeof(text) - len - 8;
		len += static_cast<size_t>(snprintf(text + len, sizeof(text) - len, " | %s",
			traceSummary + (summary > room ? summary - room : 0)));
	}
	if (len >= sizeof(text))
		len = sizeof(text) - 1;
	auto *data = OSData::withBytes(text, static_cast<unsigned>(len));
	if (!data)
		return;
	if (!options->setProperty("CR-progress", data))
		breadcrumbRefused++;
	data->release();
	options->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", "CR-progress");
	options->setProperty("IONVRAM-SYNCNOW-PROPERTY", "CR-progress");
	writes++;
}

void CRDriver::watcherThread(void *arg, wait_result_t) {
	auto *self = static_cast<CRDriver *>(arg);
	static char last[1536], now[1536];
	bool print = !checkKernelArgument("-crnowatch");
	bool watchdog = !checkKernelArgument("-crnowatchdog");
	bool controllerRequested = checkKernelArgument("-crnoloadctl");
	bool breadcrumb = !checkKernelArgument("-crnobreadcrumb");
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

		if (gpuMmio)
			underflowTick();
		if (restoreOn && gpuMmio)
			restoreTick(t);
		if (breadcrumb)
			writeBreadcrumb(t);
		if (panicAt && t >= panicAt)
			self->panicReport(t, now, "CarrizoRed: deliberate panic (crpanicat), the kernel was still running.");

		// Watchdog: a traced call that never returns (a hard hang with this CPU still alive) becomes a panic that
		// names it and stays on screen (debug=0x100). -crnowatchdog disables this.
		// nvram42: fb.open waited on the accelerator's power-up; name every open stage and add the full black-box
		// report. 60 s: CAIL's register waits time out after 3 s each (AtiAppleMcilServices::waitFor counts in ms), and
		// the RLC start-up alone has several (nvram48), so a slow but progressing power-up must not trip it.
		if (watchdog) {
			uint64_t ms = uptimeMs();
			bool stuck = false;
			for (size_t i = 0; i < MaxTraceStages && !stuck; i++)
				stuck = __atomic_load_n(&stageOpen[i], __ATOMIC_RELAXED) && ms - stageEnterMs[i] > WatchdogMs;
			if (stuck) {
				static char reason[512];
				size_t len = static_cast<size_t>(snprintf(reason, sizeof(reason), "CarrizoRed watchdog: open stages:"));
				for (size_t i = 0; i < MaxTraceStages && len < sizeof(reason); i++)
					if (__atomic_load_n(&stageOpen[i], __ATOMIC_RELAXED))
						len += static_cast<size_t>(snprintf(reason + len, sizeof(reason) - len, " %s(%llu ms)",
							stageNames[i], ms - stageEnterMs[i]));
				self->panicReport(t, now, reason);
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
		// framebuffer slot and the GUI has no display. Do the same once the wrangler has attached, and also ask for
		// acceleration (flag bit 1: LoadHWServices + LoadAccelerator, the AMDRadeonX4000HWServices and AMDRadeonX4000
		// personalities) unless -crnoaccel. -crnoloadctl disables this.
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
		// Acceleration, separately: since the interrupt fix the graphics policy (AGDP) sets LoadController itself
		// (first usb-spoof-accel boot, nvram27: no LoadController note in the trace, controller attached, no HWServices or
		// accelerator), but never the acceleration bit, and the request above is skipped. Ask once the controller is up.
		static bool accelRequested;
		if (!accelRequested && strstr(now, "AMD9500Controller (")) {
			accelRequested = true;
			bool already = self->iGPU->getProperty("LoadAccelerator") != nullptr;
			if (!checkKernelArgument("-crnoaccel") && !already) {
				self->iGPU->setProperty("LoadHWServices", true);
				self->iGPU->setProperty("LoadAccelerator", true);
				self->iGPU->registerService();
				IOLog("CarrizoRed: t=%us set LoadHWServices + LoadAccelerator on the iGPU and re-registered it\n", t);
			}
			traceNote("LoadAccelerator@%us%s ", t, already ? "(already)" : "");
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
//  power management) started, outside every traced display function (before the ASSR and IH fixes). The accelerator
//  needs PowerPlay, though: with CFG_NO_PP, BaffinPowerPlayManager::initialize fails (kIOReturnUnsupported, nvram35)
//  and the engines are never powered up. With acceleration and the Dummy PowerPlay back end (see logPCIInfo), which
//  doesn't talk to the SMU, it stays on; -crnopp forces it off, -crpp forces it on.
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
	if (!strcmp(name, "CFG_NO_PP") && !checkKernelArgument("-crpp") &&
		(checkKernelArgument("-crnopp") || checkKernelArgument("-crnoaccel") || checkKernelArgument("-crnoppdummy"))) {
		memset(value, 0, size);
		*static_cast<uint8_t *>(value) = 1;
		traceEvent("CFG_NO_PP", true, 1);
		if (!ret)
			ret = 1;
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
	enum TraceKext { InController, InSupport, InFramebuffer, InAccel, InHWServices, InHWLibs };

	struct TracedStage {
		const char *symbol;
		const char *name;
		TraceKext kext;
		bool summary;  // note the result in the repeated trace summary (start-up stages)
		bool sync;     // thread context only: also print on entry and pause, so an instant hard hang still shows it
		mach_vm_address_t org;
		uint8_t arg {0};  // entry event argument: 0 none, 1 b, 2 c, 3 b | c << 8, 4 c | d << 8, 5 c + *d after return
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
		// eDP bring-up (spoof boot 21/22: CRTC 0 enabled on the DP DTO with DISPOUT_FIFO_ERROR, panel black). Entry
		// events record the action / crtc / power argument. DisplayPortUtilities::trainLink and fastTrainLink can't be
		// wrapped (stack-protector prologue, see routeChecked), so their steps are traced instead.
		{"__ZN20DisplayPortUtilities18prepareForTrainingEh", "dp.prepareForTraining", InSupport, false, false, 0, 1},
		{"__ZN20DisplayPortUtilities18doTrainingPattern1EP19ActiveConnectorInfohPh", "dp.trainingPattern1", InSupport, false, false, 0, 5},
		{"__ZN20DisplayPortUtilities18doTrainingPattern2EP19ActiveConnectorInfohPh", "dp.trainingPattern2", InSupport, false, false, 0, 5},
		{"__ZN20DisplayPortUtilities13isLinkTrainedEv", "dp.isLinkTrained", InSupport, false, false, 0},
		{"__ZN12ATIConnector18linkTrainingFailedEv", "dp.linkTrainingFailed", InSupport, true, false, 0},
		{"__ZN20DisplayPortUtilities13setPanelPowerEh", "dp.setPanelPower", InSupport, false, false, 0, 1},
		{"__ZN13ATIController24turnOnInternalPanelPowerEP11CRTC_PARAMSb", "turnOnInternalPanelPower", InSupport, false, false, 0, 2},
		{"__ZN13AtomBiosProxy25digitalTransmitterControlEPK19ActiveConnectorInfo24AtiAtomTransmitterAction", "atom.transmitter", InSupport, false, false, 0, 2},
		{"__ZN13AtomBiosProxy21digitalEncoderControlEPK19ActiveConnectorInfo20AtiAtomEncoderAction", "atom.encoder", InSupport, false, false, 0, 2},
		{"__ZN13AtomBiosProxy10enableCrtcEhb", "atom.enableCrtc", InSupport, false, false, 0, 3},
		{"__ZN13AtomBiosProxy15setDisplayClockEhj", "atom.setDisplayClock", InSupport, false, false, 0, 2},
		{"__ZN13AtomBiosProxy16selectCrtcSourceEPK19ActiveConnectorInfo", "atom.selectCrtcSource", InSupport, false, false, 0},
		{"__ZN27BaffinDisplayPortConnection13eDPPanelPowerEb", "eDPPanelPower", InController, false, false, 0, 1},
		{"__ZN27BaffinBiosDigitalConnection12setBacklightEh", "setBacklight", InController, false, false, 0, 1},
		{"__ZN17AMD9500Controller11turnOnDigonEb", "turnOnDigon", InController, false, false, 0, 1},
		// Acceleration (Phase 3): HWServices loads the HWLibs plug-in (CAIL, which has its own Stoney support: device
		// table row 0x98E4 -> STONEY_A0, Cail_Stoney_InitFunctionPointer), then the AMDRadeonX4000 accelerator brings up
		// the AMDBaffinHardware interface, its engines (PM4 = GFX/compute, sDMA, UVD, VCE, SAMU) and GPU VM.
		{"__ZN42AMDRadeonX4000_AMDRadeonHWServicesAbstract5probeEP9IOServicePi", "hws.probe", InHWServices, true, true, 0},
		{"__ZN37AMDRadeonX4000_AMDGraphicsAccelerator5probeEP9IOServicePi", "accel.probe", InAccel, true, true, 0},
		{"__ZN42AMDRadeonX4000_AMDRadeonHWServicesAbstract5startEP9IOService", "hws.start", InHWServices, true, true, 0},
		{"__ZN38AMDRadeonX4000_AMDRadeonHWServicesCAIL19createCailInterfaceEP11IOPCIDevice", "hws.createCailInterface", InHWServices, true, true, 0},
		{"__ZN42AMDRadeonX4000_AMDRadeonHWServicesAbstract24createPowerPlayInterfaceEP18PowerPlayCallbacks", "hws.createPowerPlay", InHWServices, true, true, 0},
		{"__ZN15AmdCailServices10initializeEP15_CailInitialize", "cail.initialize", InHWLibs, true, true, 0},
		{"__ZN15AmdCailServices17initializeAdapterEv", "cail.initializeAdapter", InHWLibs, true, true, 0},
		{"_CAILPostVBIOS", "CAILPostVBIOS", InHWLibs, true, true, 0},
		{"_CAILInitialize", "CAILInitialize", InHWLibs, true, true, 0},
		{"_CAILQueryASICInfo", "CAILQueryASICInfo", InHWLibs, true, false, 0},
		{"_Stoney_SetupASIC", "Stoney_SetupASIC", InHWLibs, true, true, 0},
		{"_Stoney_LoadRlcUcode", "Stoney_LoadRlcUcode", InHWLibs, true, true, 0},
		{"__ZN37AMDRadeonX4000_AMDGraphicsAccelerator5startEP9IOService", "accel.start", InAccel, true, true, 0},
		{"__ZN26AMDRadeonX4000_AMDHardware4initEP11IOPCIDeviceP28AMDRadeonX4000_IAMDHWHandlerRjP16_GART_PARAMETERSP14_FB_PARAMETERS", "hw.init", InAccel, true, true, 0},
		{"__ZN26AMDRadeonX4000_AMDHardware8initCAILEv", "hw.initCAIL", InAccel, true, true, 0},
		{"__ZN26AMDRadeonX4000_AMDHardware9setupCAILEv", "hw.setupCAIL", InAccel, true, true, 0},
		{"__ZN26AMDRadeonX4000_AMDHardware11powerUpCAILEv", "hw.powerUpCAIL", InAccel, true, true, 0},
		{"__ZN32AMDRadeonX4000_AMDBaffinHardware7powerUpEv", "hw.powerUp", InAccel, true, true, 0},
		{"__ZN26AMDRadeonX4000_AMDHardware23allocateMemoryResourcesEv", "hw.allocateMemoryResources", InAccel, true, true, 0},
		{"__ZN28AMDRadeonX4000_AMDVIHardware16initializeVMRegsEv", "hw.initializeVMRegs", InAccel, true, true, 0},
		{"__ZN26AMDRadeonX4000_AMDHardware27initializeHardwareRegistersEv", "hw.initializeHardwareRegisters", InAccel, true, true, 0},
		{"__ZN26AMDRadeonX4000_AMDHardware19initializeHWEnginesEv", "hw.initializeHWEngines", InAccel, true, true, 0},
		// (AMDHardware::powerUpHWEngines has its own wrapper, wrapPowerUpHWEngines.)
		{"__ZN30AMDRadeonX4000_AMDPM4HWChannel21infiniteWaitOnPowerUpEv", "pm4.infiniteWaitOnPowerUp", InAccel, true, false, 0},
		{"__ZN29AMDRadeonX4000_AMDVIPM4Engine7powerUpEv", "pm4.powerUp", InAccel, true, true, 0},
		{"__ZN29AMDRadeonX4000_AMDVIPM4Engine21initializeMicroEngineEv", "pm4.initializeMicroEngine", InAccel, true, true, 0},
		{"__ZN29AMDRadeonX4000_AMDVIPM4Engine5startEv", "pm4.start", InAccel, true, true, 0},
		{"__ZN30AMDRadeonX4000_AMDVIsDMAEngine5startEv", "sdma.start", InAccel, true, true, 0},
		{"__ZN26AMDRadeonX4000_AMDHardware28initializeExternalInterfacesEv", "hw.initializeExternalInterfaces", InAccel, true, true, 0},
		{"__ZN26AMDRadeonX4000_AMDHardware16enableInterruptsEv", "hw.enableInterrupts", InAccel, true, false, 0},
		// PowerPlay: AMD9500Controller::initializePowerPlay -> BaffinPowerPlayManager::initialize, which asks the
		// accelerator for its state (ATIController::sendRequestToAccelerator -> AMDGraphicsAccelerator::
		// callPlatformFunction), creates the HWLibs PowerPlay interface (AtiPowerPlayServices::ppInitialize ->
		// PP_InitializeEX -> PHM_Initialize, whose per-family back ends leave out Carrizo/Stoney), then updatePowerPlay.
		{"__ZN22BaffinPowerPlayManager10initializeEv", "ppm.initialize", InController, true, true, 0},
		{"__ZN22BaffinPowerPlayManager24createPowerPlayInterfaceEv", "ppm.createInterface", InController, true, false, 0},
		{"__ZN22BaffinPowerPlayManager15updatePowerPlayEv", "ppm.update", InController, true, false, 0},
		{"__ZN13ATIController24sendRequestToAcceleratorE25_eAMDAccelIOFBRequestTypePvS1_S1_", "toAccel", InSupport, false, false, 0, 1},
		{"__ZN37AMDRadeonX4000_AMDGraphicsAccelerator20callPlatformFunctionEPK8OSSymbolbPvS3_S3_S3_", "accel.cpf", InAccel, false, false, 0},
		{"__ZN20AtiPowerPlayServices12ppInitializeEP22PPDisplayConfiguration", "pp.ppInitialize", InHWLibs, true, true, 0},
		{"_PP_InitializeEX", "PP_InitializeEX", InHWLibs, true, false, 0},
		{"_PHM_Initialize", "PHM_Initialize", InHWLibs, true, false, 0},
		// PHM_Initialize with the Dummy back end (nvram36: it returned 2) runs, in order and stopping at the first that
		// doesn't return 1: the PowerPlay table parser (PP_Tables_Initialize for families up to 0x8E, i.e. the VBIOS
		// PowerPlayInfo table), the dummy thermal controller, PhwDummy_Initialize (for family 0x87 it reads the APU's
		// system info table), the common hardware manager (some ASICs) and PHM_VerifyHwMgr.
		{"_PP_Tables_Initialize", "PP_Tables_Initialize", InHWLibs, true, false, 0},
		{"_PP_ThermalCtrl_Dummy_Initialize", "PP_ThermalCtrl_Dummy_Initialize", InHWLibs, true, false, 0},
		{"_PhwDummy_Initialize", "PhwDummy_Initialize", InHWLibs, true, false, 0},
		{"_Phw_CommonHwMgr_Initialize", "Phw_CommonHwMgr_Initialize", InHWLibs, true, false, 0},
		{"_PHM_VerifyHwMgr", "PHM_VerifyHwMgr", InHWLibs, true, false, 0},
		// The real back ends PHM_Initialize picks by family and revision (family 0x82, VI: Iceland, Tonga, Fiji,
		// Polaris 10/11/12, Polaris 22). On the APU spoofed as Polaris 11 the Polaris one would run.
		{"_PhwPolaris10_Initialize", "PhwPolaris10_Initialize", InHWLibs, true, false, 0},
		{"_PhwPolaris22_Initialize", "PhwPolaris22_Initialize", InHWLibs, true, false, 0},
		{"_PhwTonga_Initialize", "PhwTonga_Initialize", InHWLibs, true, false, 0},
		// CAIL's RLC and gating set-up for Stoney (nvram49-66: the RLC is enabled but never answers). Linux
		// gfx_v8_0_init_pg on Carrizo/Stoney writes the CSIB, the save/restore list and machine, the CP jump-table
		// restore address, the PG parameters and the always-on CU mask before starting the RLC; CAIL has its own
		// versions of each. Which run, and when, relative to Stoney_LoadRlcUcode and pm4.start?
		{"_Cail_Tonga_InitCSBHeader", "Cail_Tonga_InitCSBHeader", InHWLibs, true, false, 0},
		{"_Cail_Stoney_InitializePowerGating", "Cail_Stoney_InitializePowerGating", InHWLibs, true, false, 0},
		{"_stoney_init_power_gating", "stoney_init_power_gating", InHWLibs, true, false, 0},
		{"_Cail_Tonga_InitRlcSaveRestoreList", "Cail_Tonga_InitRlcSaveRestoreList", InHWLibs, true, false, 0},
		{"_Cail_Stoney_EnablePowerGating", "Cail_Stoney_EnablePowerGating", InHWLibs, true, false, 0},
		{"_Stoney_UpdateGfxPowerGating", "Stoney_UpdateGfxPowerGating", InHWLibs, true, false, 0},
		{"_Stoney_EnableCpPowerGating", "Stoney_EnableCpPowerGating", InHWLibs, true, false, 0},
		{"_Stoney_UpdateCoarseGrainClockGating", "Stoney_UpdateCoarseGrainClockGating", InHWLibs, true, false, 0},
		{"_Stoney_UpdateMediumGrainClockGating", "Stoney_UpdateMediumGrainClockGating", InHWLibs, true, false, 0},
		{"_Cail_Fiji_EnableLBPW", "Cail_Fiji_EnableLBPW", InHWLibs, true, false, 0},
		{"_stoney_init_LBPW", "stoney_init_LBPW", InHWLibs, true, false, 0},
		{"_Cail_Tonga_MicroEngineControlCp", "Cail_Tonga_MicroEngineControlCp", InHWLibs, true, false, 0},
		{"_Cail_Tonga_MicroEngineControlMec", "Cail_Tonga_MicroEngineControlMec", InHWLibs, true, false, 0},
		{"_Cail_Tonga_MicroEngineControlSdma", "Cail_Tonga_MicroEngineControlSdma", InHWLibs, true, false, 0},
		{"_CAILEarlyASICInit", "CAILEarlyASICInit", InHWLibs, true, false, 0},
		{"_Cail_Stoney_InitCPJumpTable", "Cail_Stoney_InitCPJumpTable", InHWLibs, true, false, 0},
		{"_InitializeRlcHistoryBuffer", "InitializeRlcHistoryBuffer", InHWLibs, true, false, 0},
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
		uint64_t ab = reinterpret_cast<uint64_t>(b), ac = reinterpret_cast<uint64_t>(c), ad = reinterpret_cast<uint64_t>(d);
		uint32_t arg = 0;
		switch (stages[N].arg) {
			case 1: arg = static_cast<uint32_t>(ab); break;
			case 2: arg = static_cast<uint32_t>(ac); break;
			case 3: arg = static_cast<uint32_t>((ab & 0xFF) | (ac & 0xFF) << 8); break;
			case 4: arg = static_cast<uint32_t>((ac & 0xFF) | (ad & 0xFF) << 8); break;
			case 5: arg = static_cast<uint32_t>(ac); break;
		}
		traceEvent(stages[N].name, false, arg);
		if (stages[N].sync) {
			IOLog("CarrizoRed: => %s\n", stages[N].name);
			IOSleep(30);
		}
		auto ret = FunctionCast(wrapStage<N>, stages[N].org)(a, b, c, d, e, f);
		traceEvent(stages[N].name, true, static_cast<uint32_t>(ret));
		// Mode 5: d points at a status buffer the call fills (DP training: per-lane status read from the DPCD).
		if (stages[N].arg == 5 && d)
			traceEvent("  status", true, *static_cast<uint32_t *>(d));
		__atomic_fetch_sub(&stageOpen[N], 1, __ATOMIC_RELAXED);
		if (stages[N].summary)
			traceNote("%s=%x ", stages[N].name, static_cast<uint32_t>(ret));
		return ret;
	}

#define CR_WRAP(n) reinterpret_cast<mach_vm_address_t>(wrapStage<n>)
	void routeStages(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size, TraceKext kext) {
		static_assert(arrsize(stages) == 123, "one wrapper per stage");
		mach_vm_address_t wrappers[] {
			CR_WRAP(0), CR_WRAP(1), CR_WRAP(2), CR_WRAP(3), CR_WRAP(4), CR_WRAP(5), CR_WRAP(6), CR_WRAP(7),
			CR_WRAP(8), CR_WRAP(9), CR_WRAP(10), CR_WRAP(11), CR_WRAP(12), CR_WRAP(13), CR_WRAP(14), CR_WRAP(15),
			CR_WRAP(16), CR_WRAP(17), CR_WRAP(18), CR_WRAP(19), CR_WRAP(20), CR_WRAP(21), CR_WRAP(22), CR_WRAP(23),
			CR_WRAP(24), CR_WRAP(25), CR_WRAP(26), CR_WRAP(27), CR_WRAP(28), CR_WRAP(29), CR_WRAP(30), CR_WRAP(31),
			CR_WRAP(32), CR_WRAP(33), CR_WRAP(34), CR_WRAP(35), CR_WRAP(36), CR_WRAP(37), CR_WRAP(38), CR_WRAP(39),
			CR_WRAP(40), CR_WRAP(41), CR_WRAP(42), CR_WRAP(43), CR_WRAP(44), CR_WRAP(45), CR_WRAP(46), CR_WRAP(47),
			CR_WRAP(48), CR_WRAP(49), CR_WRAP(50), CR_WRAP(51), CR_WRAP(52), CR_WRAP(53), CR_WRAP(54), CR_WRAP(55),
			CR_WRAP(56), CR_WRAP(57), CR_WRAP(58), CR_WRAP(59), CR_WRAP(60), CR_WRAP(61), CR_WRAP(62), CR_WRAP(63),
			CR_WRAP(64), CR_WRAP(65), CR_WRAP(66), CR_WRAP(67), CR_WRAP(68), CR_WRAP(69), CR_WRAP(70), CR_WRAP(71),
			CR_WRAP(72), CR_WRAP(73), CR_WRAP(74), CR_WRAP(75), CR_WRAP(76), CR_WRAP(77), CR_WRAP(78), CR_WRAP(79),
			CR_WRAP(80), CR_WRAP(81), CR_WRAP(82), CR_WRAP(83), CR_WRAP(84), CR_WRAP(85), CR_WRAP(86), CR_WRAP(87),
			CR_WRAP(88), CR_WRAP(89), CR_WRAP(90), CR_WRAP(91), CR_WRAP(92), CR_WRAP(93), CR_WRAP(94), CR_WRAP(95),
			CR_WRAP(96), CR_WRAP(97), CR_WRAP(98), CR_WRAP(99), CR_WRAP(100), CR_WRAP(101), CR_WRAP(102),
			CR_WRAP(103), CR_WRAP(104), CR_WRAP(105),
			CR_WRAP(106), CR_WRAP(107), CR_WRAP(108), CR_WRAP(109), CR_WRAP(110), CR_WRAP(111), CR_WRAP(112), CR_WRAP(113),
			CR_WRAP(114), CR_WRAP(115), CR_WRAP(116), CR_WRAP(117), CR_WRAP(118), CR_WRAP(119), CR_WRAP(120), CR_WRAP(121), CR_WRAP(122),
		};
		for (size_t i = 0; i < arrsize(stages); i++) {
			if (stages[i].kext != kext)
				continue;
			KernelPatcher::RouteRequest request {stages[i].symbol, wrappers[i], stages[i].org};
			if (!routeChecked(patcher, index, request, slide, size)) {
				SYSLOG("cr", "trace: failed to route %s (error %d)", stages[i].name, patcher.getError());
				traceNote("noroute:%s(%d) ", stages[i].name, patcher.getError());
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

// --- Acceleration (AMDRadeonX4000) ----------------------------------------------------------------------------------
// HWServices loads AMDRadeonX4000HWLibs (the "Load4000" default for a non-Apple subsystem ID), whose CAIL has full Stoney
// support and reads the real device ID from config space. The accelerator itself (AMDBaffinGraphicsAccelerator ->
// AMDBaffinHardware) assumes a Polaris 11:
//  - allocateHWEngines creates two sDMA engines (+0x3B8, +0x3C0); Stoney has one (Linux sdma_v3_0, 1 instance). The
//    second slot is released again, and startHWEngines, which requires both, starts only the first. initializeHWEngines
//    skips empty engine slots, and getHWChannel(engine, ring) returns NULL for one, but createAccelChannels spreads the
//    sDMA channels over both engines and calls into the result unchecked; engine 2 (sDMA1) is redirected to engine 1.
//  - setupAndInitializeHWCapabilities hard-codes 2 shader engines (+0x64), 1 SH per SE (+0x6C) and 8 CUs per SH (+0xA0)
//    (Ellesmere: 4/1/9). The VI base class then reads the real active-CU bitmaps from CAIL into +0x70.. and the
//    minimum active CUs into +0xA4. Stoney: 1 SE, 1 SH, 3 CUs.
//  - Local (non-system) page-table entries hold a VRAM address. On a dGPU that is an FB offset; on an APU the "VRAM" is
//    a carve-out in system RAM and the entries need its physical address, MC_VM_FB_OFFSET << 22 (Linux gmc_v8_0:
//    vm_manager.vram_base_offset, gmc_v8_0_get_vm_pde). PTEs are written by sDMA (updateContiguousPTEsWithDMAUsingAddr:
//    base address + PTE flags template) or built by getPTEValue / getPDEValue; both are converted. -crnovmfix disables.
// Field offsets come from the 15.8.1 KDK; each patch first checks the instructions that carry them (the D509BA boots
// the 15.4 recovery) and stays off if they differ.
static bool accelSdma1Dropped, accelCapsFixed, vmFixOn, vmFixReady;
static void *accelHardware;  // AMDBaffinHardware, from allocateHWEngines (for the black box's ring dump)
static uint32_t accelMmDropped, accelArbPatched;
static uint32_t accelStartPatched, accelCapsCu, accelSdma1Aliased;
static uint64_t vmFbStart, vmFbEnd, vmCarveout;
static uint32_t vmConverted, vmMcAddresses, vmOffsets, vmOther;
static struct {
	uint64_t from, to;
	uint8_t kind;  // 'D' sDMA update, 'T' getPTEValue, 'P' getPDEValue
} vmSamples[4];

static bool codeHas(mach_vm_address_t fn, size_t len, const char *hex) {
	uint8_t pattern[16];
	size_t n = 0;
	for (; hex[2 * n] && n < sizeof(pattern); n++) {
		auto nib = [](char c) { return static_cast<uint8_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10); };
		pattern[n] = static_cast<uint8_t>(nib(hex[2 * n]) << 4 | nib(hex[2 * n + 1]));
	}
	auto *code = reinterpret_cast<const uint8_t *>(fn);
	for (size_t i = 0; fn && i + n <= len; i++)
		if (!memcmp(code + i, pattern, n))
			return true;
	return false;
}

template <typename T>
static T vfunc(void *object, size_t offset) {
	return reinterpret_cast<T>((*reinterpret_cast<uintptr_t **>(object))[offset / sizeof(uintptr_t)]);
}

static bool vmFixInit() {
	if (vmFixReady || !vmFixOn || !gpuMmio)
		return vmFixReady;
	uint32_t location = gpuMmio[0x809], offset = gpuMmio[0x81A];  // MC_VM_FB_LOCATION, MC_VM_FB_OFFSET
	vmFbStart = static_cast<uint64_t>(location & 0xFFFF) << 24;
	vmFbEnd = static_cast<uint64_t>((location >> 16) & 0xFFFF) << 24 | 0xFFFFFF;
	vmCarveout = static_cast<uint64_t>(offset & 0x3FFFF) << 22;
	vmFixReady = true;
	if (!vmCarveout || vmFbEnd <= vmFbStart)
		vmFixOn = false;  // not an APU carve-out: leave the entries alone
	return vmFixReady;
}

// VRAM address in a page-table entry -> the carve-out's physical address. MC addresses (inside MC_VM_FB_LOCATION) and
// FB offsets (below the FB size) are converted; anything else (already physical, or system memory) is left alone.
static uint64_t vmToPhysical(uint64_t address, uint8_t kind) {
	if (!vmFixInit() || !vmFixOn)
		return address;
	uint64_t result = address;
	if (address >= vmFbStart && address <= vmFbEnd) {
		result = address - vmFbStart + vmCarveout;
		__atomic_fetch_add(&vmMcAddresses, 1, __ATOMIC_RELAXED);
	} else if (address <= vmFbEnd - vmFbStart) {
		result = address + vmCarveout;
		__atomic_fetch_add(&vmOffsets, 1, __ATOMIC_RELAXED);
	} else {
		__atomic_fetch_add(&vmOther, 1, __ATOMIC_RELAXED);
	}
	uint32_t n = __atomic_fetch_add(&vmConverted, 1, __ATOMIC_RELAXED);
	if (n < arrsize(vmSamples))
		vmSamples[n] = {address, result, kind};
	return result;
}

static mach_vm_address_t orgPowerUpHWEngines;
static bool wrapPowerUpHWEngines(void *that);

void CRDriver::patchAccelerator(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size) {
	auto route = [&](const char *symbol, mach_vm_address_t wrapper, mach_vm_address_t &org, const char *const *checks,
					 size_t checkLen, const char *what) {
		mach_vm_address_t fn = patcher.solveSymbol(index, symbol, slide, size);
		if (!fn) {
			SYSLOG("cr", "accel: %s not found (error %d)", what, patcher.getError());
			patcher.clearError();
			return false;
		}
		for (size_t i = 0; checks && checks[i]; i++) {
			if (!codeHas(fn, checkLen, checks[i])) {
				IOLog("CarrizoRed: accel: %s left alone, its code differs from the 15.8.1 KDK (%s)\n", what, checks[i]);
				traceNote("%s=nomatch ", what);
				return false;
			}
		}
		KernelPatcher::RouteRequest request {symbol, wrapper, org};
		if (!routeChecked(patcher, index, request, slide, size)) {
			SYSLOG("cr", "accel: failed to route %s (error %d)", what, patcher.getError());
			patcher.clearError();
			return false;
		}
		return true;
	};

	if (family == ChipFamily::Stoney && !checkKernelArgument("-crnosdmafix")) {
		static const char *const allocChecks[] {"4c89b3b8030000", "4c89b3c0030000", nullptr};
		static const char *const startChecks[] {"4a8bbcf3b8030000", "ff9048010000", "488b7b18", "ff9078010000",
			"66895008", nullptr};
		if (route("__ZN32AMDRadeonX4000_AMDBaffinHardware17allocateHWEnginesEv",
				reinterpret_cast<mach_vm_address_t>(wrapAllocateHWEngines), orgAllocateHWEngines, allocChecks, 0x100,
				"allocateHWEngines")) {
			route("__ZN26AMDRadeonX4000_AMDHardware14startHWEnginesEv",
				reinterpret_cast<mach_vm_address_t>(wrapStartHWEngines), orgStartHWEngines, startChecks, 0xA0,
				"startHWEngines");
		}
		static const char *const powerUpChecks[] {"4b8bbcfeb0030000", nullptr};
		route("__ZN26AMDRadeonX4000_AMDHardware16powerUpHWEnginesEv",
			reinterpret_cast<mach_vm_address_t>(wrapPowerUpHWEngines), orgPowerUpHWEngines, powerUpChecks, 0x30,
			"powerUpHWEngines");
		static const char *const channelChecks[] {"488bbcc7b0030000", "488b80a0010000", nullptr};
		route("__ZN26AMDRadeonX4000_AMDHardware12getHWChannelE20_eAMD_HW_ENGINE_TYPE18_eAMD_HW_RING_TYPE",
			reinterpret_cast<mach_vm_address_t>(wrapGetHWChannel), orgGetHWChannel, channelChecks, 0x26,
			"getHWChannel");
	}
	// AMDBaffinHardware::powerUp (and AMDEllesmereHardware's copy) ORs MC_ARB_MISC2.SEQ_RDY_POP_IDLE (0x40000000) and
	// POP_IDLE_REPLAY (0x800) into the memory arbiter: Polaris GDDR5 sequencer tweaks, which Linux never sets on an APU,
	// whose arbiter the CPU shares. Make both ORs write 0 (mov esi, 0x9D5; mov edx, imm; call [rax + 0x128]).
	// -crnoarbfix leaves them.
	if ((family == ChipFamily::Stoney || family == ChipFamily::Carrizo) && !checkKernelArgument("-crnoarbfix")) {
		static const uint8_t find1[] {0xBE, 0xD5, 0x09, 0x00, 0x00, 0xBA, 0x00, 0x00,
			0x00, 0x40, 0xFF, 0x90, 0x28, 0x01, 0x00, 0x00};
		static const uint8_t repl1[] {0xBE, 0xD5, 0x09, 0x00, 0x00, 0xBA, 0x00, 0x00,
			0x00, 0x00, 0xFF, 0x90, 0x28, 0x01, 0x00, 0x00};
		static const uint8_t find2[] {0xBE, 0xD5, 0x09, 0x00, 0x00, 0xBA, 0x00, 0x08,
			0x00, 0x00, 0xFF, 0x90, 0x28, 0x01, 0x00, 0x00};
		static const uint8_t repl2[] {0xBE, 0xD5, 0x09, 0x00, 0x00, 0xBA, 0x00, 0x00,
			0x00, 0x00, 0xFF, 0x90, 0x28, 0x01, 0x00, 0x00};
		KernelPatcher::LookupPatch patches[] {
			{&kexts[KextRadeonX4000], find1, repl1, sizeof(find1), 2},
			{&kexts[KextRadeonX4000], find2, repl2, sizeof(find2), 2},
		};
		for (auto &patch : patches) {
			patcher.applyLookupPatch(&patch);
			if (patcher.getError() == KernelPatcher::Error::NoError) {
				accelArbPatched++;
			} else {
				traceNote("arbfix=err%d ", patcher.getError());
				patcher.clearError();
			}
		}
	}
	if (family == ChipFamily::Stoney && !checkKernelArgument("-crnocapsfix")) {
		static const char *const capsChecks[] {"c7476402000000", "c7476c01000000", "c787a000000008000000", nullptr};
		route("__ZN32AMDRadeonX4000_AMDBaffinHardware32setupAndInitializeHWCapabilitiesEv",
			reinterpret_cast<mach_vm_address_t>(wrapSetupHWCapabilities), orgSetupHWCapabilities, capsChecks, 0x30,
			"setupAndInitializeHWCapabilities");
	}
	vmFixOn = !checkKernelArgument("-crnovmfix");
	if (vmFixOn) {
		route("__ZN29AMDRadeonX4000_AMDHWVMContext36updateContiguousPTEsWithDMAUsingAddrEyyyyy",
			reinterpret_cast<mach_vm_address_t>(wrapUpdatePTEsDMA), orgUpdatePTEsDMA, nullptr, 0,
			"updateContiguousPTEsWithDMAUsingAddr");
		route("__ZN23AMDRadeonX4000_AMDVIVMM11getPTEValueE15eAMD_VMPT_LEVELyN24AMDRadeonX4000_IAMDHWVMM10VmMapFlagsEj",
			reinterpret_cast<mach_vm_address_t>(wrapGetPTEValue), orgGetPTEValue, nullptr, 0, "getPTEValue");
		route("__ZN23AMDRadeonX4000_AMDVIVMM11getPDEValueE15eAMD_VMPT_LEVELy",
			reinterpret_cast<mach_vm_address_t>(wrapGetPDEValue), orgGetPDEValue, nullptr, 0, "getPDEValue");
	}
}

bool CRDriver::wrapAllocateHWEngines(void *that) {
	bool ret = FunctionCast(wrapAllocateHWEngines, callback->orgAllocateHWEngines)(that);
	accelHardware = that;
	auto &sdma1 = *reinterpret_cast<OSObject **>(static_cast<uint8_t *>(that) + 0x3C0);
	if (sdma1) {
		sdma1->release();
		sdma1 = nullptr;
		accelSdma1Dropped = true;
	}
	traceEvent("hw.allocateHWEngines (sDMA1 dropped)", true, ret);
	return ret;
}

// UVD 6.3 (+0x3D8), VCE 3.4 (+0x3E8) and SAMU (+0x400): on the APU the SMU keeps them power-gated until a PowerPlay
// back end asks it to power them up (Linux smu8: PPSMC_MSG_UVDPowerON etc.); the Dummy back end never does, and
// programming a gated block can hang the bus. Releasing the engines made AMDGraphicsAccelerator::start fail (nvram68,
// then a NULL dereference in its clean-up path), so they stay; only their power-up is skipped: the slots are emptied
// for the duration of AMDHardware::powerUpHWEngines, whose loop skips empty slots. -crmm powers them up too.
static bool wrapPowerUpHWEngines(void *that) {
	static const uint32_t multimedia[] {0x3D8, 0x3E8, 0x400};
	void *saved[arrsize(multimedia)] {};
	bool skip = !checkKernelArgument("-crmm");
	if (skip) {
		for (size_t i = 0; i < arrsize(multimedia); i++) {
			auto &engine = *reinterpret_cast<void **>(static_cast<uint8_t *>(that) + multimedia[i]);
			saved[i] = engine;
			if (engine)
				accelMmDropped++;
			engine = nullptr;
		}
	}
	traceEvent("hw.powerUpHWEngines", false, 0);
	bool ret = FunctionCast(wrapPowerUpHWEngines, orgPowerUpHWEngines)(that);
	if (skip)
		for (size_t i = 0; i < arrsize(multimedia); i++)
			*reinterpret_cast<void **>(static_cast<uint8_t *>(that) + multimedia[i]) = saved[i];
	traceEvent("hw.powerUpHWEngines", true, ret);
	traceNote("hw.powerUpHWEngines=%x(mm skipped %u) ", ret, skip ? accelMmDropped : 0);
	return ret;
}

// AMDHardware::startHWEngines starts both sDMA engines and fails if either is missing; with one engine, start it and set
// the same "sDMA started" bit (bit 9 of the 16-bit word at +8 of the info block from this+0x18, vtable +0x178).
bool CRDriver::wrapStartHWEngines(void *that) {
	auto *self = static_cast<uint8_t *>(that);
	if (*reinterpret_cast<void **>(self + 0x3C0))
		return FunctionCast(wrapStartHWEngines, callback->orgStartHWEngines)(that);
	void *sdma0 = *reinterpret_cast<void **>(self + 0x3B8);
	bool ok = sdma0 && vfunc<bool (*)(void *)>(sdma0, 0x148)(sdma0);
	void *info = *reinterpret_cast<void **>(self + 0x18);
	auto *state = vfunc<uint8_t *(*)(void *)>(info, 0x178)(info);
	uint32_t word = *reinterpret_cast<uint32_t *>(state + 8);
	*reinterpret_cast<uint16_t *>(state + 8) = static_cast<uint16_t>((word & 0xFFFFFDFF) | (ok ? 0x200 : 0));
	accelStartPatched++;
	traceEvent("hw.startHWEngines (sDMA0 only)", true, ok);
	traceNote("startHWEngines=%x ", ok);
	return ok;
}

// Engine types: 0 PM4, 1 sDMA0, 2 sDMA1 (slots +0x3B0 + 8 * type). With sDMA1 dropped, its channels are sDMA0's.
void *CRDriver::wrapGetHWChannel(void *that, uint32_t engine, uint32_t ring) {
	auto org = FunctionCast(wrapGetHWChannel, callback->orgGetHWChannel);
	void *channel = org(that, engine, ring);
	if (!channel && engine == 2 && accelSdma1Dropped) {
		channel = org(that, 1, ring);
		if (!accelSdma1Aliased++)
			traceNote("sdma1->sdma0 ");
	}
	return channel;
}

uint64_t CRDriver::wrapSetupHWCapabilities(void *that) {
	uint64_t ret = FunctionCast(wrapSetupHWCapabilities, callback->orgSetupHWCapabilities)(that);
	auto *self = static_cast<uint8_t *>(that);
	uint32_t activeCu = *reinterpret_cast<uint32_t *>(self + 0xA4);  // minimum active CUs per SH, from CAIL
	*reinterpret_cast<uint32_t *>(self + 0x64) = 1;                   // shader engines
	*reinterpret_cast<uint32_t *>(self + 0x6C) = 1;                   // SH per SE
	if (activeCu)
		*reinterpret_cast<uint32_t *>(self + 0xA0) = activeCu;        // CUs per SH
	accelCapsFixed = true;
	accelCapsCu = activeCu;
	traceEvent("hw.capabilities (1 SE, active CUs)", true, activeCu);
	return ret;
}

uint64_t CRDriver::wrapUpdatePTEsDMA(void *that, uint64_t dst, uint64_t count, uint64_t addr, uint64_t flags,
	uint64_t incr) {
	if ((flags & 1) && !(flags & 2))  // valid, not system memory
		addr = vmToPhysical(addr, 'D');
	return FunctionCast(wrapUpdatePTEsDMA, callback->orgUpdatePTEsDMA)(that, dst, count, addr, flags, incr);
}

uint64_t CRDriver::wrapGetPTEValue(void *that, uint32_t level, uint64_t addr, uint32_t flags, uint32_t fragment) {
	uint64_t pte = FunctionCast(wrapGetPTEValue, callback->orgGetPTEValue)(that, level, addr, flags, fragment);
	constexpr uint64_t mask = 0xFFFFFFF000;
	if (addr && (pte & 1) && !(pte & 2))  // address 0 = a flags template for sDMA updates
		pte = (pte & ~mask) | (vmToPhysical(addr & mask, 'T') & mask);
	return pte;
}

uint64_t CRDriver::wrapGetPDEValue(void *that, uint32_t level, uint64_t addr) {
	uint64_t pde = FunctionCast(wrapGetPDEValue, callback->orgGetPDEValue)(that, level, addr);
	constexpr uint64_t mask = 0xFFFFFFFFC0;
	if (addr)
		pde = (pde & ~mask) | (vmToPhysical(addr & mask, 'P') & mask);
	return pde;
}

// PowerPlay registry reads (HWLibs PECI_ReadRegistry / PECI_GetRegistrySize -> PowerPlay callbacks -> the iGPU's
// properties). nvram40: with PP_PhmUseDummyBackEnd and PP_PhmSoftPowerPlayTable set, PHM_Initialize still took the
// non-Dummy path and never called PP_Tables_Initialize. Note what the PowerPlay keys that matter read back as.
static mach_vm_address_t orgPeciReadRegistry, orgPeciGetRegistrySize;
static uint32_t ppRegNotes, peciReads;

static bool ppRegInteresting(const char *name) {
	return name && (strstr(name, "Dummy") || strstr(name, "Soft") || strstr(name, "DisableThermal"));
}

// nvram41: the soft table's size read back right (0x1EF) but PP_PhmUseDummyBackEnd read as 0 although the iGPU has it
// as 01 00 00 00, so PHM_Initialize took the VI path (the Tonga table parser and Polaris back end). Answer that key
// here; the note still shows what the registry gave. -crnoppdummy disables both.
static bool ppDummyForced;

static uint32_t wrapPeciReadRegistry(void *peci, const char *name, uint32_t *value, uint32_t fallback) {
	auto ret = FunctionCast(wrapPeciReadRegistry, orgPeciReadRegistry)(peci, name, value, fallback);
	peciReads++;
	if (ppRegInteresting(name) && ppRegNotes++ < 12)
		traceNote("reg:%s=%x(r%x) ", name, value ? *value : 0xFFFFFFFF, ret);
	if (ppDummyForced && name && value && !strcmp(name, "PP_PhmUseDummyBackEnd") && *value != 1) {
		*value = 1;
		ret = 1;
		traceNote("->1 ");
	}
	return ret;
}

static uint32_t wrapPeciGetRegistrySize(void *peci, const char *name, uint32_t *size) {
	auto ret = FunctionCast(wrapPeciGetRegistrySize, orgPeciGetRegistrySize)(peci, name, size);
	if (ppRegInteresting(name) && ppRegNotes++ < 12)
		traceNote("regsize:%s=%x(r%x) ", name, size ? *size : 0xFFFFFFFF, ret);
	return ret;
}

// Microcode loading. CAIL's flags word (+0x6F0, from CailReadinOverrideRegistrySetting) has DisableCailLoadUcode
// (0x200000), default 1: CAIL leaves the RLC, CP and SDMA microcode to the SMU firmware and waits for it
// (Stoney_LoadRlcUcode -> Cail_MCILWaitForFwLoadFinished, asking PowerPlay). The Dummy PowerPlay back end never asks
// the SMU to load anything, so pm4.powerUp waited forever (nvram43: Stoney_LoadRlcUcode open). Clear the flag so CAIL
// writes the microcode embedded in HWLibs (Stoney_RLC_UCODE, _ME_, _PFP_, _CE_, _MEC1_, _SDMA0_) itself, as Linux
// amdgpu does with fw_load_type=0 (direct loading, supported on Carrizo/Stoney). -crnodirectucode disables this.
static mach_vm_address_t orgCailReadinOverrideRegistrySetting;

// CAIL register waits (Cail_MCILWaitFor: an array of {register, mask, value} conditions, timeout in ms, 3 s by default)
// that fail. nvram48: the RLC start-up (Stoney_LoadRlcUcode) sat in them; record which register never reached its
// value and what it read instead.
static mach_vm_address_t orgCailWaitFor;
static uint32_t cailWaits, cailWaitFailures;
static struct {
	uint32_t reg, mask, value, read, ret;
	uint64_t ms;
} cailWaitFails[12];

static uint64_t wrapCailWaitFor(void *cail, void *conditions, uint64_t count, uint64_t c, uint64_t flags, uint64_t timeout,
	uint64_t g) {
	auto ret = FunctionCast(wrapCailWaitFor, orgCailWaitFor)(cail, conditions, count, c, flags, timeout, g);
	__atomic_fetch_add(&cailWaits, 1, __ATOMIC_RELAXED);
	if (static_cast<uint32_t>(ret) && conditions) {
		uint32_t n = __atomic_fetch_add(&cailWaitFailures, 1, __ATOMIC_RELAXED);
		if (n < arrsize(cailWaitFails)) {
			auto *condition = static_cast<uint32_t *>(conditions);
			uint32_t reg = condition[0];
			cailWaitFails[n] = {reg, condition[2], condition[3], gpuMmio && reg < 0x10000 ? gpuMmio[reg] : 0xFFFFFFFF,
				static_cast<uint32_t>(ret), uptimeMs()};
		}
	}
	return ret;
}
static uint32_t cailFlagsBefore, cailFlagsAfter;

static uint64_t wrapCailReadinOverrideRegistrySetting(void *cail) {
	auto ret = FunctionCast(wrapCailReadinOverrideRegistrySetting, orgCailReadinOverrideRegistrySetting)(cail);
	auto &flags = *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(cail) + 0x6F0);
	cailFlagsBefore = flags;
	flags &= ~0x200000U;
	cailFlagsAfter = flags;
	traceNote("cailflags=%x->%x ", cailFlagsBefore, cailFlagsAfter);
	return ret;
}

// RLC start-up for direct loading. CAIL's Stoney set-up runs Linux's sequence (halt, soft reset, init_pg) early, and
// Stoney_LoadRlcUcode later halts the RLC, loads its microcode and starts it. The RLC still never answers a safe-mode
// request (nvram49-66), so nothing it would manage may stay enabled: when CAIL halts the RLC for loading, turn off
// coarse-grain clock gating and light sleep (RLC_CGCG_CGLS_CTRL bits 0-1), the MGCG paths and memory light sleep, and
// CP power gating (RLC_PG_CNTL.CP_PG_DISABLE; it was 0, so an idle CP could be gated with no RLC to wake it, matching a
// CPF stuck in POST_WPTR_GFX_BUSY after the first fetch). -crnorlcreset disables this.
static mach_vm_address_t orgStoneyHaltRlc;
static uint32_t rlcResets, rlcCgcgBefore, rlcPgBefore, rlcMgcgBefore;

static uint64_t wrapStoneyHaltRlc(void *cail) {
	auto ret = FunctionCast(wrapStoneyHaltRlc, orgStoneyHaltRlc)(cail);
	if (auto *mmio = gpuMmio) {
		constexpr uint32_t GRBM_SOFT_RESET = 0x2008, RLC_PG_CNTL = 0xEC43, RLC_CGCG_CGLS_CTRL = 0xEC49;
		rlcCgcgBefore = mmio[RLC_CGCG_CGLS_CTRL];
		rlcPgBefore = mmio[RLC_PG_CNTL];
		mmio[RLC_CGCG_CGLS_CTRL] = rlcCgcgBefore & ~3U;
		mmio[RLC_PG_CNTL] = 0;
		// Nothing may gate the CP while the RLC isn't answering: as Linux's "clock gating off" paths do, force the
		// MGCG overrides (CPF, RLC, MGCG, CGCG, CGLS, GRBM), turn off CP and RLC memory light sleep, and set
		// RLC_PG_CNTL.CP_PG_DISABLE. -crgating leaves CAIL's settings.
		if (!checkKernelArgument("-crgating")) {
			constexpr uint32_t RLC_MEM_SLP_CNTL = 0xEC06, RLC_CGTT_MGCG_OVERRIDE = 0xEC48, CP_MEM_SLP_CNTL = 0x3079;
			rlcMgcgBefore = mmio[RLC_CGTT_MGCG_OVERRIDE];
			mmio[RLC_CGTT_MGCG_OVERRIDE] = rlcMgcgBefore | 0x3F;
			mmio[RLC_MEM_SLP_CNTL] = mmio[RLC_MEM_SLP_CNTL] & ~1U;
			mmio[CP_MEM_SLP_CNTL] = mmio[CP_MEM_SLP_CNTL] & ~1U;
			mmio[RLC_PG_CNTL] = 0x8000;
		}
		// CAIL's own Stoney set-up (StoneyAsicSetupTable: halt_rlc, reset_rlc = this same soft reset, then
		// init_power_gating = CSIB, save/restore list, jump-table restore, AO CU mask) already ran; a second reset
		// here, after init_power_gating, could only undo it. Opt-in with -crrlcreset.
		if (checkKernelArgument("-crrlcreset")) {
			uint32_t reset = mmio[GRBM_SOFT_RESET];
			mmio[GRBM_SOFT_RESET] = reset | 4;
			(void)mmio[GRBM_SOFT_RESET];
			IODelay(50);
			mmio[GRBM_SOFT_RESET] = reset & ~4U;
			(void)mmio[GRBM_SOFT_RESET];
			IODelay(50);
			rlcResets++;
		}
	}
	return ret;
}

// ATS / PRI / PASID. CAIL's Stoney set-up ends with stoney_init_enable_ats -> Cail_Spectre_EnableAts, which turns on
// the iGPU's own PCIe ATS (0x2B6 = 0x8000), PRI (0x2C4/0x2CC) and PASID (0x2D6) capabilities (nvram69: all three on).
// They belong to an IOMMUv2 driver (Linux amd_iommu_v2 / amdkfd; amdgpu never sets them itself), which macOS doesn't
// have for AMD; GPU reads that need a translation would never be answered (nvram69: reads stuck in the memory
// arbiter). Leave them off. -crats keeps CAIL's behaviour.
static mach_vm_address_t orgCailEnableAts;
static uint32_t atsSkipped;

static uint64_t wrapCailEnableAts(void *) {
	atsSkipped++;
	traceNote("ats=skipped ");
	return 0;
}

// SMU microcode loading (port of Linux smu8_smumgr.c, MIT). nvram71/72: the GPU's microcode RAMs ignore direct writes
// (RLC and PFP word 0 read back random values after a write) and the SMU firmware header's UcodeLoadStatus is 0. On
// Carrizo and Stoney only the SMU can load the RLC, CP (CE, PFP, ME, MEC jump table) and SDMA microcode. Windows does
// it in PowerPlay's SMU8 manager, which Apple's HWLibs doesn't have (SMUM_RequestSMULoadFW knows Tonga, Fiji, Iceland
// and Polaris only), but CAIL still carries the images (Stoney_UcodeInfo, what Stoney_GetFirmwareInfoTable hands to
// PowerPlay). So do what Linux does: in VRAM, a table of contents of SMU tasks plus the RLC save areas and copies of
// the images; then DriverDramAddrHi/Lo, InitJobs, ExecuteJob (ARAM save, power profiling, boot-up: loads every
// engine), wait for UcodeLoadStatus, and point the MEC at its image.
// The VRAM extends CAIL's own micro-engine block: in direct-loading mode reserve_fb_for_micro_engine reserves
// Stoney_FormatSmuDramDataBuffer's return value at the top of the FB ([cail+0xCE8] = its FB offset; CAIL itself only
// uses the first [cail+0xCD8] bytes, for the MEC). The load runs when CAIL starts loading the RLC
// (Stoney_LoadRlcUcode, in pm4.powerUp), before its own direct writes, which change nothing. -crnosmuload disables it.
namespace Smu8 {
	static constexpr uint32_t MP0PUB_IND_INDEX = 0x180, MP0PUB_IND_DATA = 0x181, MSG = 0x1C0, RESP = 0x1D0, ARG = 0x1E0;
	static constexpr uint32_t SramBase = 0x10000000, UcodeLoadStatus = 0x1FF80 + 0x18;  // SMU8_Firmware_Header
	static constexpr uint16_t DriverDramAddrHi = 0x33, DriverDramAddrLo = 0x34, InitJobs = 0x252, ExecuteJob = 0x254;
	// UCODE_ID_* (smu_ucode_xfer_cz.h); Stoney has no SDMA1 or MEC2, Linux maps them to SDMA0 and MEC_JT1.
	static constexpr uint8_t SDMA0 = 0, CE = 2, PFP = 3, ME = 4, MEC_JT1 = 5, RLC_G = 8, RLC_SCRATCH = 9,
	                         RLC_SRM_ARAM = 10, RLC_SRM_DRAM = 11;
	static constexpr uint32_t StoneyLoadMask = 1U << SDMA0 | 1U << CE | 1U << PFP | 1U << ME | 1U << MEC_JT1 | 1U << RLC_G;
	static constexpr uint8_t TaskUcodeLoad = 1, TaskUcodeSave = 2, TaskInitialize = 5;  // TASK_TYPE_*
	static constexpr uint8_t ArgInitMmPwrLog = 0, ArgInitClkTable = 1;                 // TASK_ARG_INIT_*
	static constexpr uint32_t JobListEntries = 32;                                     // NUM_JOBLIST_ENTRIES
	static constexpr uint16_t EndOfTaskList = 0xFFFF;
	// Region layout: TOC, the scratch buffers (Linux smu_buffer), then the images, 4 KB aligned.
	static constexpr uint32_t TocOffset = 0, ScratchOffset = 0x1000, UcodeOffset = 0x5000, RegionSize = 0x60000;
	struct Task {  // SMU_Task
		uint8_t type, arg;
		uint16_t next;
		uint32_t addrHi, addrLo, size;
	};
	static_assert(sizeof(Task) == 16, "SMU_Task layout");
	struct CailUcode {  // HWLibs' microcode descriptors (Stoney_RLC_UCODE, ...)
		uint32_t dwords, reserved;
		const char *version;
		uint64_t flags;
		const uint32_t *data;
		uint32_t jtOffset, jtSize;  // dwords
	};
}

static mach_vm_address_t orgStoneyFormatSmuDram, orgStoneyLoadRlcUcode;
static Smu8::CailUcode **stoneyUcodeInfo;  // RLC, SDMA0, SDMA1 (skipped), CE, PFP, ME, MEC1, MEC2 (skipped)
static uint32_t smuFormatCalls, smuCailSize, smuRegionStart;
static struct {
	const char *result;
	uint64_t blockOffset, mc, phys;
	uint32_t before, after, ms, resp[6], mecBase[3], tasks;
} smuLoad;

static uint64_t wrapStoneyFormatSmuDram(void *cail) {
	auto size = static_cast<uint32_t>(FunctionCast(wrapStoneyFormatSmuDram, orgStoneyFormatSmuDram)(cail));
	smuFormatCalls++;
	smuCailSize = size;
	smuRegionStart = (size + 0xFFF) & ~0xFFFU;
	return smuRegionStart + Smu8::RegionSize;
}

static uint32_t smuSendMessage(uint16_t msg, uint32_t arg) {
	auto *mmio = gpuMmio;
	// SMU_MP1_SRBM2P_RESP_0 != 0: the previous message is done (Linux waits up to its usec_timeout; allow 1 s).
	auto wait = [mmio]() {
		uint32_t resp = 0;
		for (uint32_t i = 0; i < 100000 && !(resp = mmio[Smu8::RESP] & 0xFFFF); i++)
			IODelay(10);
		return resp;
	};
	if (!wait())
		return 0xB0000 | msg;  // still busy with the last message
	mmio[Smu8::ARG] = arg;
	mmio[Smu8::RESP] = 0;
	mmio[Smu8::MSG] = msg;
	uint32_t resp = wait();
	return resp ? resp : 0xC0000 | msg;  // never answered
}

static uint32_t smuReadSram(uint32_t address) {
	gpuMmio[Smu8::MP0PUB_IND_INDEX] = Smu8::SramBase + address;
	return gpuMmio[Smu8::MP0PUB_IND_DATA];
}

static void smuLoadFirmware(void *cail) {
	using namespace Smu8;
	auto *mmio = gpuMmio;
	if (smuLoad.result || !mmio)
		return;
	auto *base = static_cast<uint8_t *>(cail);
	smuLoad.blockOffset = *reinterpret_cast<uint64_t *>(base + 0xCE8);
	if (!stoneyUcodeInfo) {
		smuLoad.result = "no Stoney_UcodeInfo";
		return;
	}
	if (!smuRegionStart || !smuLoad.blockOffset || *reinterpret_cast<uint32_t *>(base + 0xCD8) != smuCailSize) {
		smuLoad.result = "CAIL's micro-engine block isn't reserved";
		return;
	}
	uint64_t regionFb = smuLoad.blockOffset + smuRegionStart;
	uint64_t fbBase = static_cast<uint64_t>(mmio[Reg::MC_VM_FB_LOCATION] & 0xFFFF) << 24;
	smuLoad.mc = fbBase + regionFb;
	smuLoad.phys = (static_cast<uint64_t>(mmio[Reg::MC_VM_FB_OFFSET] & 0x3FFFF) << 22) + regionFb;

	// The images: RLC, SDMA0, CE, PFP, ME, MEC1 (Stoney_UcodeInfo slots 0, 1, 3, 4, 5, 6).
	static const uint8_t slots[] {0, 1, 3, 4, 5, 6};
	const CailUcode *ucode[arrsize(slots)];
	for (size_t i = 0; i < arrsize(slots); i++) {
		ucode[i] = stoneyUcodeInfo[slots[i]];
		if (!ucode[i] || !ucode[i]->data || !ucode[i]->dwords || ucode[i]->dwords > 0x20000) {
			smuLoad.result = "bad microcode descriptor";
			return;
		}
	}
	auto *desc = IOMemoryDescriptor::withPhysicalAddress(smuLoad.phys, RegionSize, kIODirectionInOut);
	auto *map = desc ? desc->map(kIOMapInhibitCache) : nullptr;
	if (!map) {
		OSSafeReleaseNULL(desc);
		smuLoad.result = "can't map the region";
		return;
	}
	auto *region = reinterpret_cast<uint8_t *>(map->getVirtualAddress());
	memset(region, 0, UcodeOffset);

	// Images, as amdgpu_ucode_init_bo lays them out: each 4 KB aligned; the MEC's jump table also copied to the next
	// 4 KB boundary after the MEC image (amdgpu_ucode_patch_jt), where the SMU's MEC_JT1 task loads it from.
	uint32_t offset = UcodeOffset, ucodeAt[arrsize(slots)], ucodeSize[arrsize(slots)], jtAt = 0, jtSize = 0;
	bool fits = true;
	for (size_t i = 0; i < arrsize(slots) && fits; i++) {
		ucodeAt[i] = offset;
		ucodeSize[i] = ucode[i]->dwords * 4;
		offset = (offset + ucodeSize[i] + 0xFFF) & ~0xFFFU;
		fits = offset <= RegionSize;
		if (fits)
			memcpy(region + ucodeAt[i], ucode[i]->data, ucodeSize[i]);
	}
	auto *mec = ucode[5];
	if (fits && mec->jtSize && mec->jtOffset + mec->jtSize <= mec->dwords) {
		jtAt = offset;
		jtSize = mec->jtSize * 4;
		fits = jtAt + jtSize <= RegionSize;
		if (fits)
			memcpy(region + jtAt, mec->data + mec->jtOffset, jtSize);
	}
	if (!fits || !jtSize) {
		OSSafeReleaseNULL(map);
		OSSafeReleaseNULL(desc);
		smuLoad.result = fits ? "MEC has no jump table" : "images don't fit";
		return;
	}
	enum { Rlc, Sdma0, Ce, Pfp, Me, Mec };

	// Scratch buffers (smu8_smu_init): RLC scratch, SRM ARAM and DRAM save areas, the multimedia power log
	// (SMU8_MultimediaPowerLogData) and the clock table (SMU8_Fusion_ClkTable), 32-byte aligned.
	enum { Scratch, Aram, Dram, PowerLog, ClkTable };
	static const uint32_t scratchSize[] {132, 8192, 4096, 48, 744};
	uint32_t scratchAt[arrsize(scratchSize)];
	for (uint32_t i = 0, at = ScratchOffset; i < arrsize(scratchSize); i++) {
		scratchAt[i] = at;
		at += (scratchSize[i] + 31) & ~31U;
	}

	// The table of contents (smu8_smu_construct_toc).
	auto *jobList = region + TocOffset;
	auto *tasks = reinterpret_cast<Task *>(region + TocOffset + JobListEntries);
	memset(jobList, 0xFF, JobListEntries);  // IGNORE_JOB
	uint16_t used = 0;
	auto task = [&](uint8_t type, uint8_t arg, uint32_t at, uint32_t size, bool last) {
		uint64_t mc = smuLoad.mc + at;
		auto &t = tasks[used++];
		t = {type, arg, last ? EndOfTaskList : used, static_cast<uint32_t>(mc >> 32), static_cast<uint32_t>(mc), size};
	};
	auto load = [&](uint8_t arg, int image, bool last) {
		if (arg == MEC_JT1)
			task(TaskUcodeLoad, arg, jtAt, jtSize, last);
		else
			task(TaskUcodeLoad, arg, ucodeAt[image], ucodeSize[image], last);
	};
	auto scratch = [&](uint8_t type, uint8_t arg, int entry, bool last) {
		task(type, arg, scratchAt[entry], scratchSize[entry], last);
	};
	uint16_t aramJob = used;
	scratch(TaskUcodeSave, RLC_SRM_ARAM, Aram, true);
	jobList[0] = static_cast<uint8_t>(used);  // JOB_GFX_SAVE
	scratch(TaskUcodeSave, RLC_SCRATCH, Scratch, false);
	scratch(TaskUcodeSave, RLC_SRM_DRAM, Dram, true);
	jobList[1] = static_cast<uint8_t>(used);  // JOB_GFX_RESTORE
	load(CE, Ce, false);
	load(PFP, Pfp, false);
	load(ME, Me, false);
	load(MEC_JT1, Mec, false);
	load(MEC_JT1, Mec, false);  // MEC_JT2 on Carrizo
	load(RLC_G, Rlc, false);
	scratch(TaskUcodeLoad, RLC_SCRATCH, Scratch, false);
	scratch(TaskUcodeLoad, RLC_SRM_ARAM, Aram, false);
	scratch(TaskUcodeLoad, RLC_SRM_DRAM, Dram, true);
	uint16_t powerProfilingJob = used;
	scratch(TaskInitialize, ArgInitMmPwrLog, PowerLog, true);
	uint16_t bootupJob = used;
	load(SDMA0, Sdma0, false);
	load(CE, Ce, false);
	load(PFP, Pfp, false);
	load(ME, Me, false);
	load(MEC_JT1, Mec, false);
	load(RLC_G, Rlc, true);
	scratch(TaskInitialize, ArgInitClkTable, ClkTable, true);
	smuLoad.tasks = used;
	__sync_synchronize();
	(void)*reinterpret_cast<volatile uint32_t *>(region + TocOffset);

	// smu8_request_smu_load_fw.
	uint64_t start = uptimeMs();
	smuLoad.before = smuReadSram(UcodeLoadStatus);
	mmio[MP0PUB_IND_INDEX] = SramBase + UcodeLoadStatus;
	mmio[MP0PUB_IND_DATA] = 0;
	smuLoad.resp[0] = smuSendMessage(DriverDramAddrHi, static_cast<uint32_t>(smuLoad.mc >> 32));
	smuLoad.resp[1] = smuSendMessage(DriverDramAddrLo, static_cast<uint32_t>(smuLoad.mc));
	smuLoad.resp[2] = smuSendMessage(InitJobs, 0);
	smuLoad.resp[3] = smuSendMessage(ExecuteJob, aramJob);
	smuLoad.resp[4] = smuSendMessage(ExecuteJob, powerProfilingJob);
	smuLoad.resp[5] = smuSendMessage(ExecuteJob, bootupJob);
	uint32_t status = 0;
	for (uint32_t i = 0; i < 100000; i++) {  // 1 s
		status = smuReadSram(UcodeLoadStatus);
		if ((status & StoneyLoadMask) == StoneyLoadMask)
			break;
		IODelay(10);
	}
	smuLoad.after = status;
	smuLoad.ms = static_cast<uint32_t>(uptimeMs() - start);

	// smu8_load_mec_firmware: halt both MEC pipes, then CP_CPC_IC_BASE_CNTL (VMID 0, ATC 0, CACHE_POLICY 0, MTYPE 1)
	// and CP_CPC_IC_BASE_LO/HI = the MEC image.
	constexpr uint32_t CP_MEC_CNTL = 0x208D, CP_CPC_IC_BASE_LO = 0x30B9, CP_CPC_IC_BASE_HI = 0x30BA,
	                   CP_CPC_IC_BASE_CNTL = 0x30BB;
	uint64_t mecMc = smuLoad.mc + ucodeAt[Mec];
	mmio[CP_MEC_CNTL] = mmio[CP_MEC_CNTL] | 0x50000000;
	mmio[CP_CPC_IC_BASE_CNTL] = (mmio[CP_CPC_IC_BASE_CNTL] & ~0x1980000FU) | 1U << 27;
	mmio[CP_CPC_IC_BASE_LO] = static_cast<uint32_t>(mecMc) & 0xFFFFF000;
	mmio[CP_CPC_IC_BASE_HI] = static_cast<uint32_t>(mecMc >> 32) & 0xFFFF;
	smuLoad.mecBase[0] = mmio[CP_CPC_IC_BASE_LO];
	smuLoad.mecBase[1] = mmio[CP_CPC_IC_BASE_HI];
	smuLoad.mecBase[2] = mmio[CP_CPC_IC_BASE_CNTL];

	OSSafeReleaseNULL(map);
	OSSafeReleaseNULL(desc);
	smuLoad.result = (status & StoneyLoadMask) == StoneyLoadMask ? "loaded" : "timed out";
	traceNote("smuload=%x->%x ", smuLoad.before, smuLoad.after);
}

static uint64_t wrapStoneyLoadRlcUcode(void *cail) {
	smuLoadFirmware(cail);
	return FunctionCast(wrapStoneyLoadRlcUcode, orgStoneyLoadRlcUcode)(cail);
}

// Which of the HWLibs hooks below were installed (bit per hook, in HwlibsHook order) and which failed. nvram72 showed
// none of them taking effect (no PowerPlay key notes, flags untouched, ATS not skipped) with no "noroute" note.
enum HwlibsHook {
	HookPeciRead, HookPeciSize, HookWaitFor, HookAts, HookHaltRlc, HookFlags, HookSmuFormat, HookSmuLoad, HookEarlyInit
};
static uint32_t hwlibsHooks, hwlibsHookFails;

static bool routeHwlibs(KernelPatcher &patcher, size_t index, KernelPatcher::RouteRequest &request,
	mach_vm_address_t slide, size_t size, HwlibsHook hook) {
	if (routeChecked(patcher, index, request, slide, size)) {
		hwlibsHooks |= 1U << hook;
		return true;
	}
	hwlibsHookFails |= 1U << hook;
	traceNote("noroute:%s(%d) ", request.symbol, patcher.getError());
	patcher.clearError();
	return false;
}

// CAIL's early start-up. AmdCailServices::earlyInitialize copies the VBIOS from the "ATY,bin_image" property and,
// without it, returns 0xE00002BC before CAILEarlyASICInit: CAIL then never reads its registry settings (so the
// DisableCailLoadUcode override, PowerPlay's Dummy back-end key and the ATS skip never happen; nvram72/73, both boots
// where the display controller had started before the accelerator kexts loaded, after which the property was gone).
// Put our copy back if it's missing.
static mach_vm_address_t orgCailEarlyInitialize;
static uint32_t binImageChecks, binImageRestores, earlyInitResult = 0xFFFFFFFF;

static uint64_t wrapCailEarlyInitialize(void *that, void *callbacks, void *init) {
	binImageChecks++;
	if (vbiosImage && vbiosOwner && !vbiosOwner->getProperty(VBIOS::ImageProperty)) {
		vbiosOwner->setProperty(VBIOS::ImageProperty, vbiosImage);
		binImageRestores++;
		traceNote("binimage=restored ");
	}
	auto ret = FunctionCast(wrapCailEarlyInitialize, orgCailEarlyInitialize)(that, callbacks, init);
	earlyInitResult = static_cast<uint32_t>(ret);
	return ret;
}

void CRDriver::traceKext(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size, size_t kext) {
	// HWLibs' stages are routed after its hooks below: Stoney_LoadRlcUcode is both a stage and the SMU load's hook, and
	// the one routed second fails (nvram73: the stage won, so the SMU load never ran).
	if (!checkKernelArgument("-crnotrace") && kext != KextRadeonX4000HWLibs)
		routeStages(patcher, index, slide, size, kext == KextRadeonX4000 ? InAccel : InHWServices);
	if (kext == KextRadeonX4000HWLibs) {
		ppDummyForced = (family == ChipFamily::Stoney || family == ChipFamily::Carrizo) &&
			!checkKernelArgument("-crnoppdummy");
		KernelPatcher::RouteRequest requests[] {
			{"_PECI_ReadRegistry", wrapPeciReadRegistry, orgPeciReadRegistry},
			{"_PECI_GetRegistrySize", wrapPeciGetRegistrySize, orgPeciGetRegistrySize},
			{"_Cail_MCILWaitFor", wrapCailWaitFor, orgCailWaitFor},
		};
		KernelPatcher::RouteRequest early {"__ZN15AmdCailServices15earlyInitializeEP23_MCIL_SERVICE_CALLBACKSP15_CailInitialize",
			wrapCailEarlyInitialize, orgCailEarlyInitialize};
		routeHwlibs(patcher, index, early, slide, size, HookEarlyInit);
		static const HwlibsHook hooks[] {HookPeciRead, HookPeciSize, HookWaitFor};
		for (size_t i = 0; i < arrsize(requests); i++)
			routeHwlibs(patcher, index, requests[i], slide, size, hooks[i]);
		if ((family == ChipFamily::Stoney || family == ChipFamily::Carrizo) && !checkKernelArgument("-crats")) {
			KernelPatcher::RouteRequest ats {"_Cail_Spectre_EnableAts", wrapCailEnableAts, orgCailEnableAts};
			routeHwlibs(patcher, index, ats, slide, size, HookAts);
		}
		if ((family == ChipFamily::Stoney || family == ChipFamily::Carrizo) && !checkKernelArgument("-crnodirectucode")) {
			// The flags word store, and the RLC loader's test of the flag (test byte [rbx+0x6F2], 0x20).
			auto fn = patcher.solveSymbol(index, "_CailReadinOverrideRegistrySetting", slide, size);
			auto rlc = patcher.solveSymbol(index, "_Stoney_LoadRlcUcode", slide, size);
			patcher.clearError();
			KernelPatcher::RouteRequest request {"_CailReadinOverrideRegistrySetting",
				wrapCailReadinOverrideRegistrySetting, orgCailReadinOverrideRegistrySetting};
			if (!checkKernelArgument("-crnorlcreset")) {
				KernelPatcher::RouteRequest halt {"_stoney_halt_rlc", wrapStoneyHaltRlc, orgStoneyHaltRlc};
				auto haltFn = patcher.solveSymbol(index, "_stoney_halt_rlc", slide, size);
				patcher.clearError();
				// RLC_CNTL = 0 (mov esi, 0xEC00), then the serdes idle wait.
				if (!codeHas(haltFn, 0x20, "be00ec0000"))
					traceNote("rlcreset=nomatch ");
				else
					routeHwlibs(patcher, index, halt, slide, size, HookHaltRlc);
			}
			if (!codeHas(fn, 0x100, "8983f0060000") || !codeHas(rlc, 0x60, "f683f206000020")) {
				traceNote("directucode=nomatch ");
			} else if (routeHwlibs(patcher, index, request, slide, size, HookFlags) && family == ChipFamily::Stoney &&
				!checkKernelArgument("-crnosmuload")) {
				// See smuLoadFirmware. The size function must end by storing its total at [cail+0xCD8]
				// (mov [rbx+0xCD8], eax), which the wrapper relies on.
				auto info = patcher.solveSymbol(index, "_Stoney_UcodeInfo", slide, size);
				auto format = patcher.solveSymbol(index, "_Stoney_FormatSmuDramDataBuffer", slide, size);
				patcher.clearError();
				if (!info || !codeHas(format, 0x120, "8983d80c0000")) {
					traceNote("smuload=nomatch ");
				} else {
					stoneyUcodeInfo = reinterpret_cast<Smu8::CailUcode **>(info);
					KernelPatcher::RouteRequest smu[] {
						{"_Stoney_FormatSmuDramDataBuffer", wrapStoneyFormatSmuDram, orgStoneyFormatSmuDram},
						{"_Stoney_LoadRlcUcode", wrapStoneyLoadRlcUcode, orgStoneyLoadRlcUcode},
					};
					if (routeHwlibs(patcher, index, smu[0], slide, size, HookSmuFormat))
						routeHwlibs(patcher, index, smu[1], slide, size, HookSmuLoad);
				}
			}
		}
		if (!checkKernelArgument("-crnotrace"))
			routeStages(patcher, index, slide, size, InHWLibs);
	}
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

// Line buffer size. LB_MEMORY_CTRL (pipe 0 at dword 0x1AC1, +0x200 per pipe on DCE 11.0): Apple writes
// LB_MEMORY_SIZE 0x404, sized for Polaris's DCE 11.2 line buffer (13-bit field, shared by 6 pipes). DCE 11.0's field
// is 12 bits and Linux (dce_transform_set_scaler, LB_TOTAL_NUMBER_OF_ENTRIES) and the firmware both use 0x6B0 (1712).
// Spoof boot 29: the link trained and the scanout memory was a uniform grey, yet the panel showed identical
// coloured rows (vertical stripes), i.e. line-buffer contents rather than the surface. Force the size to 0x6B0 and
// keep Apple's other fields. -crnolbfix disables.

void CRDriver::wrapRegWrite32(void *that, uint32_t offset, uint32_t value) {
	if (!lbFixOff) {
		uint32_t dword = offset >> 2;
		if (dword == 0x1AC1 || dword == 0x1CC1 || dword == 0x1EC1) {
			uint32_t fixed = (value & ~0xFFFu) | 0x6B0;
			if (fixed != value) {
				value = fixed;
				__atomic_fetch_add(&lbFixes, 1, __ATOMIC_RELAXED);
			}
		}
	}
	// DP colour depth: see the DIGxEncoderControl patch in wrapExecuteBiosCommandTable.
	if (!bpcCapOff && bootDpDepth != 0xFF) {
		uint32_t dword = offset >> 2;
		if ((dword == 0x4AA1 || dword == 0x4BA1 || dword == 0x4CA1) && ((value >> 24) & 7) > bootDpDepth) {
			value = (value & ~0x7000000u) | (static_cast<uint32_t>(bootDpDepth) << 24);
			__atomic_fetch_add(&bpcCaps, 1, __ATOMIC_RELAXED);
		}
	}
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

static uint32_t ihEnableCalls, ihLastEnable = 0xFF;  // BaffinInterruptManager::setHardwareEnabled, for the black box

// IH interrupt enable. BaffinInterruptManager::setHardwareEnabled() enables the ring the Tonga (oss 3.0) way:
// IH_RB_CNTL.RB_ENABLE (bit 0) and IH_RB_CNTL.ENABLE_INTR (bit 17). Carrizo/Stoney's IH (oss 3.0.1, Linux cz_ih) has
// ENABLE_INTR in IH_CNTL bit 0 instead, and needs IH_CNTL.RPTR_REARM (bit 4) for MSI so that each read-pointer write
// re-arms the message; bit 17 of IH_RB_CNTL does nothing. Spoof boot 36: the IH wrote 2316 entries into the ring
// (WPTR 0x90C0, write-back agreeing) with IH_CNTL = 0x01080000, and the handler was never called. Mirror Apple's enable
// state into IH_CNTL after it runs. -crnoihfix disables.
static constexpr uint32_t IhCntlEnableIntr = 0x1, IhCntlRptrRearm = 0x10;

void CRDriver::wrapIhSetHardwareEnabled(void *that, bool enable) {
	bool off = enable && checkKernelArgument("-crnoih");
	ihEnableCalls++;
	ihLastEnable = enable && !off;
	ihManager = that;
	traceEvent(off ? "IH enable (forced off by -crnoih)" : enable ? "IH enable" : "IH disable", true, enable);
	FunctionCast(wrapIhSetHardwareEnabled, callback->orgIhSetHardwareEnabled)(that, enable && !off);
	if (gpuMmio && !checkKernelArgument("-crnoihfix")) {
		uint32_t cntl = gpuMmio[IhReg::CNTL];
		if (enable && !off)
			cntl |= IhCntlEnableIntr | IhCntlRptrRearm;
		else
			cntl &= ~IhCntlEnableIntr;
		gpuMmio[IhReg::CNTL] = cntl;
		traceEvent("IH_CNTL (CZ enable)", true, cntl);
	}
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
	// The command-table hook records VBIOS calls and carries the DP PHY clock-source fix; it stays installed unless
	// both are switched off.
	dpExtClkOff = checkKernelArgument("-crnodpextclk");
	edpPowerOnOff = checkKernelArgument("-crnoedppoweron");
	if (!checkKernelArgument("-crnoatomlog") || !dpExtClkOff || !edpPowerOnOff) {
		// Legacy atombios VBIOSes (GCN 1-4, Stoney included) run through AtiAtomBiosParserService (→ _ParseTable);
		// AtiAtomFirmwareInterface is the atomfirmware (Vega+) path and saw no calls on boot 23.
		KernelPatcher::RouteRequest request {"__ZN24AtiAtomBiosParserService23executeBiosCommandTableEjPvj",
			wrapExecuteBiosCommandTable, orgExecuteBiosCommandTable};
		if (!routeChecked(patcher, index, request, slide, size)) {
			SYSLOG("cr", "failed to route executeBiosCommandTable (error %d)", patcher.getError());
			patcher.clearError();
		}
	}
	if (!checkKernelArgument("-crnoatomlog")) {
		KernelPatcher::RouteRequest requests[] {
			{"__ZN15AuxChannelProxy9readBytesEjjPhh", wrapAuxReadBytes, orgAuxReadBytes},
			{"__ZN15AuxChannelProxy10writeBytesEjjPhh", wrapAuxWriteBytes, orgAuxWriteBytes},
		};
		for (auto &request : requests) {
			if (!routeChecked(patcher, index, request, slide, size)) {
				SYSLOG("cr", "failed to route %s (error %d)", request.symbol, patcher.getError());
				patcher.clearError();
			}
		}
	}
	if (!checkKernelArgument("-crnolanecap")) {
		KernelPatcher::RouteRequest request {"__ZNK28DPTrainingSettingsCalculator12maxLinkLanesEv", wrapMaxLinkLanes,
			orgMaxLinkLanes};
		if (!routeChecked(patcher, index, request, slide, size)) {
			SYSLOG("cr", "failed to route maxLinkLanes (error %d)", patcher.getError());
			patcher.clearError();
		}
	}
	{
		KernelPatcher::RouteRequest request {"__ZNK28DPTrainingSettingsCalculator11maxLinkRateEv", wrapMaxLinkRate,
			orgMaxLinkRate};
		if (!routeChecked(patcher, index, request, slide, size)) {
			SYSLOG("cr", "failed to route maxLinkRate (error %d)", patcher.getError());
			patcher.clearError();
		}
	}
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

uint8_t CRDriver::wrapExecuteBiosCommandTable(void *that, uint32_t table, void *params, uint32_t flag) {
	__atomic_fetch_add(&atomCalls, 1, __ATOMIC_RELAXED);
	bool aux = table == AtomAuxTable;
	if (aux)
		__atomic_fetch_add(&auxCalls, 1, __ATOMIC_RELAXED);
	AtomCall entry {};
	auto *call = &entry;
	call->table = static_cast<uint16_t>(table);
	// DP PHY clock source and coherent mode. DIG1TransmitterControl (table 0x4C) v1.5 parameters: usSymClock, ucPhyId,
	// ucAction, ucLaneNum, ucConnObjId, ucDigMode (+6), asConfig (+7, ATOM_DIG_TRANSMITTER_CONFIG_V5 little-endian:
	// [0] reserved, [1] ucCoherentMode, [3:2] ucPhyClkSrcId, [6:4] ucHPDSel), ucDigEncoderSel, ucDPLaneSet. For DP Apple
	// sends 0x10: HPD1, P1PLL, not coherent. The APU VBIOS FirmwareInfo has usUniphyDPModeExtClkFreq = 100 MHz
	// ("non-zero: DP-mode Uniphy input clock from the external spread clock"); legacy amdgpu (atombios_encoders.c,
	// case 5) sends ucPhyClkSrcId = ENCODER_REFCLK_SRC_EXTCLK (3) when dp_extclk is set and ucCoherentMode = 1 ("DP
	// requires coherent"), i.e. 0x1E for eDP on HPD1. Spoof boots 24-27 failed clock recovery at every swing (boots
	// 25-27 had this patch writing bits [1:0] by mistake: coherent + reserved, clock still P1PLL). Apply both for DP
	// modes (0 SST, 5 MST), keep Apple's HPD. -crnodpextclk disables.
	uint8_t patched = 0;
	if (table == AtomTransmitterTable && params && !dpExtClkOff) {
		auto *bytes = static_cast<uint8_t *>(params);
		if (bytes[6] == 0 || bytes[6] == 5) {
			uint8_t config = static_cast<uint8_t>((bytes[7] & 0xF0) | (3 << 2) | 0x02);
			if (config != bytes[7]) {
				bytes[7] = config;
				patched = 0x80;
			}
		}
	}
	// DP colour depth. The firmware drives this eDP panel at 6 bpc (DP_PIXEL_FORMAT.DP_COMPONENT_DEPTH = 0, FMT truncation
	// to 6 bits + dither): an 18 bpp panel. Apple's DIGxEncoderControl (table 4) v4 calls pass ucBitPerColor (+6) =
	// PANEL_8BIT_PER_COLOR for DP mode (ucEncoderMode +4 = 0), and DP_PIXEL_FORMAT ended up at 8 bpc. Spoof boot 31: link
	// trained, scanout memory uniform grey, panel showing identical coloured rows, as a 6 bpc sink decoding 24 bpp data
	// would. Cap ucBitPerColor at the firmware's depth (ATOM value = DP_COMPONENT_DEPTH + 1); DP_PIXEL_FORMAT writes are
	// capped in wrapRegWrite32. -crnobpccap disables.
	if (table == AtomEncoderTable && params && !bpcCapOff && bootDpDepth != 0xFF) {
		auto *bytes = static_cast<uint8_t *>(params);
		uint8_t atomBpc = static_cast<uint8_t>(bootDpDepth + 1);
		if (bytes[3] != 0x10 && bytes[4] == 0 && bytes[6] > atomBpc) {
			bytes[6] = atomBpc;
			patched = 0x80;
			__atomic_fetch_add(&bpcCaps, 1, __ATOMIC_RELAXED);
		}
	}
	if (params)
		memcpy(call->in, params, sizeof(call->in));
	else
		memset(call->in, 0, sizeof(call->in));
	// eDP panel power. Legacy amdgpu (atombios_encoders.c, enable_dig) runs the transmitter table with
	// ATOM_TRANSMITTER_ACTION_POWER_ON (12) for an eDP connector before ATOM_TRANSMITTER_ACTION_ENABLE and waits for the
	// panel; Apple never does. Spoof boot 26: AUX worked, but the sink never saw clock recovery on lane 0 at any swing,
	// and the LVTMA power sequencer (DIGON/SYNCEN/BLON on at boot) had been switched off by Apple's clean-up; SYNCEN
	// gates the PHY data output. Before Apple's transmitter ENABLE (action 1) for connector 0x14 (eDP), run the table
	// the way amdgpu_atombios_encoder_set_edp_panel_power() does (all parameters zero, ucAction = POWER_ON) and allow
	// the 300 ms it gives the panel to raise HPD. -crnoedppoweron disables.
	if (table == AtomTransmitterTable && params && !edpPowerOnOff) {
		auto *bytes = static_cast<uint8_t *>(params);
		if (bytes[3] == 1 && bytes[5] == 0x14) {
			uint8_t power[32] {};
			power[3] = 12;  // ATOM_TRANSMITTER_ACTION_POWER_ON
			uint8_t powered = FunctionCast(wrapExecuteBiosCommandTable, callback->orgExecuteBiosCommandTable)(that,
				table, power, flag);
			AtomCall injected {};
			injected.table = static_cast<uint16_t>(table);
			injected.ok = static_cast<uint8_t>((powered & 1) | 0x40);  // 0x40 = injected by CarrizoRed
			injected.repeat = 1;
			memcpy(injected.in, power, sizeof(injected.in));
			memcpy(injected.out, power, sizeof(injected.out));
			ringPushMerged(atomRing, AtomRingSize, atomHead, injected, +[](const AtomCall &, const AtomCall &) {
				return false;
			});
			if (ml_get_interrupts_enabled() && !ml_at_interrupt_context())
				IOSleep(300);
			else
				IODelay(300000);
		}
	}
	uint8_t ok = FunctionCast(wrapExecuteBiosCommandTable, callback->orgExecuteBiosCommandTable)(that, table, params,
		flag);
	call->ok = static_cast<uint8_t>((ok & 1) | patched);
	if (params)
		memcpy(call->out, params, sizeof(call->out));
	call->repeat = 1;
	if (aux) {
		if (!(ok & 1))
			__atomic_fetch_add(&auxFailures, 1, __ATOMIC_RELAXED);
	} else {
		ringPushMerged(atomRing, AtomRingSize, atomHead, entry, +[](const AtomCall &a, const AtomCall &b) {
			return a.table == b.table && a.ok == b.ok && !memcmp(a.in, b.in, sizeof(a.in)) &&
			       !memcmp(a.out, b.out, sizeof(a.out));
		});
	}
	return ok;
}

static void dpcdRecord(uint32_t address, uint32_t length, bool write, const uint8_t *data, IOReturn ret) {
	__atomic_fetch_add(&dpcdCount, 1, __ATOMIC_RELAXED);
	if (write && address == 0x102 && length >= 1 && data && (data[0] & 3) == 1 && gpuMmio) {
		for (size_t i = 0; i < sizeof(trainSnapRegs) / sizeof(trainSnapRegs[0]); i++)
			trainSnap[i] = gpuMmio[trainSnapRegs[i]];
		trainSnaps++;
	}
	DpcdTx tx {};
	tx.address = address;
	tx.length = static_cast<uint8_t>(length > 0xFF ? 0xFF : length);
	tx.write = write;
	tx.ok = ret == kIOReturnSuccess ? 0 : static_cast<uint8_t>(ret & 0xFF);
	tx.repeat = 1;
	if (data)
		memcpy(tx.data, data, length < sizeof(tx.data) ? length : sizeof(tx.data));
	ringPushMerged(dpcdRing, DpcdRingSize, dpcdHead, tx, +[](const DpcdTx &a, const DpcdTx &b) {
		return a.address == b.address && a.length == b.length && a.write == b.write && a.ok == b.ok &&
		       !memcmp(a.data, b.data, sizeof(a.data));
	});
}

// eDP alternate scrambler seed reset (ASSR). Spoof boot 34: with pipe 0 held at the firmware's values the CRTC read back
// the right pixels (CRTC_PIXEL_DATA_READBACK = 0xBF8 per channel, the grey in memory) and DIG0 / DP0 matched the
// firmware's, yet the panel still showed identical rows of random colour. Uniform pixels scrambled with one seed and
// descrambled with another decode to the same wrong colours on every line, since the scrambler resets each line.
// An eDP source and panel that support it use ASSR (DPCD 0x00D bit 0 = capable, 0x10A bit 0 = enabled): the firmware
// sets it in both the DIG (VBIOS panel mode) and the panel, but Apple's training never writes 0x10A, and the panel
// forgets it when its power is cycled. Record 0x00D / 0x10A before Apple's first AUX transaction, and when Apple sets
// the link's channel coding (DPCD 0x108, just before training pattern 1) enable ASSR in the panel again if it is
// capable. -crnoassr disables the write (the values are still recorded).
static uint8_t assrCap = 0xFF, assrBoot = 0xFF, assrReadback = 0xFF;
static uint32_t assrWrites;
static bool assrProbed;

static void assrProbe(void *that, uint8_t flags, IOReturn (*read)(void *, uint32_t, uint32_t, uint8_t *, uint8_t)) {
	if (assrProbed)
		return;
	assrProbed = true;
	uint8_t value = 0;
	if (read(that, 0x00D, 1, &value, flags) == kIOReturnSuccess)
		assrCap = value;
	if (read(that, 0x10A, 1, &value, flags) == kIOReturnSuccess)
		assrBoot = value;
	IOLog("CarrizoRed: eDP DPCD 0x00D (eDP config cap) = 0x%02x, 0x10A (eDP config set) = 0x%02x before Apple's link "
		  "setup\n", assrCap, assrBoot);
}

IOReturn CRDriver::wrapAuxReadBytes(void *that, uint32_t address, uint32_t length, uint8_t *data, uint8_t flags) {
	auto read = FunctionCast(wrapAuxReadBytes, callback->orgAuxReadBytes);
	assrProbe(that, flags, read);
	IOReturn ret = read(that, address, length, data, flags);
	dpcdRecord(address, length, false, data, ret);
	return ret;
}

IOReturn CRDriver::wrapAuxWriteBytes(void *that, uint32_t address, uint32_t length, uint8_t *data, uint8_t flags) {
	auto read = FunctionCast(wrapAuxReadBytes, callback->orgAuxReadBytes);
	auto write = FunctionCast(wrapAuxWriteBytes, callback->orgAuxWriteBytes);
	assrProbe(that, flags, read);
	IOReturn ret = write(that, address, length, data, flags);
	dpcdRecord(address, length, true, data, ret);
	if (!assrOff && assrCap != 0xFF && (assrCap & 1) && address <= 0x108 && address + length > 0x108 &&
		ret == kIOReturnSuccess) {
		uint8_t value = 1;  // ALTERNATE_SCRAMBLER_RESET_ENABLE
		IOReturn r = write(that, 0x10A, 1, &value, flags);
		dpcdRecord(0x10A, 1, true, &value, r);
		if (r == kIOReturnSuccess)
			assrWrites++;
		if (read(that, 0x10A, 1, &value, flags) == kIOReturnSuccess)
			assrReadback = value;
	}
	return ret;
}

// DP lane count. DPTrainingSettingsCalculator::maxLinkLanes() = min(LinkCaps lanes, sink DPCD MAX_LANE_COUNT), with no
// knowledge of how many lanes the board wires to the eDP panel. Spoof boot 23: the firmware drove the panel on DIG0/DP0
// with DP_CONFIG.DP_UDI_LANES = 0 (one lane); Apple's training reported lane 0 fully locked (DPCD 0x202 = 0x07) and
// lane 1 dead, i.e. it trained more than one lane, failed clock recovery and powered the panel off. Cap the lane count
// at what the firmware used for the link it left running (bootDpLanes, from initDisplayRegs). -crnolanecap disables.
static uint8_t bootDpLanes;  // 0 = no DP link was running at boot

// Exhaustive display-path diff. Spoof boot 30: link trained, video on, scanout memory uniform grey, every watched
// surface register equal to the firmware's (bar address and pitch), yet the panel shows identical coloured rows. So
// the difference is in a register not watched: snapshot whole blocks at boot (firmware/GOP state, working picture)
// and report every register that differs at crpanicat time.
static const struct {
	uint32_t start, count;
} diffBlocks[] {
	{0x100, 0x60},   // DCCG: pixel rate, DTOs, DPREFCLK, DENTIST, resync
	{0x2C0, 0x80},   // pipe power gating, DMIF, DCI, MCIF
	{0x1A00, 0x200}, // pipe 0: DCP (surface, DVMM, CSC, gamma), LB, DCFE, DMIF_PG/DPG, SCL, BLND, CRTC, FMT
	{0x4800, 0x100}, // LVTMA power sequencer, backlight, HPD, DCIO
	{0x4A00, 0x100}, // DIG0 / DP0
};
static constexpr uint32_t DiffRegCount = 0x60 + 0x80 + 0x200 + 0x100 + 0x100;
static uint32_t diffBoot[DiffRegCount];
static bool diffBootValid;

// -crrestore: write the firmware's boot values back into pipe 0's processing chain. Spoof boot 32: the DP transport
// (DIG0, DP0, pixel format, M/N, PHY) matched the firmware's working state register for register and the scanout memory
// was uniform grey, yet the panel showed identical rows of random colour. Spoof boot 33 restored the blocks one at a
// time, 15 s apart, but Apple rewrote some of them afterwards (CUR_CONTROL was back on) and the flicker made the timing
// impossible to read off the panel. So restore every block at t=30 and hold it there: re-write any register Apple
// changes back on every watcher tick. Control registers only (status / counter registers and the surface address and
// pitch, which point at Apple's framebuffer, are left alone).
static const uint32_t restoreFmt[] {0x1BE8, 0x1BE9, 0x1BEA, 0x1BEB, 0x1BEC, 0x1BED, 0x1BEE, 0x1BEF, 0x1BF0, 0x1BF1,
	0x1BF2, 0x1BF3, 0x1BF4, 0x1BF5, 0x1BF6, 0x1BF7, 0x1BF8, 0x1BF9, 0x1BFA, 0x1BFB, 0x1BFC};
// LB_DATA_FORMAT, LB_MEMORY_CTRL, LB_DESKTOP_HEIGHT, LB_VLINE_START_END, LB_SYNC_RESET_SEL (LB_SYNC_DURATION).
static const uint32_t restoreLb[] {0x1AC0, 0x1AC1, 0x1AC3, 0x1AC4, 0x1ACC};
// Input gamma, CSC, cursor (CUR_CONTROL turns Apple's 128x128 hardware cursor off), regamma: REGAMMA_CONTROL back to
// the firmware's bypass plus the region setup (0x1AA4..0x1AAD; not the LUT index / data / write-mask ports).
static const uint32_t restoreColor[] {0x1A10, 0x1A2D, 0x1A50, 0x1A59, 0x1A5A, 0x1A5B, 0x1A5C, 0x1A5D, 0x1A5E, 0x1A5F,
	0x1A60, 0x1A66, 0x1AA0, 0x1AA4, 0x1AA5, 0x1AA6, 0x1AA7, 0x1AA8, 0x1AA9, 0x1AAA, 0x1AAB, 0x1AAC, 0x1AAD};
// Scaler, CRTC update / trigger control and CRTC_CONTROL (CRTC_START_POINT_CNTL).
static const uint32_t restoreScl[] {0x1B40, 0x1B44, 0x1B4A, 0x1B4C, 0x1B4E, 0x1B53, 0x1B86, 0x1B88, 0x1B89, 0x1B8A,
	0x1B9C, 0x1BB6, 0x1BBE, 0x1BE7};
// Spoof boot 34: with all of the above held, the last control-looking differences after the CRTC are four DIG0
// registers the Linux DCE 11.0 headers don't name (0x4A49..0x4A4D; 0x4A58/59/66/67 change constantly and look like
// CRC / seed values, so they are left alone). Restored later than the rest, at t=30 + 15, so a change on the panel at
// that point names them.
static const uint32_t restoreDig[] {0x4A49, 0x4A4A, 0x4A4B, 0x4A4D};
static const struct {
	uint32_t at;
	const char *name;
	const uint32_t *regs;
	size_t count;
} restoreSteps[] {
	{30, "FMT", restoreFmt, sizeof(restoreFmt) / sizeof(restoreFmt[0])},
	{30, "LB", restoreLb, sizeof(restoreLb) / sizeof(restoreLb[0])},
	{30, "DCP colour + cursor + regamma", restoreColor, sizeof(restoreColor) / sizeof(restoreColor[0])},
	{30, "SCL + CRTC", restoreScl, sizeof(restoreScl) / sizeof(restoreScl[0])},
	{45, "DIG0 unnamed", restoreDig, sizeof(restoreDig) / sizeof(restoreDig[0])},
};
static bool diffBootValue(uint32_t offset, uint32_t &value) {
	if (!diffBootValid)
		return false;
	uint32_t n = 0;
	for (auto &block : diffBlocks) {
		if (offset >= block.start && offset < block.start + block.count) {
			value = diffBoot[n + offset - block.start];
			return true;
		}
		n += block.count;
	}
	return false;
}

static uint32_t restoreStep(size_t index) {
	auto &step = restoreSteps[index];
	uint32_t written = 0;
	for (size_t i = 0; i < step.count; i++) {
		uint32_t boot;
		if (diffBootValue(step.regs[i], boot) && gpuMmio[step.regs[i]] != boot) {
			gpuMmio[step.regs[i]] = boot;
			written++;
		}
	}
	return written;
}

static void restoreTick(uint32_t t) {
	if ((gpuMmio[Reg::PIPE0_PG_STATUS] >> 30) != 0)
		return;  // pipe 0 power-gated
	// Hold the steps already done: Apple rewrites some of these (cursor, regamma) on its own.
	for (size_t i = 0; i < restoreDone; i++)
		restoreHolds += restoreStep(i);
	while (restoreDone < sizeof(restoreSteps) / sizeof(restoreSteps[0]) && t >= restoreSteps[restoreDone].at) {
		uint32_t written = restoreStep(restoreDone++);
		restoreWrites += written;
		IOLog("CarrizoRed: t=%us restore step %u (%s): %u registers set to their boot values\n", t, restoreDone,
			restoreSteps[restoreDone - 1].name, written);
	}
}

// Pipe 0 underflow sampling, every watcher tick. Spoof boot 33: with the transport matching the firmware's, the panel
// flickered between rows of random colour, a white line and flashing, and LB_BUFFER_LEVEL_STATUS.DATA_BUFFER_LEVEL was
// 0x10 against the firmware's 0x37: the look of a pipe starved of pixel data (underflow), not of a wrong setting. Arm
// the XDMA cache underflow counter (GRPH_XDMA_CACHE_UNDERFLOW_DET_CNTL.CNT_EN) and record its count, the DMIF and blender
// underflow flags (acknowledged after each sighting so a new one shows) and the line-buffer level range.
static void underflowTick() {
	if ((gpuMmio[Reg::PIPE0_PG_STATUS] >> 30) != 0)
		return;
	static bool armed;
	if (!armed) {
		gpuMmio[0x1A9F] |= 1;  // GRPH_XDMA_CACHE_UNDERFLOW_CNT_EN
		armed = true;
	}
	underflowSamples++;
	uint32_t xdma = gpuMmio[0x1ABF] & 0xFFFFF;
	if (xdma > xdmaUnderflowMax)
		xdmaUnderflowMax = xdma;
	if (gpuMmio[0x2F7] & 0x10000000)  // DMIF_STATUS.DMIF_UNDERFLOW
		dmifUnderflows++;
	uint32_t blnd = gpuMmio[0x1B71];
	if (blnd & 1) {  // BLND_UNDERFLOW_INTERRUPT.BLND_UNDERFLOW_INT_OCCURED
		blndUnderflows++;
		gpuMmio[0x1B71] = blnd | 0x100;  // ACK
	}
	uint32_t level = (gpuMmio[0x1AD7] >> 16) & 0xFFF;  // LB_BUFFER_LEVEL_STATUS.DATA_BUFFER_LEVEL
	if (level < lbLevelMin)
		lbLevelMin = level;
	if (level > lbLevelMax)
		lbLevelMax = level;
}

uint8_t CRDriver::wrapMaxLinkLanes(const void *that) {
	uint8_t lanes = FunctionCast(wrapMaxLinkLanes, callback->orgMaxLinkLanes)(that);
	uint8_t capped = bootDpLanes && lanes > bootDpLanes ? bootDpLanes : lanes;
	traceEvent("dp.maxLinkLanes apple<<8|used", true, (static_cast<uint32_t>(lanes) << 8) | capped);
	return capped;
}

// Logged only: the link rate Apple trains at (DPCD units, 0x06 = 1.62, 0x0A = 2.7, 0x14 = 5.4 Gbps).
uint8_t CRDriver::wrapMaxLinkRate(const void *that) {
	uint8_t rate = FunctionCast(wrapMaxLinkRate, callback->orgMaxLinkRate)(that);
	traceEvent("dp.maxLinkRate", true, rate);
	return rate;
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

	// PowerPlay (HWLibs PHM_Initialize) has back ends for VI dGPUs, Vega, Navi and Raven/Renoir, but none for family
	// 0x87 (Carrizo/Stoney), and the APU spoofed as Polaris 11 gets the VI path: the Tonga table parser and the Polaris
	// SMU7 back end, which fail (PHM_Initialize = 2, nvram36-41) and would drive an SMU the APU doesn't have. Without
	// PowerPlay the accelerator never powers up. PP_* registry keys are read from the GPU's PCI device properties
	// (ATIController::getRegistryKey); PP_PhmUseDummyBackEnd selects the Dummy back end, which leaves clocks and power
	// to the SMU firmware, as Linux amdgpu does with dpm=0. That key read back as 0 all the same (nvram41), so
	// wrapPeciReadRegistry answers it too. -crnoppdummy disables both.
	if (!checkKernelArgument("-crnoppdummy")) {
		uint32_t one = 1;
		iGPU->setProperty("PP_PhmUseDummyBackEnd", &one, sizeof(one));
	}
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
	relocateFb();
}

// The accelerator (AMDRadeonX4000) assumes Polaris's memory layout: VRAM at GPU address 0, the GART right after it
// (0x20000000 for 512 MB), and a system aperture covering [0, GART) (AMDVIHardware::initializeSystemApertureRegs). It
// never reads MC_VM_FB_LOCATION. On the APU the firmware maps the carve-out at 0xF4_0000_0000, so a "VRAM" address
// such as 0x01000000 falls in the system aperture but outside the FB, and the memory controller treats it as a
// physical RAM address: the GPU reads garbage and writes into whatever macOS keeps in low memory (nvram50: the CP
// stalled with fetches pending after its first packets; earlier accelerated boots reset hard). Move the FB window to
// 0, as Linux radeon did on APUs (radeon_vram_location(..., 0)); the carve-out itself (MC_VM_FB_OFFSET) stays where it
// is. The CPU's BAR0 window (HDP_NONSURFACE_BASE) and any live scanout surface move with it; AMD9500Controller reads
// the FB base back from the register (AtiAsicInfo::refreshFbLocation). Only with acceleration; -crnofbreloc disables
// it.
static uint32_t fbRelocOld, fbRelocNew, fbRelocReadback, fbRelocHdp;
static uint64_t fbRelocBase;

void CRDriver::relocateFb() {
	if (!gpuMmio || checkKernelArgument("-crnoaccel") || checkKernelArgument("-crnofbreloc"))
		return;
	auto *mmio = gpuMmio;
	uint32_t location = mmio[Reg::MC_VM_FB_LOCATION];
	uint64_t base = static_cast<uint64_t>(location & 0xFFFF) << 24;
	uint64_t top = static_cast<uint64_t>(location >> 16) << 24;
	if (!base || top < base)
		return;
	uint32_t moved = static_cast<uint32_t>((top - base) >> 24) << 16;
	fbRelocOld = location;
	fbRelocNew = moved;
	fbRelocBase = base;
	// Scanout surfaces are MC addresses: move every enabled pipe's primary and secondary surface down with the FB.
	for (uint32_t pipe = 0; pipe < displayPipes(family); pipe++) {
		uint32_t r = pipeRegisterBase(pipe);
		bool powered = (mmio[Reg::PIPE0_PG_STATUS + 3 * pipe] >> 30) == 0;
		if (!powered || !(mmio[Reg::GRPH_ENABLE + r] & 1))
			continue;
		for (uint32_t surface = 0; surface < 2; surface++) {
			uint32_t low = Reg::GRPH_PRIMARY_SURFACE_ADDRESS + surface;  // primary, secondary
			uint32_t high = low + 3;                                     // GRPH_PRIMARY/SECONDARY_SURFACE_ADDRESS_HIGH
			uint64_t address = static_cast<uint64_t>(mmio[high + r]) << 32 | mmio[low + r];
			if (address >= base && address <= (top | 0xFFFFFF)) {
				address -= base;
				mmio[high + r] = static_cast<uint32_t>(address >> 32);
				mmio[low + r] = static_cast<uint32_t>(address);
			}
		}
	}
	mmio[Reg::MC_VM_FB_LOCATION] = moved;
	uint32_t hdp = mmio[Reg::HDP_NONSURFACE_BASE];
	if (hdp == static_cast<uint32_t>(base >> 8))
		mmio[Reg::HDP_NONSURFACE_BASE] = 0;
	fbRelocHdp = hdp;
	fbRelocReadback = mmio[Reg::MC_VM_FB_LOCATION];
	SYSLOG("cr", "FB window moved from 0x%llX to 0: MC_VM_FB_LOCATION 0x%08X -> 0x%08X (reads 0x%08X), HDP base 0x%X",
		base, location, moved, fbRelocReadback, hdp);
	iGPU->setProperty("CarrizoRed,fb-relocated", fbRelocReadback == moved);
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
		{"DMIF_HW_DEBUG", 0x2F8},
		{"DMIF_STATUS2", 0x304},
		{"PIXCLK0_RESYNC_CNTL", Reg::PIXCLK0_RESYNC_CNTL},
		{"PIXCLK1_RESYNC_CNTL", Reg::PIXCLK1_RESYNC_CNTL},
		{"PHYPLL_PIXCLK_CNTL", Reg::PHYPLL_PIXCLK_CNTL},
		{"CRTC0_PIXEL_RATE_CNTL", Reg::CRTC0_PIXEL_RATE_CNTL},
		{"CRTC1_PIXEL_RATE_CNTL", Reg::CRTC0_PIXEL_RATE_CNTL + 4},
		// Pixel clock from the DP DTO (eDP): phase / modulo = pixel clock / DPREFCLK; DPREFCLK from DENTIST.
		{"DP_DTO0_PHASE", 0x141},
		{"DP_DTO0_MODULO", 0x142},
		{"DP_DTO1_PHASE", 0x145},
		{"DP_DTO1_MODULO", 0x146},
		{"DPREFCLK_CNTL", 0x118},
		{"DENTIST_DISPCLK_CNTL", 0x124},
		{"DISPCLK_FREQ_CHANGE_CNTL", 0x131},
		{"DCCG_DISP_CNTL_REG", 0x13F},
		// Encoders / DP link (DIG0-2, DP0-2).
		{"DIG0_DIG_FE_CNTL", 0x4A00},
		{"DIG0_DIG_BE_CNTL", 0x4A47},
		{"DP0_DP_LINK_CNTL", 0x4AA0},
		{"DP0_DP_CONFIG", 0x4AA3},
		{"DP0_DP_VID_STREAM_CNTL", 0x4AA4},
		{"DP0_DP_LINK_FRAMING_CNTL", 0x4AAB},
		{"DP0_DP_DPHY_CNTL", 0x4AAF},
		{"DIG1_DIG_FE_CNTL", 0x4B00},
		{"DIG1_DIG_BE_CNTL", 0x4B47},
		{"DP1_DP_LINK_CNTL", 0x4BA0},
		{"DP1_DP_CONFIG", 0x4BA3},
		{"DP1_DP_VID_STREAM_CNTL", 0x4BA4},
		{"DP1_DP_LINK_FRAMING_CNTL", 0x4BAB},
		{"DP1_DP_DPHY_CNTL", 0x4BAF},
		{"DIG2_DIG_FE_CNTL", 0x4C00},
		{"DIG2_DIG_BE_CNTL", 0x4C47},
		{"DP2_DP_LINK_CNTL", 0x4CA0},
		{"DP2_DP_CONFIG", 0x4CA3},
		{"DP2_DP_VID_STREAM_CNTL", 0x4CA4},
		{"DP2_DP_DPHY_CNTL", 0x4CAF},
		// Panel power sequencer, backlight PWM, HPD.
		{"LVTMA_PWRSEQ_CNTL", 0x481B},
		{"LVTMA_PWRSEQ_STATE", 0x481C},
		{"BL_PWM_CNTL", 0x4820},
		{"BL_PWM_CNTL2", 0x4821},
		{"BL_PWM_PERIOD_CNTL", 0x4822},
		{"DC_GPIO_HPD_A", 0x488D},
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
		{"CRTC_H_TOTAL", 0x1B80},
		{"CRTC_V_TOTAL", 0x1B87},
		{"CRTC_BLANK_CONTROL", 0x1B9D},
		{"CRTC_STATUS", 0x1BA3},
		// Surface geometry, line buffer, scaler (spoof boot 28: link up, picture = identical rows / vertical stripes).
		{"GRPH_SWAP_CNTL", 0x1A03},
		{"GRPH_PITCH", 0x1A06},
		{"GRPH_SURFACE_OFFSET_X", 0x1A09},
		{"GRPH_SURFACE_OFFSET_Y", 0x1A0A},
		{"GRPH_X_START", 0x1A0B},
		{"GRPH_Y_START", 0x1A0C},
		{"GRPH_X_END", 0x1A0D},
		{"GRPH_Y_END", 0x1A0E},
		{"LB_DATA_FORMAT", 0x1AC0},
		{"LB_MEMORY_CTRL", 0x1AC1},
		{"LB_DESKTOP_HEIGHT", 0x1AC3},
		{"SCL_MODE", 0x1B42},
		{"SCL_BYPASS_CONTROL", 0x1B45},
		{"VIEWPORT_START", 0x1B5C},
		{"VIEWPORT_SIZE", 0x1B5D},
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
		uint32_t bootValue;  // as the firmware (GOP) left it
		bool bootValid;
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
		reg.bootValue = reg.value;
		reg.bootValid = true;
	}
	{
		uint32_t n = 0;
		for (auto &block : diffBlocks)
			for (uint32_t i = 0; i < block.count; i++)
				diffBoot[n++] = gpuMmio[block.start + i];
		diffBootValid = true;
	}
	// The DP link the firmware left running (video stream enabled) and its lane count (DP_CONFIG.DP_UDI_LANES:
	// 0 = 1, 1 = 2, 3 = 4 lanes); see wrapMaxLinkLanes.
	for (uint32_t dp = 0; dp < 3; dp++) {
		uint32_t stream = gpuMmio[0x4AA4 + 0x100 * dp], config = gpuMmio[0x4AA3 + 0x100 * dp];
		if (stream & 1) {
			uint32_t udi = config & 3;
			bootDpLanes = udi == 0 ? 1 : udi == 1 ? 2 : 4;
			bootDpDepth = static_cast<uint8_t>((gpuMmio[0x4AA1 + 0x100 * dp] >> 24) & 7);
			SYSLOG("cr", "boot: DP%u link running, %u lane(s) (DP_CONFIG 0x%x)", dp, bootDpLanes, config);
			break;
		}
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
void CRDriver::panicReport(uint32_t t, const char *tree, const char *reason) {
	static char report[8192];
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
	add("boot DP lanes %u (lane cap %s), LB size fixes %u%s, boot DP depth %u, bpc caps %u%s\n", bootDpLanes,
		checkKernelArgument("-crnolanecap") ? "off" : "on", lbFixes, lbFixOff ? " (off)" : "", bootDpDepth, bpcCaps,
		bpcCapOff ? " (off)" : "");
	if (restoreOn)
		add("restore: %u steps done, %u registers written, %u re-written after Apple changed them\n", restoreDone,
			restoreWrites, restoreHolds);
	add("assr: DPCD 0x00D cap 0x%02x, 0x10A before Apple 0x%02x, %u writes%s, readback 0x%02x\n", assrCap, assrBoot,
		assrWrites, assrOff ? " (off)" : "", assrReadback);
	if (!checkKernelArgument("-crnoaccel")) {
		auto flag = [&](const char *name) {
			auto *value = OSDynamicCast(OSBoolean, iGPU->getProperty(name));
			return value ? (value->isTrue() ? "1" : "0") : "-";
		};
		add("iGPU Load* props: AMDSupport %s Controller %s HWServices %s Accelerator %s\n", flag("LoadAMDSupport"),
			flag("LoadController"), flag("LoadHWServices"), flag("LoadAccelerator"));
		add("accel: multimedia engine power-ups skipped %u, arbiter patches %u; ", accelMmDropped, accelArbPatched);
		add("accel: sDMA1 dropped %u (aliased %u), sDMA0-only starts %u, caps fixed %u (CUs %u); vm fix %s FB %llx-%llx carve-out "
			"%llx: %u converted (%u MC, %u offset, %u other)",
			accelSdma1Dropped, accelSdma1Aliased, accelStartPatched, accelCapsFixed, accelCapsCu, vmFixOn ? "on" : "off", vmFbStart,
			vmFbEnd, vmCarveout, vmConverted, vmMcAddresses, vmOffsets, vmOther);
		for (uint32_t i = 0; i < vmConverted && i < arrsize(vmSamples); i++)
			add(" %c %llx->%llx", vmSamples[i].kind, vmSamples[i].from, vmSamples[i].to);
		add("\n");
	}
	if (gpuMmio) {
		// Interrupt path, end to end (spoof boot 35: picture right, beach ball frozen, handler never called). Display
		// sources (CRTC0 V_UPDATE enable / status, DISP_INTERRUPT_STATUS..CONTINUE2) -> IH (ring, pointers, write-back,
		// CNTL / STATUS) -> BIF (INTERRUPT_CNTL / CNTL2) -> MSI in config space -> BaffinInterruptManager::handler.
		auto *m = gpuMmio;
		add("irq: IH enable calls %u last %u; RB_CNTL %x BASE %x RPTR %x WPTR %x WPTR_ADDR %x:%x CNTL %x STATUS %x\n",
			ihEnableCalls, ihLastEnable, m[IhReg::RB_CNTL], m[IhReg::RB_BASE], m[IhReg::RB_RPTR], m[IhReg::RB_WPTR],
			m[IhReg::RB_WPTR_ADDR_HI], m[IhReg::RB_WPTR_ADDR_LO], m[IhReg::CNTL], m[IhReg::STATUS]);
		uint32_t wb = 0xFFFFFFFF;
		uint64_t ringGpu = 0;
		bool vram = false;
		if (auto *manager = static_cast<uint8_t *>(ihManager)) {
			ringGpu = *reinterpret_cast<uint64_t *>(manager + 0x48);
			if (ihGetWptrWbAddress)
				if (auto *p = reinterpret_cast<volatile uint32_t *(*)(void *)>(ihGetWptrWbAddress)(manager))
					wb = *p;
			if (ihIsUsingVram)
				vram = reinterpret_cast<bool (*)(void *)>(ihIsUsingVram)(manager);
		}
		add(" wb %x ring gpu %llx %s; BIF INTERRUPT_CNTL %x CNTL2 %x; DISP_INT %x %x %x; CRTC0 INT_CTL %x VUPD_STAT %x "
			"VINT0 %x\n",
			wb, ringGpu, vram ? "vram" : "sysmem", m[0x151A], m[0x151B], m[0x1857], m[0x1858], m[0x1859], m[0x1BB4],
			m[0x1BC4], m[0x1BCF]);
		if (auto *gpu = iGPU) {
			uint16_t command = gpu->configRead16(0x04), status = gpu->configRead16(0x06);
			uint8_t cap = gpu->configRead8(0x34) & 0xFC;
			for (int guard = 0; cap && guard < 48; guard++) {
				if (gpu->configRead8(cap) == 0x05) {
					uint16_t control = gpu->configRead16(cap + 2);
					bool wide = control & 0x80;
					add(" pci cmd %x status %x; msi control %x address %x%08x data %x\n", command, status, control,
						wide ? gpu->configRead32(cap + 8) : 0, gpu->configRead32(cap + 4),
						gpu->configRead16(cap + (wide ? 12 : 8)));
					break;
				}
				cap = gpu->configRead8(cap + 1) & 0xFC;
			}
		}
	}
	add("underflow: %u samples, xdma count %u, dmif %u, blnd %u, LB data level %u..%u (firmware 0x37 = 55)\n",
		underflowSamples, xdmaUnderflowMax, dmifUnderflows, blndUnderflows, lbLevelMin == ~0U ? 0 : lbLevelMin,
		lbLevelMax);
	add("t=%us regs %u ih %u missing-pipe %u pm %u cfgwr %u\n", t, __atomic_load_n(&regAccesses, __ATOMIC_RELAXED),
		__atomic_load_n(&ihHandlerCalls, __ATOMIC_RELAXED), __atomic_load_n(&pipeAccesses, __ATOMIC_RELAXED),
		__atomic_load_n(&pmEvents, __ATOMIC_RELAXED), __atomic_load_n(&pciConfigWrites, __ATOMIC_RELAXED));
	add("tree:\n%s", tree ? tree : "");
	// The display-debugging sections (VBIOS calls, DPCD, link training, block diff, surface and pixels) are left out of
	// accelerated boots, whose trace summary needs the room in NVRAM; -crdisplaydiag keeps them.
	bool displayDiag = checkKernelArgument("-crnoaccel") || checkKernelArgument("-crdisplaydiag");
	if (gpuMmio) {
		// Registers as offset=now/boot (boot = what the firmware's GOP left, with a working eDP link) where they
		// changed, then the offsets that still hold their boot value.
		add("regs now/boot:");
		bool powered[MaxWatchedPipes] {};
		for (uint32_t pipe = 0; pipe < MaxWatchedPipes && pipe < displayPipes(family); pipe++)
			powered[pipe] = pipePowered(pipe);
		for (size_t i = 0; i < watchedRegCount; i++) {
			auto &reg = watchedRegs[i];
			if (reg.pipe >= 0 && !powered[reg.pipe])
				continue;
			uint32_t now = gpuMmio[reg.offset];
			if (!reg.bootValid)
				add(" %x=%x/-", reg.offset, now);
			else if (now != reg.bootValue)
				add(" %x=%x/%x", reg.offset, now, reg.bootValue);
		}
		// The unchanged registers were listed in boots 22-25; leave them out to fit the DPCD transcript in NVRAM.
		add("\n");
		if (displayDiag)
			atomReport(add);
	}
	if (gpuMmio && diffBootValid && displayDiag) {
		add("diff now/boot:");
		uint32_t n = 0, changed = 0;
		for (auto &block : diffBlocks) {
			for (uint32_t i = 0; i < block.count; i++, n++) {
				uint32_t now = gpuMmio[block.start + i];
				if (now != diffBoot[n] && changed++ < 200)
					add(" %x=%x/%x", block.start + i, now, diffBoot[n]);
			}
		}
		add(" (%u changed)\n", changed);
	}
	// Pipe 0 surface registers, always (now/boot), and what the scanout memory actually holds: a few pixels on the top,
	// middle and bottom rows of the surface, read through BAR0 (the carve-out from MC_VM_FB_LOCATION's base). Spoof boot
	// 28 showed identical rows on the panel (vertical stripes): different rows in memory mean the display fetch
	// (underflow) is at fault, identical rows mean the content is.
	if (gpuMmio && displayDiag) {
		static const uint32_t surfaceRegs[] {0x1A00, 0x1A01, 0x1A03, 0x1A04, 0x1A06, 0x1A07, 0x1A09, 0x1A0A, 0x1A0B, 0x1A0C,
			0x1A0D, 0x1A0E, 0x1AC0, 0x1AC1, 0x1AC3, 0x1B42, 0x1B45, 0x1B5C, 0x1B5D, 0x2F7, 0x2F8, 0x304, 0x140};
		add("surface now/boot:");
		for (auto offset : surfaceRegs) {
			uint32_t now = gpuMmio[offset];
			const WatchedReg *found = nullptr;
			for (size_t i = 0; i < watchedRegCount; i++)
				if (watchedRegs[i].offset == offset)
					found = &watchedRegs[i];
			if (found && found->bootValid)
				add(" %x=%x/%x", offset, now, found->bootValue);
			else
				add(" %x=%x", offset, now);
		}
		add("\n");
		uint64_t fbBase = static_cast<uint64_t>(gpuMmio[Reg::MC_VM_FB_LOCATION] & 0xFFFF) << 24;
		uint64_t surface = (static_cast<uint64_t>(gpuMmio[0x1A07] & 0xFF) << 32) | (gpuMmio[0x1A04] & ~0xFFu);
		uint32_t pitchBytes = (gpuMmio[0x1A06] & 0x7FFF) * 4, height = gpuMmio[0x1A0E];
		if (surface >= fbBase && pitchBytes && height && height < 0x4000) {
			if (auto *map = iGPU->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0)) {
				auto *fb = reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());
				uint64_t length = map->getLength(), offset = surface - fbBase;
				add("pixels (surface +0x%llx, pitch %u bytes, %u rows):", offset, pitchBytes, height);
				const uint32_t rows[] {0, height / 2, height - 1}, columns[] {0, 1, 2, 100, 400, 683, 1000, 1365};
				for (auto row : rows) {
					add(" row %u:", row);
					for (auto column : columns) {
						uint64_t at = offset + static_cast<uint64_t>(row) * pitchBytes + column * 4ull;
						if (at + 4 <= length)
							add(" %x", *reinterpret_cast<volatile uint32_t *>(fb + at));
					}
				}
				add("\n");
				map->release();
			}
		}
	}
	// Is CRTC 0 running? Position and frame count, 20 ms (more than one frame) apart.
	if (gpuMmio && pipePowered(0)) {
		uint32_t pos1 = gpuMmio[0x1BA4], frame1 = gpuMmio[0x1BA6];
		IODelay(20000);
		uint32_t pos2 = gpuMmio[0x1BA4], frame2 = gpuMmio[0x1BA6];
		add("crtc0 position %x -> %x, frame %x -> %x\n", pos1, pos2, frame1, frame2);
	}
	add("rlc resets %u (CGCG_CGLS was %x, PG_CNTL was %x, MGCG_OVERRIDE was %x); ", rlcResets, rlcCgcgBefore, rlcPgBefore,
		rlcMgcgBefore);
	add("cail waits: %u, %u failed:", cailWaits, cailWaitFailures);
	for (uint32_t i = 0; i < cailWaitFailures && i < arrsize(cailWaitFails); i++)
		add(" [%x]&%x!=%x(%x r%x @%llu)", cailWaitFails[i].reg, cailWaitFails[i].mask, cailWaitFails[i].value,
			cailWaitFails[i].read, cailWaitFails[i].ret, cailWaitFails[i].ms);
	add("\n");
	// GFX state (nvram49: the CP was started but never went idle, and the RLC never acknowledged safe mode). VI
	// offsets (gfx_8_0_d.h / oss_3_0_d.h); each value is printed with its offset.
	if (gpuMmio && !checkKernelArgument("-crnogfxregs")) {
		static const uint16_t gfxRegs[] {
			0x2004, 0x2002, 0x0394, 0x0393,                  // GRBM_STATUS, GRBM_STATUS2, SRBM_STATUS, SRBM_STATUS2
			0xEC00, 0xEC04, 0xEC05, 0xEC40, 0xEC43, 0xEC49,  // RLC_CNTL, _STAT, _SAFE_MODE, _GPM_STAT, _PG_CNTL, _CGCG_CGLS_CTRL
			0xEC61, 0xEC62,                                  // RLC_SERDES_CU/NONCU_MASTER_BUSY
			0x21A0, 0x21B6, 0x208D,                          // CP_STAT, CP_ME_CNTL, CP_MEC_CNTL
			0x3040, 0x3041, 0x21C0, 0x3045,                  // CP_RB0_BASE, _CNTL, _RPTR, _WPTR
		};
		add("gfx:");
		for (auto reg : gfxRegs)
			add(" %x=%x", reg, gpuMmio[reg]);
		add("\n");
		// nvram51: the CP still stalls at ring dword 0x88 with fetches pending. CP_STALLED_STAT3/1/2, CP_BUSY_STAT,
		// CP_STAT, the ME/PFP/CE header dumps (last packet headers) and the CPF status (0x219C-0x21A8); the ring and
		// indirect-buffer registers (0x21C0-0x21D7); VM context 0/1 protection fault status and address
		// (VM_CONTEXT0/1_PROTECTION_FAULT_STATUS 0x536/0x537, _ADDR 0x53E/0x53F).
		static const struct {
			uint16_t first, last;
		} cpRanges[] {{0x219C, 0x21A4}, {0x21C0, 0x21C2}, {0x536, 0x537}, {0x53E, 0x53F},
			// nvram60 (gfx_8_0_d.h / oss_3_0_d.h): CP_IB1_OFFSET, CP_CE_IB1_OFFSET, CP_IB1_BASE_LO/HI, CP_IB1_BUFSZ;
			// CP_CPF_STATUS/BUSY_STAT/STALLED_STAT1; CP_ROQ_IB1_STAT .. CP_CE_ROQ_RB_STAT; SRBM_STATUS3, SRBM_READ_ERROR,
			// SRBM_INT_STATUS, SRBM_READ_ERROR2; CP_RB0_BASE_HI, CP_RB_DOORBELL_CONTROL; RLC_GPR_REG2 (CZ safe mode).
			{0xC092, 0xC092}, {0xC098, 0xC098}, {0xC0CC, 0xC0CE}, {0x2087, 0x2089}, {0x21E1, 0x21E8}, {0x395, 0x395},
			{0x3A6, 0x3A6}, {0x3A9, 0x3A9}, {0x3AE, 0x3AE}, {0x30B1, 0x30B1}, {0x3059, 0x3059}, {0xEC7A, 0xEC7A},
			// nvram63: the first fetch on each stream (ring, IB) returns, the next never does. GPU memory path
			// (gmc_8_1_d.h): VM_L2_CNTL..VM_DUMMY_PAGE_FAULT_CNTL, VM_CONTEXT0_CNTL2, VM_INVALIDATE_RESPONSE,
			// VM_CONTEXT0_PROTECTION_FAULT_DEFAULT_ADDR, VM_CONTEXT0_PAGE_TABLE_BASE/START/END_ADDR, VM_L2_CNTL4,
			// MC_VM_AGP_TOP..MC_VM_SYSTEM_APERTURE_DEFAULT_ADDR, MC_VM_MX_L1_TLB_CNTL, MC_VM_FB_OFFSET; the CP's
			// read-pointer writeback and WPTR poll addresses (CP_RB0_RPTR_ADDR/_HI, CP_RB_WPTR_POLL_ADDR_LO/_HI).
			{0x500, 0x506}, {0x50C, 0x50C}, {0x51F, 0x51F}, {0x546, 0x546}, {0x54F, 0x54F}, {0x557, 0x557},
			{0x55F, 0x55F}, {0x578, 0x578}, {0x80A, 0x80F}, {0x819, 0x81A}, {0x3043, 0x3047},
			// nvram66: the CP's second ring fetch never returns. Cache channel set-up, where Stoney differs from
			// Polaris 11 (Linux stoney_golden_*: GB_ADDR_CONFIG 0x12010001, TCP_CHAN_STEER_LO 0x10101010,
			// TCP_ADDR_CONFIG 0xF1, TCC_EXE_DISABLE bit 1): GB_ADDR_CONFIG, TCP_CHAN_STEER_LO/HI, TCP_ADDR_CONFIG,
			// TCI_CNTL_1/2, TCC_CTRL, TCC_EXE_DISABLE, HDP_HOST_PATH_CNTL, HDP_ADDR_CONFIG/MISC_CNTL; memory hub
			// status: MC_HUB_MISC_STATUS, MC_HUB_WDP_ERR, MC_HUB_WDP_STATUS, _RDREQ_STATUS, _WRRET_STATUS,
			// MC_HUB_MISC_IDLE_STATUS, MC_ARB_BUSY_STATUS, MC_VM_MB_L1_TLB0-2_STATUS.
			{0x263E, 0x263E}, {0x2B03, 0x2B05}, {0x2B62, 0x2B63}, {0x2B80, 0x2B80}, {0x2B84, 0x2B84}, {0xB00, 0xB00},
			{0xBD2, 0xBD3}, {0x832, 0x832}, {0x836, 0x836}, {0x838, 0x83A}, {0x847, 0x847}, {0x9FD, 0x9FD},
			{0x895, 0x897},
			// RLC set-up (gfx_8_0_d.h): RLC_DEBUG_SELECT/DEBUG, RLC_MEM_SLP_CNTL, RLC_LB_CNTL,
			// RLC_SAVE_AND_RESTORE_BASE, RLC_JUMP_TABLE_RESTORE, RLC_GPM_THREAD_ENABLE, RLC_CGTT_MGCG_OVERRIDE,
			// RLC_PG_DELAY, RLC_PG_ALWAYS_ON_CU_MASK .. RLC_AUTO_PG_CTRL, RLC_SRM_CNTL, RLC_CSIB_ADDR_LO/HI/LENGTH,
			// CP_MEM_SLP_CNTL, CGTS_SM_CTRL_REG.
			{0xEC01, 0xEC02}, {0xEC06, 0xEC06}, {0xEC19, 0xEC19}, {0xEC1D, 0xEC1E}, {0xEC45, 0xEC45}, {0xEC48, 0xEC48},
			{0xEC4D, 0xEC4D}, {0xEC53, 0xEC55}, {0xEC80, 0xEC80}, {0xECA2, 0xECA4}, {0x3079, 0x3079}, {0xF000, 0xF000},
			// ATC (gmc_8_1_d.h): VM apertures 0/1 low/high/CNTL/CNTL2, ATS_CNTL/DEBUG/FAULT_DEBUG/STATUS/FAULT_CNTL,
			// MISC_CG, L2_CNTL/CNTL2/DEBUG/DEBUG2, VMID_PASID_MAPPING_UPDATE_STATUS and VMID0-15_PASID_MAPPING.
			{0xCC0, 0xCC7}, {0xCC9, 0xCCD}, {0xCD4, 0xCD8}, {0xCE6, 0xCF6}};
		// The GFX ring's contents around the read pointer (nvram54: stuck at dword 0x88 with an SRBM request pending).
		// AMDBaffinHardware +0x3B0 = PM4 engine, +0x50 = its GFX ring (AMDVICommandsRing <- AMDCommandRing: +0x30 size in
		// dwords, +0x40 CPU mapping, +0x48 GPU address).
		if (auto *hw = static_cast<uint8_t *>(accelHardware)) {
			auto *pm4 = *reinterpret_cast<uint8_t **>(hw + 0x3B0);
			auto *ring = pm4 ? *reinterpret_cast<uint8_t **>(pm4 + 0x50) : nullptr;
			auto *cpu = ring ? *reinterpret_cast<uint32_t **>(ring + 0x40) : nullptr;
			uint32_t dwords = ring ? *reinterpret_cast<uint32_t *>(ring + 0x30) : 0;
			uint32_t rptr = gpuMmio[0x21C0];
			// From dword 0 (nvram55: the fetcher was at 0x88, past the start sequence, which ends with SET_BASE at 0x80),
			// with runs of the GFX8 NOP filler (PACKET3(NOP, 0x3FFF) = 0xFFFF1000) collapsed to "*n".
			if (cpu && dwords >= 0x100) {
				add("ring %llx (%u dw):", ring ? *reinterpret_cast<uint64_t *>(ring + 0x48) : 0, dwords);
				uint32_t end = rptr + 8 < 0x200 ? rptr + 8 : 0x200;
				for (uint32_t i = 0; i < end && i < dwords;) {
					if (i == rptr)
						add(" |");
					uint32_t run = 0;
					while (i + run < end && cpu[i + run] == 0xFFFF1000 && (run == 0 || i + run != rptr))
						run++;
					if (run > 1) {
						add(" *%u", run);
						i += run;
					} else {
						add(" %x", cpu[i]);
						i++;
					}
				}
				add("\n");
				// The first INDIRECT_BUFFER on the ring (the clear-state buffer, in "VRAM"): its packets, read from the
				// carve-out's physical memory (MC_VM_FB_OFFSET << 22 + the address, the FB window being at 0; nvram61: BAR0
				// is smaller than the 512 MB carve-out and the IB sits at its top). Type-3 packets as op/count, with the
				// register of SET_*_REG and the destination of WRITE_DATA, "@" before the first packet past ROQ_IB1's
				// read pointer; type-0 as t0:register/count.
				for (uint32_t i = 0; i + 3 < end && i < dwords; i++) {
					if (cpu[i] != 0xC0023F00)
						continue;
					uint64_t ib = (static_cast<uint64_t>(cpu[i + 2] & 0xFFFF) << 32) | (cpu[i + 1] & ~3U);
					uint32_t ibDwords = cpu[i + 3] & 0xFFFFF;
					uint64_t fbSize = (static_cast<uint64_t>(gpuMmio[Reg::MC_VM_FB_LOCATION] >> 16) + 1) << 24;
					uint64_t phys = (static_cast<uint64_t>(gpuMmio[Reg::MC_VM_FB_OFFSET] & 0x3FFFF) << 22) + ib;
					IOMemoryDescriptor *desc = nullptr;
					IOMemoryMap *map = nullptr;
					if (ibDwords && ib + ibDwords * 4ULL <= fbSize &&
						(desc = IOMemoryDescriptor::withPhysicalAddress(phys, ibDwords * 4, kIODirectionIn)))
						map = desc->map(kIOMapInhibitCache);
					if (!map) {
						add("ib %llx (%u dw) phys %llx: not mapped\n", ib, ibDwords, phys);
						OSSafeReleaseNULL(desc);
						break;
					}
					auto *data = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
					uint32_t roqIb1 = gpuMmio[0x21E1] & 0x3FF;  // CP_ROQ_IB1_STAT.ROQ_RPTR_INDIRECT1 (dwords fetched)
					add("ib %llx (%u dw) phys %llx, roq %x:", ib, ibDwords, phys, roqIb1);
					uint32_t packets = 0;
					bool marked = false;
					for (uint32_t j = 0; j < ibDwords && packets < 80; packets++) {
						if (!marked && j >= roqIb1) {
							add(" @");
							marked = true;
						}
						uint32_t header = data[j], type = header >> 30, count = ((header >> 16) & 0x3FFF) + 1;
						if (type == 3) {
							uint32_t op = (header >> 8) & 0xFF;
							if ((op == 0x68 || op == 0x69 || op == 0x76 || op == 0x79) && j + 1 < ibDwords)
								add(" %x/%u:%x", op, count, data[j + 1]);
							else if (op == 0x37 && j + 3 < ibDwords)
								add(" %x/%u:%x>%x", op, count, data[j + 1], data[j + 2]);
							else
								add(" %x/%u", op, count);
							j += 1 + count;
						} else if (type == 0) {
							add(" t0:%x/%u", header & 0xFFFF, count);
							j += 1 + count;
						} else {
							add(" t%u:%x", type, header);
							j++;
						}
					}
					add("\n");
					map->release();
					desc->release();
					break;
				}
			} else {
				add("ring: none (hw %d pm4 %d ring %d cpu %d, %u dw)\n", hw != nullptr, pm4 != nullptr, ring != nullptr,
					cpu != nullptr, dwords);
			}
		}
		// GART (VMID0) entries for the CP's writeback page (fence at 0x20000000, read pointer at 0x20000008) and the
		// ring page, and what the GPU wrote there. nvram64: the VM set-up matches Linux, the first read on each stream
		// returns and the next never does, as if a GPU write to system memory never completed and blocked the queue.
		// The table is in "VRAM" (VM_CONTEXT0_PAGE_TABLE_BASE_ADDR), read through the carve-out's physical memory.
		{
			uint64_t carveout = static_cast<uint64_t>(gpuMmio[Reg::MC_VM_FB_OFFSET] & 0x3FFFF) << 22;
			uint64_t table = static_cast<uint64_t>(gpuMmio[0x54F]) << 12, start = static_cast<uint64_t>(gpuMmio[0x557]) << 12;
			uint64_t fbTop = (static_cast<uint64_t>(gpuMmio[Reg::MC_VM_FB_LOCATION] >> 16) + 1) << 24;
			auto readPhys = [](uint64_t phys, uint32_t bytes, void *out) {
				auto *desc = IOMemoryDescriptor::withPhysicalAddress(phys, bytes, kIODirectionIn);
				auto *map = desc ? desc->map(kIOMapInhibitCache) : nullptr;
				bool mapped = map != nullptr;
				if (mapped)
					memcpy(out, reinterpret_cast<const void *>(map->getVirtualAddress()), bytes);
				OSSafeReleaseNULL(map);
				OSSafeReleaseNULL(desc);
				return mapped;
			};
			add("gart: table %llx start %llx:", table, start);
			if (table + 0x200 <= fbTop && start) {
				uint64_t ptes[0x22] {};
				if (readPhys(carveout + table, sizeof(ptes), ptes)) {
					static const uint32_t indices[] {0, 0x20, 0x21};
					for (uint32_t index : indices) {
						uint64_t pte = ptes[index], page = pte & 0xFFFFFFFFF000ULL;
						uint32_t words[4] {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
						if ((pte & 1) && page)
							readPhys(page, sizeof(words), words);
						add(" [%x] %llx -> %x %x %x %x", index, pte, words[0], words[1], words[2], words[3]);
					}
				} else {
					add(" unmapped");
				}
			}
			add("\n");
		}
		add("cp:");
		for (auto &range : cpRanges) {
			add(" %x:", range.first);
			for (uint32_t reg = range.first; reg <= range.last; reg++)
				add(" %x", gpuMmio[reg]);
		}
		add("\n");
	}
	add("fbreloc: base %llx loc %x -> %x (reads %x, now %x), hdp was %x now %x; sysap %x-%x\n", fbRelocBase,
		fbRelocOld, fbRelocNew, fbRelocReadback, gpuMmio ? gpuMmio[Reg::MC_VM_FB_LOCATION] : 0, fbRelocHdp,
		gpuMmio ? gpuMmio[Reg::HDP_NONSURFACE_BASE] : 0, gpuMmio ? gpuMmio[Reg::MC_VM_SYSTEM_APERTURE_LOW_ADDR] : 0,
		gpuMmio ? gpuMmio[Reg::MC_VM_SYSTEM_APERTURE_HIGH_ADDR] : 0);
	// CAIL's Stoney set-up ends with stoney_init_enable_ats -> Cail_Spectre_EnableAts, which (if its caps allow) writes
	// the iGPU's own PCIe capabilities: ATS control (0x2B6 = 0x8000), PRI control/allocation (0x2C4, 0x2CC) and PASID
	// control (0x2D6). Those are IOMMUv2 features macOS doesn't set up for AMD.
	add("ats (skipped %u): 2b6=%x 2c4=%x 2cc=%x 2d6=%x\n", atsSkipped, iGPU->extendedConfigRead16(0x2B6), iGPU->extendedConfigRead16(0x2C4),
		iGPU->extendedConfigRead32(0x2CC), iGPU->extendedConfigRead16(0x2D6));
	// Microcode RAM read-back (nvram70: the RLC is enabled but never runs, and current Linux only loads GFX8 microcode
	// through the SMU). Read the first words of each RAM back through the ADDR/DATA pairs CAIL wrote them with
	// (gfx_8_0_d.h); expected (Stoney images, = linux-firmware): RLC c4080015 308c0001, CE d8400022 d80003f6,
	// PFP c4380015 c7b4000b, ME d8400018 cc0003e4, MEC c424000b 8000019f. Zeros or garbage mean the writes were dropped.
	if (gpuMmio && !checkKernelArgument("-crnoucodecheck")) {
		static const struct {
			const char *name;
			uint16_t addr, data;
		} rams[] {{"rlc", 0xF83C, 0xF83D}, {"ce", 0xF818, 0xF819}, {"pfp", 0xF814, 0xF815}, {"me", 0xF816, 0xF817},
			{"mec", 0xF81A, 0xF81B}};
		add("ucode:");
		for (auto &ram : rams) {
			gpuMmio[ram.addr] = 0;
			uint32_t w0 = gpuMmio[ram.data], w1 = gpuMmio[ram.data], w2 = gpuMmio[ram.data];
			add(" %s %x %x %x", ram.name, w0, w1, w2);
		}
		// nvram71: none of them read back as the Stoney images. Write-then-read test on word 0 of the RLC and PFP RAMs
		// (both dead by now): a RAM that keeps 0x5A5A1234 accepts direct loading; anything else means the driver can't
		// write it and only the SMU can load these APUs' microcode, as current Linux does.
		static const struct {
			const char *name;
			uint16_t addr, data;
		} tests[] {{"rlc", 0xF83C, 0xF83D}, {"pfp", 0xF814, 0xF815}};
		for (auto &ram : tests) {
			gpuMmio[ram.addr] = 0;
			gpuMmio[ram.data] = 0x5A5A1234;
			gpuMmio[ram.addr] = 0;
			add(" %s-wr %x", ram.name, gpuMmio[ram.data]);
		}
		add("\n");
		// The SMU's own view (Linux smu8_smumgr: SMU8_Firmware_Header at SMU SRAM 0x1FF80, read through
		// MP0PUB_IND_INDEX/DATA at SMN 0x10000000 + address): firmware version, UcodeLoadStatus (bitmap of the engines it
		// has loaded), signature; and the message mailbox (SMU_MP1_SRBM2P_MSG_0/RESP_0/ARG_0).
		{
			auto smuSram = [](uint32_t offset) {
				gpuMmio[0x180] = 0x10000000 + 0x1FF80 + offset;
				return gpuMmio[0x181];
			};
			add("smu: version %x load-status %x signature %x; msg %x resp %x arg %x\n", smuSram(0), smuSram(0x18),
				smuSram(0x7C), gpuMmio[0x1C0], gpuMmio[0x1D0], gpuMmio[0x1E0]);
		}
		// Our SMU load (smuLoadFirmware): CAIL's block and our region, the SMU's answers to DriverDramAddrHi/Lo,
		// InitJobs and the three ExecuteJobs (1 = OK; Bxxxx busy, Cxxxx no answer), UcodeLoadStatus before/after.
		add("smuload: %s; format calls %u cail size %x region +%x block fb+%llx mc %llx phys %llx tasks %u; resp %x %x %x "
			"%x %x %x; status %x -> %x in %u ms; mec ic %x %x cntl %x\n", smuLoad.result ? smuLoad.result : "not run",
			smuFormatCalls, smuCailSize, smuRegionStart, smuLoad.blockOffset, smuLoad.mc, smuLoad.phys, smuLoad.tasks,
			smuLoad.resp[0], smuLoad.resp[1], smuLoad.resp[2], smuLoad.resp[3], smuLoad.resp[4], smuLoad.resp[5],
			smuLoad.before, smuLoad.after, smuLoad.ms, smuLoad.mecBase[0], smuLoad.mecBase[1], smuLoad.mecBase[2]);
		add("hooks: hwlibs routed %x failed %x; peci reads %u; bin_image checks %u restores %u, earlyInitialize %x\n",
			hwlibsHooks, hwlibsHookFails, peciReads, binImageChecks, binImageRestores, earlyInitResult);
	}
	add("soft pptable: %u bytes rev %x\n", softPPTableSize, softPPTableRev);
	add("breadcrumb: %u writes, %u refused%s\n", breadcrumbWrites, breadcrumbRefused,
		breadcrumbNoOptions ? ", no /options" : "");
	add("trace: %s\nlast events:", traceSummary);
	// Newest 16 events, oldest first, without the repetitive attribute and vblank-wait calls.
	uint32_t head = __atomic_load_n(&traceHead, __ATOMIC_RELAXED);
	uint32_t picked[16], count = 0;
	for (uint32_t i = head; i != 0 && head - i < TraceRingSize && count < 16;) {
		i--;
		auto &event = traceRing[i % TraceRingSize];
		if (!event.name || !strncmp(event.name, "fb.setAttribute", 15) || !strncmp(event.name, "tg.wait", 7))
			continue;
		picked[count++] = i;
	}
	while (count) {
		auto &event = traceRing[picked[--count] % TraceRingSize];
		add(" %s%s=%x", event.exit ? "" : ">", event.name, event.value);
	}
	panic("%s\n%s", reason, report);
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
