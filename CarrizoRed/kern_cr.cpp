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

// IH storm breaker. IH registers are dword offsets in BAR5, the same on Tonga (oss_3_0) and CZ (oss_3_0_1).
namespace IhReg {
	static constexpr uint32_t RB_CNTL = 0xE30, RB_BASE = 0xE31, RB_RPTR = 0xE32, RB_WPTR = 0xE33, RB_WPTR_ADDR_HI = 0xE34,
	                          RB_WPTR_ADDR_LO = 0xE35, CNTL = 0xE36, STATUS = 0xE38;
}
static volatile uint32_t *ihMmio;
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

enum KextIndex : size_t {
	KextRadeonX4000,
	KextRadeonX4000HWServices,
	KextPolarisController,
	KextSupport,
	KextFramebuffer,
	KextAgpm,
	KextAgdp,
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

	logPCIInfo();
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
		else if (i == KextSupport) {
			patchSupport(patcher, index, slide, size);
			traceSupport(patcher, index, slide, size);
		}
		else if (i == KextFramebuffer)
			traceFramebuffer(patcher, index, slide, size);
		else if (i == KextAgpm) {
			blockProbe(patcher, index, slide, size, "__ZN14AGPMController5probeEP9IOServicePi", "-crnoagpm");
			blockProbe(patcher, index, slide, size, "__ZN4AGPM5probeEP9IOServicePi", "-crnoagpm");
		} else if (i == KextAgdp && checkKernelArgument("-crnoagdp")) {
			KernelPatcher::RouteRequest request {"__ZN32AppleGraphicsDeviceControlClient5startEP9IOService", wrapStartFail};
			if (!patcher.routeMultiple(index, &request, 1, slide, size)) {
				SYSLOG("cr", "-crnoagdp: failed to route (error %d)", patcher.getError());
				patcher.clearError();
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
void CRDriver::patchPolarisController(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size) {
	if (checkKernelArgument("-crnopipes")) {
		SYSLOG("cr", "pipe limit patch disabled by -crnopipes");
		return;
	}
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
		if (patcher.routeMultiple(index, &request, 1, slide, size)) {
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
		if (!patcher.routeMultiple(index, &request, 1, slide, size)) {
			SYSLOG("cr", "failed to route ATIController::TestVRAM (error %d)", patcher.getError());
			patcher.clearError();
		}
	}
}

bool CRDriver::wrapTestVram(void *that, uint32_t region, bool flag) {
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
//  CFG_NO_MSI (opt-in with -crnomsi): legacy interrupts instead of MSI for the display controller.
static const struct {
	const char *name;
	const char *arg;
	bool optIn;  // apply only with arg (else: apply unless arg)
} configOverrides[] {
	{"CFG_NON_ZERO_MC_LOC", "-crnomcloc", false},
	{"CFG_NO_PP", "-crpp", false},
	{"CFG_NO_MSI", "-crnomsi", true},
};

uint64_t CRDriver::wrapGetConfigProperty(void *that, const char *name, void *value, uint32_t size) {
	auto ret = FunctionCast(wrapGetConfigProperty, callback->orgGetConfigProperty)(that, name, value, size);
	if (!name || !value || !size)
		return ret;
	for (auto &option : configOverrides) {
		if (!strcmp(name, option.name) && checkKernelArgument(option.arg) == option.optIn) {
			memset(value, 0, size);
			*static_cast<uint8_t *>(value) = 1;
			traceEvent(option.name, true, 1);
		}
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
		static_assert(arrsize(stages) == 43, "one wrapper per stage");
		mach_vm_address_t wrappers[] {
			CR_WRAP(0), CR_WRAP(1), CR_WRAP(2), CR_WRAP(3), CR_WRAP(4), CR_WRAP(5), CR_WRAP(6), CR_WRAP(7),
			CR_WRAP(8), CR_WRAP(9), CR_WRAP(10), CR_WRAP(11), CR_WRAP(12), CR_WRAP(13), CR_WRAP(14), CR_WRAP(15),
			CR_WRAP(16), CR_WRAP(17), CR_WRAP(18), CR_WRAP(19), CR_WRAP(20), CR_WRAP(21), CR_WRAP(22), CR_WRAP(23),
			CR_WRAP(24), CR_WRAP(25), CR_WRAP(26), CR_WRAP(27), CR_WRAP(28), CR_WRAP(29), CR_WRAP(30), CR_WRAP(31),
			CR_WRAP(32), CR_WRAP(33), CR_WRAP(34), CR_WRAP(35), CR_WRAP(36), CR_WRAP(37), CR_WRAP(38), CR_WRAP(39),
			CR_WRAP(40), CR_WRAP(41), CR_WRAP(42),
		};
		for (size_t i = 0; i < arrsize(stages); i++) {
			if (stages[i].kext != kext)
				continue;
			KernelPatcher::RouteRequest request {stages[i].symbol, wrappers[i], stages[i].org};
			if (!patcher.routeMultiple(index, &request, 1, slide, size)) {
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
	if (!patcher.routeMultiple(index, &request, 1, slide, size)) {
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
	if (checkKernelArgument("-crnotrace"))
		return;
	routeStages(patcher, index, slide, size, InSupport);
	KernelPatcher::RouteRequest requests[] {
		{"__ZN13ATIController5probeEP9IOServicePi", wrapAtiProbe, orgAtiProbe},
		{"__ZN13ATIController5startEP9IOService", wrapAtiStart, orgAtiStart},
	};
	for (auto &request : requests) {
		if (!patcher.routeMultiple(index, &request, 1, slide, size)) {
			SYSLOG("cr", "trace: failed to route %s (error %d)", request.symbol, patcher.getError());
			patcher.clearError();
		}
	}
}

IOService *CRDriver::wrapAtiProbe(IOService *that, IOService *provider, SInt32 *score) {
	IOLog("CarrizoRed: %s::probe ...\n", that->getMetaClass()->getClassName());
	auto *ret = FunctionCast(wrapAtiProbe, callback->orgAtiProbe)(that, provider, score);
	IOLog("CarrizoRed: %s::probe -> %s\n", that->getMetaClass()->getClassName(), ret ? "match" : "NO MATCH");
	traceNote("%s.probe=%s ", that->getMetaClass()->getClassName(), ret ? "match" : "NO");
	return ret;
}

bool CRDriver::wrapAtiStart(IOService *that, IOService *provider) {
	IOLog("CarrizoRed: %s::start ...\n", that->getMetaClass()->getClassName());
	bool ret = FunctionCast(wrapAtiStart, callback->orgAtiStart)(that, provider);
	IOLog("CarrizoRed: %s::start -> %s\n", that->getMetaClass()->getClassName(), ret ? "OK" : "FAILED");
	traceNote("%s.start=%s ", that->getMetaClass()->getClassName(), ret ? "OK" : "FAILED");
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
	if (patcher.routeMultiple(index, &request, 1, slide, size)) {
		SYSLOG("cr", "%s: %s will not match", arg, probe);
	} else {
		SYSLOG("cr", "%s: failed to route %s (error %d)", arg, probe, patcher.getError());
		patcher.clearError();
	}
}

bool CRDriver::wrapStartFail(IOService *, IOService *) { return false; }

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
		if (!patcher.routeMultiple(index, &request, 1, slide, size)) {
			SYSLOG("cr", "interrupts: failed to route %s (error %d)", request.symbol, patcher.getError());
			patcher.clearError();
		}
	}
	stormBreak = !checkKernelArgument("-crnostormbreak");
	ihGetWptrWbAddress = patcher.solveSymbol(index, "__ZN22BaffinInterruptManager23getWPTRWriteBackAddressEv", slide, size);
	ihIsUsingVram = patcher.solveSymbol(index, "__ZN22BaffinInterruptManager24isUsingVRAMForRingBufferEv", slide, size);
	patcher.clearError();
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
	if (++ihWindowCalls > 500 && ihMmio && callback->stormBreak) {
		auto *mmio = ihMmio;
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

IOService *CRDriver::wrapProbeNoMatch(IOService *, IOService *, SInt32 *) { return nullptr; }

uint32_t CRDriver::wrapGetDisplayLimit(void *) { return displayPipes(callback->family); }

uint8_t CRDriver::wrapGetNumLineBuffers(void *) { return displayPipes(callback->family); }

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
	// Keep the mapping: the IH storm breaker writes registers directly (see wrapIhHandler).
	ihMmio = mmio;
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
}

void CRDriver::disableKextCallbacks() {
	for (auto &kext : kexts)
		kext.switchOff();
}
