//
//  kern_cr.cpp
//  CarrizoRed
//
//  Phase 1: observe only. Finds the iGPU, logs what the hardware reports, and
//  watches Apple's Polaris kexts load. Nothing is patched yet.
//

#include "kern_cr.hpp"
#include "kern_vbios.hpp"
#include "kern_sigs.hpp"

#include <Headers/kern_api.hpp>
#include <Headers/kern_devinfo.hpp>
#include <Headers/kern_iokit.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <kern/debug.h>
#include <kern/clock.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <pexpert/pexpert.h>
#include <sys/fcntl.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/vnode_if.h>
#include <stdarg.h>

// Lilu's createTrampoline copies a wrapped function's first instructions verbatim ("assuming it is PIC") and jumps back
// after them; nothing is relocated. When the wrapper is more than 2 GB away (kext addresses change every boot) the copy
// covers at least 14 bytes, and a stack-protector prologue's `mov rax, [rip + __stack_chk_guard@GOT]` at +0xb then reads
// from the wrong address. Spoof boot 19's panic (AAPL,PanicInfo): WindowServer opening a framebuffer ran
// AMD9500Controller::hwInitializeHardwareSettings through such a trampoline, loaded "aryP12OS" as the guard pointer and
// took a general-protection fault, which never reached the screen. ATIController::start and initializeProjectInfo have
// the same prologue. Every wrapper with an original therefore goes through routeChecked, which refuses (and logs) any
// function with a RIP-relative operand or a relative branch in its first 14 bytes, whatever the jump size.
//
// Trampolines: Lilu builds every plugin's in one shared 4 KB buffer (KernelPatcher::tempExecutableMemory). Once it is
// full, createTrampoline returns 0, routeFunctionInternal returns EINVAL, and routeMultiple takes that non-zero value for
// the trampoline's address and reports success: the function is left untouched and nothing is logged as failed. With
// ~150 traced stages CarrizoRed alone needs more than 4 KB, so whichever kext loaded last lost its hooks; HWLibs, loaded
// last on most boots, lost CAIL's (nvram133/40: earlyInitialize still Apple's prologue at setupCAIL, the "re-patch"
// reported success and changed nothing, yet a direct write landed). So routeChecked builds the trampoline itself, in
// its own pool, has Lilu write only the jump (no wrapper, so no trampoline), and reads the jump back.
static constexpr size_t RoutePoolSize = 16384;
static uint8_t routePool[RoutePoolSize] __attribute__((section("__TEXT,__text"), aligned(16)));
static size_t routePoolUsed;
static uint32_t routesOk, routesFailed, routesUnverified;
static const char *routeLastFailed;

// HWLibs functions by signature (kern_sigs.hpp) where the symbol is missing: builds before Sequoia ship HWLibs
// stripped. Each signature is searched once per load address; the occurrence-th match is the function.
static uint32_t hwsigResolved, hwsigMissed;
static mach_vm_address_t hwlibsTextStart;  // HWLibs' __TEXT_EXEC (found when it loads)
static uint64_t hwlibsTextSize;

// All of this version's signatures are resolved together, on the first lookup for a load, while HWLibs is still
// unpatched: a signature can run past its function into the next one (Monterey's 128-byte Stoney_EnableCpPowerGating
// signature covers the start of Stoney_UpdateGfxPowerGating, 0x44 bytes on), and a route written there first broke the
// match (hwsig 35/2 on the first Monterey boot).
static mach_vm_address_t hwsigFind(const char *symbol, mach_vm_address_t start, size_t size) {
	static mach_vm_address_t found[arrsize(hwlibsSignatures)];
	static mach_vm_address_t foundStart;
	// Only HWLibs' executable segment, which must lie in the kext being patched.
	if (!hwlibsTextStart || hwlibsTextStart < start || hwlibsTextStart + hwlibsTextSize > start + size)
		return 0;
	if (foundStart != start) {
		memset(found, 0, sizeof(found));
		foundStart = start;
		auto *base = reinterpret_cast<const uint8_t *>(hwlibsTextStart);
		for (size_t s = 0; s < arrsize(hwlibsSignatures); s++) {
			auto &sig = hwlibsSignatures[s];
			// Only the entries checked against this macOS version (an occurrence-th match is build specific).
			if (sig.darwin != static_cast<uint32_t>(getKernelVersion()))
				continue;
			uint32_t seen = 0;
			for (size_t i = 0; !found[s] && i + sig.size <= hwlibsTextSize; i++) {
				size_t k = 0;
				while (k < sig.size && (base[i + k] & sig.mask[k]) == (sig.pattern[k] & sig.mask[k]))
					k++;
				if (k == sig.size && seen++ == sig.occurrence)
					found[s] = hwlibsTextStart + i;
			}
			if (found[s])
				hwsigResolved++;
			else
				hwsigMissed++;
		}
	}
	for (size_t s = 0; s < arrsize(hwlibsSignatures); s++)
		if (found[s] && !strcmp(hwlibsSignatures[s].symbol, symbol))
			return found[s];
	return 0;
}

// The symbol, else its signature (for HWLibs functions on stripped builds).
static mach_vm_address_t solveOrSignature(KernelPatcher &patcher, size_t index, const char *symbol,
	mach_vm_address_t slide, size_t size) {
	auto addr = patcher.solveSymbol(index, symbol, slide, size);
	if (addr)
		return addr;
	addr = hwsigFind(symbol, slide, size);
	if (addr)
		patcher.clearError();
	return addr;
}

// Set around a route that may move a conditional jump (see below); off by default, so the hooks refused before stay out.
static bool routeAllowJcc;

static bool routeChecked(KernelPatcher &patcher, size_t index, KernelPatcher::RouteRequest &request,
	mach_vm_address_t slide, size_t size) {
	if (request.org) {
		auto func = patcher.solveSymbol(index, request.symbol, slide, size);
		bool bySignature = false;
		if (!func) {
			func = hwsigFind(request.symbol, slide, size);
			bySignature = func != 0;
			if (bySignature)
				patcher.clearError();
		}
		if (!func) {
			SYSLOG("cr", "route: %s not found (error %d)", request.symbol, patcher.getError());
			return false;
		}
		// The trampoline: the instructions the jump overwrites, moved as they are, except a conditional jump, which becomes
		// the inverse short jump over a jmp [rip+0] to its original target (AMDAccelVideoContext::getHWInfo has
		// "test rsi, rsi; je" inside its first 14 bytes).
		uint8_t code[96];
		size_t copied = 0, codeLen = 0;
		while (copied < 14) {
			hde64s hs {};
			size_t len = Disassembler::hdeDisasm(func + copied, &hs);
			if (!len || (hs.flags & F_ERROR)) {
				SYSLOG("cr", "route: REFUSED %s: cannot decode +0x%lx", request.symbol, copied);
				return false;
			}
			// A function shorter than the patch (nvram115/116: the 6-byte AMDSWScheduler::signalTimeout) would have
			// the jump spill into the next one.
			bool ends = hs.opcode == 0xC3 || hs.opcode == 0xC2 || (hs.opcode == 0xFF && (hs.flags & F_MODRM) &&
				(hs.modrm_reg == 4 || hs.modrm_reg == 5));
			if (ends && copied + len < 14) {
				SYSLOG("cr", "route: REFUSED %s: function ends at +0x%lx, inside the patch", request.symbol, copied + len);
				return false;
			}
			bool ripRelative = (hs.flags & F_MODRM) && hs.modrm_mod == 0 && hs.modrm_rm == 5;
			bool jcc = (hs.flags & F_RELATIVE) && ((hs.opcode & 0xF0) == 0x70 || (hs.opcode == 0x0F &&
				(hs.opcode2 & 0xF0) == 0x80));
			if (jcc && routeAllowJcc && codeLen + 16 <= sizeof(code)) {
				int64_t disp = (hs.flags & F_IMM8) ? static_cast<int8_t>(hs.imm.imm8) : static_cast<int32_t>(hs.imm.imm32);
				mach_vm_address_t target = func + copied + len + disp;
				if (target >= func && target < func + 14) {
					SYSLOG("cr", "route: REFUSED %s: branch at +0x%lx lands inside the patch", request.symbol, copied);
					return false;
				}
				uint8_t cc = (hs.opcode == 0x0F ? hs.opcode2 : hs.opcode) & 0x0F;
				const uint8_t skip[8] {static_cast<uint8_t>(0x70 | (cc ^ 1)), 14, 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
				memcpy(code + codeLen, skip, sizeof(skip));
				memcpy(code + codeLen + sizeof(skip), &target, sizeof(target));
				codeLen += sizeof(skip) + sizeof(target);
				copied += len;
				continue;
			}
			if (ripRelative || (hs.flags & F_RELATIVE) || codeLen + len > sizeof(code)) {
				SYSLOG("cr", "route: REFUSED %s: %s instruction at +0x%lx would be copied unrelocated", request.symbol,
					ripRelative ? "RIP-relative" : "relative-branch", copied);
				return false;
			}
			memcpy(code + codeLen, reinterpret_cast<const void *>(func + copied), len);
			codeLen += len;
			copied += len;
		}
		// A branch later in the function into the bytes the jump overwrites would land inside it (Big Sur's
		// AMDHardware::initializeHWEngines loops back to +0xC: invalid opcode there, nvram22). Look through the next
		// 0x400 bytes; a decoding stop or a branch from beyond the function only makes this more careful.
		for (size_t at = copied; at < 0x400;) {
			hde64s hs {};
			size_t len = Disassembler::hdeDisasm(func + at, &hs);
			if (!len || (hs.flags & F_ERROR))
				break;
			if (hs.flags & F_RELATIVE) {
				int64_t disp = (hs.flags & F_IMM8) ? static_cast<int8_t>(hs.imm.imm8) : static_cast<int32_t>(hs.imm.imm32);
				mach_vm_address_t target = func + at + len + disp;
				if (target > func && target < func + copied) {
					SYSLOG("cr", "route: REFUSED %s: branch at +0x%lx goes to +0x%llx, inside the patch", request.symbol, at,
						target - func);
					routesFailed++;
					routeLastFailed = request.symbol;
					return false;
				}
			}
			at += len;
		}
		// Trampoline: the moved instructions (checked above), then jmp [rip+0] back to func + copied.
		if (routePoolUsed + codeLen + 14 > RoutePoolSize) {
			SYSLOG("cr", "route: %s: trampoline pool full (%lu bytes used)", request.symbol, routePoolUsed);
			routesFailed++;
			routeLastFailed = request.symbol;
			return false;
		}
		auto *tramp = routePool + routePoolUsed;
		if (MachInfo::setKernelWriting(true, nullptr) != KERN_SUCCESS) {
			SYSLOG("cr", "route: %s: cannot write the trampoline", request.symbol);
			routesFailed++;
			routeLastFailed = request.symbol;
			return false;
		}
		memcpy(tramp, code, codeLen);
		static const uint8_t jumpBack[6] {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
		memcpy(tramp + codeLen, jumpBack, sizeof(jumpBack));
		mach_vm_address_t back = func + copied;
		memcpy(tramp + codeLen + sizeof(jumpBack), &back, sizeof(back));
		MachInfo::setKernelWriting(false, nullptr);
		routePoolUsed += (codeLen + 14 + 15) & ~static_cast<size_t>(15);
		*request.org = reinterpret_cast<mach_vm_address_t>(tramp);
		KernelPatcher::RouteRequest jump {request.symbol, request.to};
		bool written;
		if (bySignature) {
			// No symbol for Lilu to resolve: write the jump at the address.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
			patcher.routeFunctionLong(func, request.to, false, true, false);
#pragma clang diagnostic pop
			written = patcher.getError() == KernelPatcher::Error::NoError;
		} else {
			written = patcher.routeMultipleLong(index, &jump, 1, slide, size);
		}
		if (!written) {
			SYSLOG("cr", "route: %s: failed to write the jump (error %d)", request.symbol, patcher.getError());
			*request.org = 0;
			routesFailed++;
			routeLastFailed = request.symbol;
			return false;
		}
		// Lilu's long route: jmp [rip+0] (FF 25 00000000) followed by the destination.
		auto *entry = reinterpret_cast<const uint8_t *>(func);
		mach_vm_address_t dest;
		memcpy(&dest, entry + 6, sizeof(dest));
		if (entry[0] != 0xFF || entry[1] != 0x25 || dest != request.to) {
			SYSLOG("cr", "route: %s: the jump didn't land (%02x %02x)", request.symbol, entry[0], entry[1]);
			routesUnverified++;
			routeLastFailed = request.symbol;
			return false;
		}
		routesOk++;
		return true;
	}
	return patcher.routeMultiple(index, &request, 1, slide, size);
}

// Short summary of the bring-up trace, repeated by the watcher so it survives the console scrolling.
static char traceSummary[2560];
static uint16_t softPPTableSize, softPPTableRev;  // see setSoftPowerPlayTable

static void diskLogEvent(const char *fmt, ...);

// Also into the disk log: an Apple panic carries no report, and NVRAM written by the kernel is lost without a clean
// shutdown (Ventura nvram1/2: a stale CR-progress), so the log is where a crashed boot's notes survive.
static void traceNote(const char *fmt, ...) {
	char note[192];
	va_list va;
	va_start(va, fmt);
	vsnprintf(note, sizeof(note), fmt, va);
	va_end(va);
	diskLogEvent("note: %s", note);
	size_t len = strlen(traceSummary);
	if (len + 2 >= sizeof(traceSummary))
		return;
	strlcpy(traceSummary + len, note, sizeof(traceSummary) - len);
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

static constexpr size_t TraceRingSize = 512, MaxTraceStages = 192;
// Catalina's GPU power-up runs ~40 s longer than Sonoma's (slow CAIL waits) but completes, so it gets more room.
static constexpr uint64_t WatchdogMs = 60000, WatchdogMsCatalina = 180000;
static TraceEvent traceRing[TraceRingSize];
static uint32_t traceHead;
static uint32_t stageOpen[MaxTraceStages];
static uint64_t stageEnterMs[MaxTraceStages];
// Catalina: when acceleration doesn't come up, AMD9500Controller has no PowerPlay manager (+0x1A070 stays NULL after
// the boot-time initializePowerPlay fails), but WindowServer's framebuffer open (AMDFramebuffer::enableHardware) calls
// initializePowerPlay again, which dereferences it unchecked (nvram25/28: page fault at +0x85). The wrapper then
// returns the boot-time error instead. 0 = off (other versions, or the load at +0x7E didn't match).
static uint32_t ppmGuardOff;
static uint32_t stageCalls[MaxTraceStages], stageLastArg[MaxTraceStages];
static const char *stageNames[MaxTraceStages];
// crstopat=<stage>[#n]: on entering that traced stage (the n-th time, default 1st), block the calling thread and have the
// watcher panic with the report. nvram81/82: with real microcode the machine froze somewhere after
// Stoney_LoadRlcUcode; this walks the stop point forward without a freeze swallowing the report.
static char stopAtName[64];
static uint32_t stopAtCount = 1, stopAtHits;
static const char *stoppedStage;

// Lost flip completions. nvram108/120: after hundreds to thousands of flips, one display-pipe transaction never gets
// its completion ('txni' -> AMDAccelDisplayPipe::processTransactionInterrupt) while vblank interrupts keep coming and
// the GPU is idle, so WindowServer never presents again (frozen screen). The watcher delivers it: when the newest
// transaction has been performed for 250 ms without its completion, call processTransactionInterrupt itself (harmless
// if it's not pending: it only signals when the pipe's transaction-pending flag is set). -crnoflipkick disables it.
static void *flipPipe;               // AMDAccelDisplayPipe of the last performTransaction
static uint64_t flipPerformMs;       // when it was performed
static uint32_t flipPerforms, flipIrqs, flipKicks;
static mach_vm_address_t flipProcessIrq;  // AMDAccelDisplayPipe::processTransactionInterrupt

static uint64_t uptimeMs();
static bool smuLoadOk();  // see the SMU loader

static void kickStalledFlip() {
	if (!flipPipe || !flipProcessIrq || checkKernelArgument("-crnoflipkick"))
		return;
	uint32_t performs = __atomic_load_n(&flipPerforms, __ATOMIC_RELAXED), irqs = __atomic_load_n(&flipIrqs, __ATOMIC_RELAXED);
	if (performs <= irqs || uptimeMs() - flipPerformMs < 250)
		return;
	flipKicks++;
	reinterpret_cast<void (*)(void *, void *, void *)>(flipProcessIrq)(flipPipe, nullptr, nullptr);
}

// Start order. The graphics policy sets LoadController and LoadAccelerator together, so the display controller and the
// accelerator stack match in parallel. When HWServices/the accelerator start first (nvram109/113/124), CAIL's early
// initialisation (AmdCailServices::earlyInitialize) is never called at all: no registry settings, no SMU load, no
// PowerPlay, no GUI. Until AMD9500Controller has finished its hardware set-up (initializeProjectDependentResources),
// their probes report no match; the watcher then re-registers the iGPU so they match again. Off unless -cracceldefer.
static bool controllerReady, accelDeferred, accelDeferOff;
static uint32_t accelDeferrals, accelRematches;

// AMDHardware::setupCAIL reads the VBIOS from the iGPU's ATY,bin_image and, if it's missing, returns before ever
// initialising CAIL (AmdCailServices::earlyInitialize): no registry settings, no SMU load, no PowerPlay, no GUI. The
// property can be gone by then (nvram124/127; the restore in wrapCailEarlyInitialize never ran because that call never
// came). Put our copy back on entry to setupCAIL too, and count how often it was missing there.
static uint32_t binImageSetupChecks, binImageSetupMissing;
static void restoreVbiosImageForSetup();
// The live HWLibs copy (see checkLiveHwlibs): nvram124/127/128 called earlyInitialize through the CAIL object's vtable
// with ATY,bin_image present, yet none of the HWLibs hooks ran.
static void checkLiveHwlibs(void *hardware);

// RB+ (render backend plus). Stoney is the only GFX8 chip with it; Mesa (radeonsi, rbplus_allowed) programs
// SX_PS_DOWNCONVERT, SX_BLEND_OPT_EPSILON, SX_BLEND_OPT_CONTROL and SX_MRT0-7_BLEND_OPT for every draw. Apple's Polaris
// Metal driver has never heard of them (Polaris has no RB+), so on Stoney they keep the clear-state defaults, and
// blend optimisations left on with arbitrary settings would break alpha-blended drawing (the icons and Dock render
// wrong while opaque windows are fine). The clear-state buffer (RLC_CSIB_ADDR_LO/HI, RLC_CSIB_LENGTH = 0xECA2-0xECA4,
// in VRAM) holds the context registers' defaults that CLEAR_STATE loads: record what it sets for 0x28750-0x2877C
// (context register indices 0x1D4-0x1DF) and set SX_BLEND_OPT_CONTROL (0x1D6) to every MRT's colour and alpha
// optimisation off plus PIXEN_ZERO_OPT_DISABLE (0xB3333333). -crnorbplusfix only records.
static bool rbplusDone;
// Every context register the clear-state buffer sets (what a context starts with before Metal sets anything), for the
// one-time context dump (censusLog).
static uint32_t csbCtx[0x400], csbCtxSet[0x400 / 32];
static uint32_t rbplusValues[12], rbplusSeen, rbplusPatched, rbplusCsibDwords;
static uint64_t rbplusCsib;

static void fixRbPlus(volatile uint32_t *mmio, bool stoney) {
	if (rbplusDone || !mmio || !stoney)
		return;
	uint64_t mc = static_cast<uint64_t>(mmio[0xECA3]) << 32 | mmio[0xECA2];
	uint32_t dwords = mmio[0xECA4];
	if (!mc || !dwords || dwords > 0x2000)
		return;
	rbplusDone = true;
	rbplusCsib = mc;
	rbplusCsibDwords = dwords;
	uint64_t fbBase = static_cast<uint64_t>(mmio[Reg::MC_VM_FB_LOCATION] & 0xFFFF) << 24;
	uint64_t fbTop = (static_cast<uint64_t>(mmio[Reg::MC_VM_FB_LOCATION] >> 16) + 1) << 24;
	if (mc < fbBase || mc + dwords * 4 > fbTop)
		return;
	uint64_t phys = (static_cast<uint64_t>(mmio[Reg::MC_VM_FB_OFFSET] & 0x3FFFF) << 22) + (mc - fbBase);
	auto *desc = IOMemoryDescriptor::withPhysicalAddress(phys, dwords * 4, kIODirectionInOut);
	auto *map = desc ? desc->map(kIOMapInhibitCache) : nullptr;
	if (map) {
		auto *buf = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
		bool patch = !checkKernelArgument("-crnorbplusfix");
		for (uint32_t i = 0; i < dwords;) {
			uint32_t header = buf[i];
			if ((header >> 30) != 3) {
				i++;
				continue;
			}
			uint32_t count = ((header >> 16) & 0x3FFF) + 1, opcode = (header >> 8) & 0xFF;
			if (opcode == 0x69 && count >= 2 && i + count < dwords) {  // SET_CONTEXT_REG: start index, values
				uint32_t start = buf[i + 1];
				for (uint32_t k = 0; k + 1 < count; k++) {
					uint32_t reg = start + k;
					if (reg < 0x400) {
						csbCtx[reg] = buf[i + 2 + k];
						csbCtxSet[reg / 32] |= 1U << (reg % 32);
					}
					if (reg >= 0x1D4 && reg <= 0x1DF) {
						rbplusValues[reg - 0x1D4] = buf[i + 2 + k];
						rbplusSeen |= 1U << (reg - 0x1D4);
						if (reg == 0x1D6 && patch) {
							buf[i + 2 + k] = 0xB3333333;
							rbplusPatched++;
						}
					}
				}
			}
			i += count + 1;
		}
		traceNote("rbplus=%x/%u ", rbplusSeen, rbplusPatched);
	}
	OSSafeReleaseNULL(map);
	OSSafeReleaseNULL(desc);
}

// GPU not responding: every register reads 0xFFFFFFFF, the PCI "nobody home" value (nvram99/137: CAIL's waits read
// RLC_SAFE_MODE as 0xFFFFFFFF and nothing after that could work; a full power-off fixed it). Checked at start-up
// (probeMMIO) and by the watcher every tick: GRBM_STATUS and MC_VM_FB_LOCATION are never all-ones on a live GPU.
static bool gpuDeadAtStart;
static uint64_t gpuDeadMs;  // uptime when it was first seen dead (0: never)
static uint64_t uptimeMs();

static bool gpuRegistersDead(volatile uint32_t *mmio) {
	return mmio && mmio[0x2004] == 0xFFFFFFFF && mmio[0x809] == 0xFFFFFFFF;  // GRBM_STATUS, MC_VM_FB_LOCATION
}

static void checkGpuAlive(volatile uint32_t *mmio) {
	if (!gpuDeadMs && gpuRegistersDead(mmio)) {
		uint64_t ms = uptimeMs();
		gpuDeadMs = ms ? ms : 1;
		IOLog("CarrizoRed: the GPU stopped responding (registers read 0xFFFFFFFF); power the machine off fully\n");
	}
}

// Defined with the SMU loader / black box below; called from the stage wrappers.
static void smuReloadCp();
// Set by a full SMU load (boot, wake), cleared by the next pm4.initializeMicroEngine: that one needs no CP reload.
static bool smuFreshLoad;
static void snapshotHang(uint32_t arg);

static void parseStopAt() {
	if (!PE_parse_boot_argn("crstopat", stopAtName, sizeof(stopAtName)))
		stopAtName[0] = '\0';
	if (char *hash = strchr(stopAtName, '#')) {
		*hash = '\0';
		stopAtCount = 0;
		for (char *c = hash + 1; *c >= '0' && *c <= '9'; c++)
			stopAtCount = stopAtCount * 10 + static_cast<uint32_t>(*c - '0');
		if (!stopAtCount)
			stopAtCount = 1;
	}
}
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
static bool edpSeen;                 // Apple enabled an eDP transmitter (connector 0x14) this boot
static volatile bool edpWakePowerOn;  // a full wake is under way: power the eDP panel before the next AUX transaction
// Catalina wake #7 (log 1793307460): with the panel powered, AUX still failed after the full wake, and the machine was
// restarted 10 s later, before the 30 s report. The first failed DPCD transfer of a full wake brings the report forward
// to 1 s later and adds the display sections (VBIOS calls, DPCD, the register diff against the firmware's boot state).
static volatile bool wakeAuxWatch, wakeDisplayDiag;
static volatile bool wakeRestorePending;  // a full wake is under way: put back the display set-up S3 lost, see below
static uint32_t restoreAfterWake();

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
static uint32_t probeValues[4];       // CONFIG_MEMSIZE, MC_VM_FB_LOCATION, MC_VM_FB_OFFSET, GRBM_STATUS at the mapping
static uint64_t probeMs, probePhys;
static uint32_t mmioMapMethod, mmioMapFailures;  // 1 IOKit, 2 BAR5 from config space; failed attempts
// crsmustep=N: stop the SMU load (smuLoadFirmware) after step N and have the watcher panic with the report. nvram74:
// the full load froze the whole machine (no crpanicat panic); a boot that freezes without a report points at the step.
static uint32_t smuStepLimit = 0xFF, smuStoppedAt;
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
static mach_vm_address_t ihGetWptrWbAddress, ihIsUsingVram, ihSetRingBufferLocation;
static bool ihStormBroken;
static uint64_t ihWindowStart;
static uint32_t ihWindowCalls, ihWindowWptr, ihStormFalseAlarms;
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
// Catalina's IOPCIFamily still has the flat bundle layout.
static const char *pathPci[] {"/System/Library/Extensions/IOPCIFamily.kext/Contents/MacOS/IOPCIFamily",
	"/System/Library/Extensions/IOPCIFamily.kext/IOPCIFamily"};

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

static uint32_t accelKextLoads[3], accelKextRepeats[3];  // X4000, HWServices, HWLibs: new loads, repeats (skipped)
static mach_vm_address_t accelKextSlide[3];  // and where the last load was

static KernelPatcher::KextInfo kexts[KextCount] {
	// HWLibs is reloadable (Lilu otherwise patches only the first load): nvram72/109/113 ran an unpatched HWLibs while
	// every hook "installed" into an earlier copy, as if the library was unloaded and loaded again. Not X4000 and
	// HWServices: with them reloadable, boots 115/116 crashed in AMDSWScheduler::advance at ~58 s.
	[KextRadeonX4000] = {"com.apple.kext.AMDRadeonX4000", pathRadeonX4000, arrsize(pathRadeonX4000), {}, {},
		KernelPatcher::KextInfo::Unloaded},
	[KextRadeonX4000HWServices] = {"com.apple.kext.AMDRadeonX4000HWServices", pathRadeonX4000HWServices,
		arrsize(pathRadeonX4000HWServices), {}, {}, KernelPatcher::KextInfo::Unloaded},
	[KextRadeonX4000HWLibs] = {"com.apple.kext.AMDRadeonX4000HWLibs", pathRadeonX4000HWLibs,
		arrsize(pathRadeonX4000HWLibs), {false, true}, {}, KernelPatcher::KextInfo::Unloaded},
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

// Stoney's shader instruction set. Apple's Metal compiler (AMDShared.bundle's libSC.dylib, run in MTLCompilerService)
// picks its back end with SCGetInstructionSet(family, revision, device): family 0x82 (VI, what the Metal driver reports
// for every GFX8 chip) with a revision outside Tonga's 0x14-0x27 gives the generic VI set 0x0E, which SCCreate turns into
// SCIcelandInfo (baseline GFX 8.0); family 0x87 (CZ) with revision 0x61-0xFE, i.e. Stoney, gives 0x1E, which becomes
// SCGfx81Info, AMD's own Stoney back end (16 LDS banks: v_interp_p1_f32 must not reuse its source register, which a GFX
// 8.0 back end does, giving wrong texture coordinates; Carrizo itself gets 0x0E). The Metal driver only knows VI, so make
// the VI branch return 0x1E.
//
// libSC exists only inside the dyld shared cache. Lilu's user patcher can't help: it is switched off on Big Sur and
// later (Configuration::getBootArguments sets isUserDisabled), so neither its binary patches nor its process callbacks
// ever run (nvram144: no MTLCompilerService start seen), and it would locate a patch by reading the library from disk
// anyway. The shared cache's pages are validated through cs_validate_page (not cs_validate_range: nvram142, 0 of 2730
// pages matched), with the vnode and the page's file offset. Wrap it, as RestrictEvents does for its cache patches, and
// after a successful validation patch the page in place: it is then shared by every process that maps the cache. The
// match is 72 bytes of SCGetInstructionSet at 0xD78 in its page (file offset 0xEAED78 of dyld_shared_cache_x86_64h.03,
// macOS 15.8.1 24H32); the patched byte is the VI branch's `mov eax, 0x0E`. -crnoscpatch disables it. nvram145: patched
// and in use, no visible change.
//
// The same hook fixes two context registers the Metal driver (AMDMTLBronzeDriver, also in .03) programs for Baffin
// that Stoney must not get (nvram146, read from WindowServer's command buffer):
// - PA_SU_SMALL_PRIM_FILTER_CNTL (0x28830) = 5, the small-primitive filter, which culls triangles that cover no sample.
//   amdMtlBronzeInitStaticHwCtxRegs sets it from the chip record's supportsSmallPrimFilter bit (`and ecx, 5`). Mesa
//   enables the filter only after Polaris 12 (Polaris has a sample location bug with it) and never on Stoney, which
//   predates it. Make it `and ecx, 0`: the register is then written as 0. -crnosmallprim disables this.
// - PA_SC_MODE_CNTL_1 (0x28A4C) = 0x060201B8, i.e. WALK_FENCE_SIZE 3 (amdMtlBronzeInitRenderEncoderHwCtxRegs, a 64-bit
//   immediate). Mesa uses 2 on 2-pipe chips (Stoney: GB_ADDR_CONFIG NUM_PIPES 1) and 3 otherwise. Make it 0x060201A8.
//   -crnowalkfence disables this.
static const uint8_t scPatchFind[] {
	0x83, 0xC6, 0xD8, 0x83, 0xFE, 0x13, 0x77, 0x12, 0x83, 0xE2, 0xFE, 0xB8, 0x0D, 0x00, 0x00, 0x00, 0x81, 0xFA,
	0xA0, 0x67, 0x00, 0x00, 0x75, 0x02, 0x5D, 0xC3, 0xB8, 0x0C, 0x00, 0x00, 0x00, 0x5D, 0xC3, 0x83, 0xC6, 0xEC,
	0xB8, 0x0E, 0x00, 0x00, 0x00, 0x83, 0xFE, 0x13, 0x77, 0xEA, 0x81, 0xC2, 0xD7, 0x96, 0xFF, 0xFF, 0x83, 0xFA,
	0x07, 0x73, 0xDF, 0x48, 0x63, 0xC2, 0x48, 0x8D, 0x0D, 0x41, 0x82, 0x0B, 0x00, 0x8B, 0x04, 0x81, 0x5D, 0xC3,
};
// mov ecx, [rsi + 0xD8]; shl ecx, 10; sar ecx, 31; and ecx, 5; xor edx, edx; mov [rdi + 0x38], ecx
static const uint8_t smallPrimFind[] {
	0x8B, 0x8E, 0xD8, 0x00, 0x00, 0x00, 0xC1, 0xE1, 0x0A, 0xC1, 0xF9, 0x1F, 0x83, 0xE1, 0x05, 0x31, 0xD2, 0x89, 0x4F, 0x38,
};
// movabs rcx, 0x060201B800000002; mov [rdi + 0x2DC], rcx; mov dword [rdi + 0x2E4], 0x1E9
static const uint8_t walkFenceFind[] {
	0x48, 0xB9, 0x02, 0x00, 0x00, 0x00, 0xB8, 0x01, 0x02, 0x06, 0x48, 0x89, 0x8F, 0xDC, 0x02, 0x00, 0x00, 0xC7, 0x87,
	0xE4, 0x02, 0x00, 0x00, 0xE9, 0x01, 0x00, 0x00,
};
// The texture layout code (setSharedTextureParams' callee) computes DCC metadata, and so gives the texture delta colour
// compression, only when a flag is set: `test byte [rbp - 0x2C], 1; je <no DCC>`. nvram146/147: WindowServer's render
// targets have CB_COLOR_INFO.DCC_ENABLE. Make the je a jmp (90 E9, same displacement) so no texture gets DCC.
// -crdcc keeps DCC.
static const uint8_t dccFind[] {
	0x41, 0x89, 0x45, 0x00, 0x4D, 0x89, 0xE1, 0x49, 0x8B, 0x04, 0x24, 0x4C, 0x8B, 0x45, 0xA0, 0x48, 0x8B, 0x55, 0xA8, 0xF6,
	0x45, 0xD4, 0x01, 0x0F, 0x84, 0x78, 0x01, 0x00, 0x00, 0x4D, 0x89, 0xCC, 0x48, 0x8D, 0xB5, 0x40, 0xFE, 0xFF, 0xFF, 0x48,
};
// Ventura (13.7.8, read from the D509BA's cache): the same SCGetInstructionSet, small-prim and walk-fence code at other
// page offsets (isa without its trailing lea, whose displacement is build specific), and the DCC flag test compiled the
// other way round: test r8b, 1; jne <DCC set-up out of line> (the same set-up as 15.8.1's: mov dword [rsi], 0x40; mov
// dword [rdx], 0x20; shr eax, 9). Make the jne a 6-byte nop so no texture gets DCC. Each pattern occurs once in the cache.
static const uint8_t dccVenturaFind[] {
	0x41, 0x3B, 0x07, 0x76, 0x03, 0x41, 0x89, 0x07, 0x41, 0x89, 0xD8, 0x41, 0xF6, 0xC0, 0x01, 0x0F, 0x85, 0xB4, 0x00, 0x00, 0x00,
};
// Sonoma (14.x, read from the D509BA's cache): the same three at other page offsets again, and the DCC flag test compiled
// as in 15.8.1 (test byte [rsp + 8], 1; je <no DCC>): make the je a jmp. Each pattern occurs once in that cache only.
static const uint8_t dccSonomaFind[] {
	0x8B, 0x84, 0x24, 0xB8, 0x00, 0x00, 0x00, 0x3B, 0x03, 0x76, 0x02, 0x89, 0x03, 0x4C, 0x8B, 0x54, 0x24, 0x20, 0xF6, 0x44,
	0x24, 0x08, 0x01, 0x0F, 0x84, 0x80, 0x01, 0x00, 0x00,
};
// Monterey (12.7.6 21H1320, read from the D509BA's cache, all four in .2): the VI branch of SCGetInstructionSet behind a
// different preceding branch; the small-prim mask reading the chip record at +0xC4 instead of +0xD8, with no xor edx after
// it; the walk-fence immediate through rax instead of rcx; the DCC test as in 15.8.1 (test byte [rbp - 0x2C], 1, then a
// mov before the je). Same replacements. Each pattern occurs once in that cache.
static const uint8_t isaMontereyFind[] {
	0x83, 0xE6, 0xFE, 0x31, 0xC0, 0x81, 0xFE, 0x80, 0x00, 0x00, 0x00, 0x0F, 0x95, 0xC0, 0x8D, 0x04, 0x40, 0x83, 0xC0, 0x13,
	0x5D, 0xC3, 0x83, 0xC6, 0xEC, 0xB8, 0x0E, 0x00, 0x00, 0x00, 0x83, 0xFE, 0x13, 0x77, 0x46, 0x81, 0xC2, 0xD7, 0x96, 0xFF,
	0xFF, 0x83, 0xFA, 0x07, 0x73, 0x3B,
};
static const uint8_t smallPrimMontereyFind[] {
	0x8B, 0x8E, 0xC4, 0x00, 0x00, 0x00, 0xC1, 0xE1, 0x0A, 0xC1, 0xF9, 0x1F, 0x83, 0xE1, 0x05, 0x89, 0x4F, 0x38,
};
static const uint8_t walkFenceMontereyFind[] {
	0x48, 0xB8, 0x02, 0x00, 0x00, 0x00, 0xB8, 0x01, 0x02, 0x06, 0x48, 0x89, 0x87, 0xDC, 0x02, 0x00, 0x00, 0xC7, 0x87,
	0xE4, 0x02, 0x00, 0x00, 0xE9, 0x01, 0x00, 0x00,
};
static const uint8_t dccMontereyFind[] {
	0xF6, 0x45, 0xD4, 0x01, 0x48, 0x8B, 0x5D, 0x90, 0x0F, 0x84, 0x94, 0x00, 0x00, 0x00, 0x48, 0x8D, 0xB5, 0xA0, 0xFE, 0xFF,
	0xFF,
};
// Big Sur (11.7.11 20G1443, one cache file): SCGetInstructionSet with 32-bit branches, the small-prim mask in edx
// (chip record +0xC4), Monterey's walk-fence bytes, and the DCC test as in 15.8.1 with a mov less before the je. Each
// pattern occurs once in that cache.
static const uint8_t isaBigSurFind[] {
	0x83, 0xE2, 0xFE, 0xB8, 0x0D, 0x00, 0x00, 0x00, 0x81, 0xFA, 0xA0, 0x67, 0x00, 0x00, 0x0F, 0x84, 0x0F, 0x01, 0x00, 0x00,
	0xB8, 0x0C, 0x00, 0x00, 0x00, 0x5D, 0xC3, 0x83, 0xC6, 0xEC, 0xB8, 0x0E, 0x00, 0x00, 0x00, 0x83, 0xFE, 0x13, 0x0F, 0x87,
	0xF7, 0x00, 0x00, 0x00, 0x81, 0xC2, 0xD7, 0x96, 0xFF, 0xFF, 0x83, 0xFA, 0x07, 0x0F, 0x83, 0xE8, 0x00, 0x00, 0x00,
};
static const uint8_t smallPrimBigSurFind[] {
	0x8B, 0x96, 0xC4, 0x00, 0x00, 0x00, 0xC1, 0xE2, 0x0A, 0xC1, 0xFA, 0x1F, 0x83, 0xE2, 0x05, 0x89, 0x57, 0x38,
};
static const uint8_t dccBigSurFind[] {
	0xF6, 0x45, 0xD4, 0x01, 0x0F, 0x84, 0x8F, 0x00, 0x00, 0x00, 0x0F, 0x57, 0xC0, 0x48, 0x8D, 0xB5, 0x50, 0xFE, 0xFF, 0xFF,
};
// Catalina (10.15.7 19H2): not in the dyld cache. The compiler is AMDShared.bundle's libSC.dylib, whose
// SCGetInstructionSet numbers the targets differently: the VI branch (family 130) returns 4, and the CZ branch returns
// 0x0E (GFX81) for revision 0x61 and up, i.e. Stoney. Make the VI branch's `mov eax, 4` a `mov eax, 0x0E`. The other
// three are in AMDMTLBronzeDriver: the small-prim mask from the chip record's +0xAC, the walk-fence immediate stored at
// +0x2D8 (through rax, with the next register's value in its high half), and the DCC flag test on [rbp - 0x30]. Each
// pattern occurs once in its file.
static const uint8_t isaCatalinaFind[] {
	0x83, 0xC6, 0xEC, 0xB8, 0x04, 0x00, 0x00, 0x00, 0x83, 0xFE, 0x13, 0x0F, 0x87, 0xE9, 0x00, 0x00, 0x00, 0x81, 0xC2, 0xD7,
	0x96, 0xFF, 0xFF, 0x83, 0xFA, 0x07, 0x0F, 0x83, 0xDA, 0x00, 0x00, 0x00,
};
static const uint8_t smallPrimCatalinaFind[] {
	0x8B, 0x96, 0xAC, 0x00, 0x00, 0x00, 0xC1, 0xE2, 0x0A, 0xC1, 0xFA, 0x1F, 0x83, 0xE2, 0x05, 0x89, 0x57, 0x40,
};
static const uint8_t walkFenceCatalinaFind[] {
	0x48, 0xB8, 0xB8, 0x01, 0x02, 0x06, 0xE9, 0x01, 0x00, 0x00, 0x48, 0x89, 0x87, 0xD8, 0x02, 0x00, 0x00,
};
static const uint8_t dccCatalinaFind[] {
	0x41, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x4D, 0x89, 0xE9, 0xF6, 0x45, 0xD0, 0x01, 0x0F, 0x84, 0xA0, 0x00, 0x00, 0x00, 0x4D,
	0x89, 0xCD,
};
// Tahoe (26.7.2 25G332, read from the Dell's cache, all four in .03): SCGetInstructionSet is byte for byte 15.8.1's at
// the same page offset (the "isa" entry), the small-prim and walk-fence code is 15.8.1's at other page offsets, and the
// DCC flag test is cmp byte [rbp - 0x2C], 0; je <no DCC> (the same set-up follows): make the je a jmp. Each pattern
// occurs once in that cache.
static const uint8_t dccTahoeFind[] {
	0x8B, 0x45, 0xC0, 0x48, 0x8B, 0x00, 0x4C, 0x8B, 0x45, 0xA0, 0x4C, 0x8B, 0x75, 0x88, 0x80, 0x7D, 0xD4, 0x00, 0x0F, 0x84,
	0x67, 0x01, 0x00, 0x00,
};
static const uint8_t isaReplace[] {0x1E}, smallPrimReplace[] {0x00}, walkFenceReplace[] {0xA8}, dccReplace[] {0x90, 0xE9},
	dccVenturaReplace[] {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00}, isaCatalinaReplace[] {0x0E};
struct PagePatch {
	const char *name;
	const char *arg;  // boot-arg that turns it off (on, for an opt-in patch)
	bool optIn;
	uint32_t pageOffset;
	const uint8_t *find;
	uint32_t size;
	uint32_t at;
	const uint8_t *replace;
	uint32_t replaceSize;
	bool enabled;
	uint32_t hits;
};
static PagePatch pagePatches[] {
	{"isa", "-crnoscpatch", false, 0xD78, scPatchFind, sizeof(scPatchFind), 0x25, isaReplace, sizeof(isaReplace)},
	{"smallprim", "-crnosmallprim", false, 0x215, smallPrimFind, sizeof(smallPrimFind), 14, smallPrimReplace,
		sizeof(smallPrimReplace)},
	{"walkfence", "-crnowalkfence", false, 0x4AA, walkFenceFind, sizeof(walkFenceFind), 6, walkFenceReplace,
		sizeof(walkFenceReplace)},
	{"dcc", "-crdcc", false, 0xA2C, dccFind, sizeof(dccFind), 0x17, dccReplace, sizeof(dccReplace)},
	{"isa13", "-crnoscpatch", false, 0xD3A, scPatchFind, 60, 0x25, isaReplace, sizeof(isaReplace)},
	{"smallprim13", "-crnosmallprim", false, 0xA68, smallPrimFind, sizeof(smallPrimFind), 14, smallPrimReplace,
		sizeof(smallPrimReplace)},
	{"walkfence13", "-crnowalkfence", false, 0xCF0, walkFenceFind, sizeof(walkFenceFind), 6, walkFenceReplace,
		sizeof(walkFenceReplace)},
	{"dcc13", "-crdcc", false, 0x500, dccVenturaFind, sizeof(dccVenturaFind), 15, dccVenturaReplace,
		sizeof(dccVenturaReplace)},
	{"isa14", "-crnoscpatch", false, 0x4B6, scPatchFind, 60, 0x25, isaReplace, sizeof(isaReplace)},
	{"smallprim14", "-crnosmallprim", false, 0x65D, smallPrimFind, sizeof(smallPrimFind), 14, smallPrimReplace,
		sizeof(smallPrimReplace)},
	{"walkfence14", "-crnowalkfence", false, 0x8F3, walkFenceFind, sizeof(walkFenceFind), 6, walkFenceReplace,
		sizeof(walkFenceReplace)},
	{"dcc14", "-crdcc", false, 0x60A, dccSonomaFind, sizeof(dccSonomaFind), 0x17, dccReplace, sizeof(dccReplace)},
	{"isa12", "-crnoscpatch", false, 0xE1F, isaMontereyFind, sizeof(isaMontereyFind), 0x1A, isaReplace, sizeof(isaReplace)},
	{"smallprim12", "-crnosmallprim", false, 0xF6A, smallPrimMontereyFind, sizeof(smallPrimMontereyFind), 14,
		smallPrimReplace, sizeof(smallPrimReplace)},
	{"walkfence12", "-crnowalkfence", false, 0x1BB, walkFenceMontereyFind, sizeof(walkFenceMontereyFind), 6,
		walkFenceReplace, sizeof(walkFenceReplace)},
	{"dcc12", "-crdcc", false, 0x50E, dccMontereyFind, sizeof(dccMontereyFind), 8, dccReplace, sizeof(dccReplace)},
	{"isa11", "-crnoscpatch", false, 0x4AD, isaBigSurFind, sizeof(isaBigSurFind), 0x1F, isaReplace, sizeof(isaReplace)},
	{"smallprim11", "-crnosmallprim", false, 0x93F, smallPrimBigSurFind, sizeof(smallPrimBigSurFind), 14,
		smallPrimReplace, sizeof(smallPrimReplace)},
	{"walkfence11", "-crnowalkfence", false, 0xB8F, walkFenceMontereyFind, sizeof(walkFenceMontereyFind), 6,
		walkFenceReplace, sizeof(walkFenceReplace)},
	{"dcc11", "-crdcc", false, 0xF4F, dccBigSurFind, sizeof(dccBigSurFind), 4, dccReplace, sizeof(dccReplace)},
	{"isa10", "-crnoscpatch", false, 0x758, isaCatalinaFind, sizeof(isaCatalinaFind), 4, isaCatalinaReplace,
		sizeof(isaCatalinaReplace)},
	{"smallprim10", "-crnosmallprim", false, 0x21A, smallPrimCatalinaFind, sizeof(smallPrimCatalinaFind), 14,
		smallPrimReplace, sizeof(smallPrimReplace)},
	{"walkfence10", "-crnowalkfence", false, 0x472, walkFenceCatalinaFind, sizeof(walkFenceCatalinaFind), 2,
		walkFenceReplace, sizeof(walkFenceReplace)},
	{"dcc10", "-crdcc", false, 0xAD9, dccCatalinaFind, sizeof(dccCatalinaFind), 13, dccReplace, sizeof(dccReplace)},
	{"smallprim26", "-crnosmallprim", false, 0x3BF, smallPrimFind, sizeof(smallPrimFind), 14, smallPrimReplace,
		sizeof(smallPrimReplace)},
	{"walkfence26", "-crnowalkfence", false, 0x654, walkFenceFind, sizeof(walkFenceFind), 6, walkFenceReplace,
		sizeof(walkFenceReplace)},
	{"dcc26", "-crdcc", false, 0x31E, dccTahoeFind, sizeof(dccTahoeFind), 18, dccReplace, sizeof(dccReplace)},
};
static mach_vm_address_t orgCsValidatePage, orgCsValidateRange;
static bool scPatchHooked;
static uint32_t scPatchCalls;

// Video decode (-crvideo, docs/VIDEO.md stage 2): AppleGVA picks the hardware decoder from the platform's board-id, and
// for iMac19,1 it expects the Intel iGPU's, so the AMD decoder never got a session (no UVD firmware load or start ran).
// As WhateverGreen's unfairgva=4 does (acidanthera, kern_unfair.cpp): rename the property AppleGVA reads, "board-id"
// next to "hw.model" in the dyld shared cache, to "hwgva-id", and set hwgva-id on the device tree root to iMacPro1,1's
// board-id (an AMD-only Mac). The rest of the system keeps the real SMBIOS. The string is in two places in each cache
// (15.8.1, 14.x, 13.7.8), never across a page. Only pages of dyld cache files are searched (vnode path checked once).
static bool gvaPatchOn;
static uint32_t gvaPatchHits;
// What wrapConfigureDevice did (bits): 1 flags forced, 2 IOGVAHEVCDecode published, 4 HEVC encode withdrawn, 8 renamed,
// 0x10 HEVC decode withdrawn again (no -cruvdhevc).
static uint32_t gvaHevcState;

static bool isSharedCacheVnode(void *vnode) {
	static struct {
		void *vnode;
		bool cache;
	} seen[64];
	static uint32_t next;
	for (auto &e : seen)
		if (e.vnode == vnode)
			return e.cache;
	char path[160];
	int len = sizeof(path);
	bool cache = vn_getpath(static_cast<vnode_t>(vnode), path, &len) == 0 && strstr(path, "dyld_shared_cache_x86_64");
	auto &slot = seen[next++ % arrsize(seen)];
	slot.vnode = vnode;
	slot.cache = cache;
	return cache;
}

static void gvaPatchPage(uint8_t *page) {
	static const char find[] = "board-id\0hw.model";  // 17 bytes with the NUL in the middle
	for (uint32_t i = 0; i + sizeof(find) - 1 <= 0x1000;) {
		auto *hit = static_cast<uint8_t *>(memchr(page + i, 'b', 0x1000 - i));
		if (!hit)
			return;
		uint32_t at = static_cast<uint32_t>(hit - page);
		if (at + sizeof(find) - 1 <= 0x1000 && !memcmp(hit, find, sizeof(find) - 1)) {
			memcpy(hit, "hwgva", 5);
			__atomic_fetch_add(&gvaPatchHits, 1, __ATOMIC_RELAXED);
		}
		i = at + 1;
	}
}

static void patchValidatedPage(void *vnode, uint8_t *page) {
	if (gvaPatchOn && isSharedCacheVnode(vnode))
		gvaPatchPage(page);
	for (auto &patch : pagePatches) {
		if (!patch.enabled)
			continue;
		auto *at = page + patch.pageOffset;
		if (at[0] == patch.find[0] && !memcmp(at, patch.find, patch.size)) {
			memcpy(at + patch.at, patch.replace, patch.replaceSize);
			__atomic_fetch_add(&patch.hits, 1, __ATOMIC_RELAXED);
		}
	}
}

static void wrapCsValidatePage(void *vnode, void *pager, uint64_t offset, const void *data, int *validated,
	int *tainted, int *nx) {
	FunctionCast(wrapCsValidatePage, orgCsValidatePage)(vnode, pager, offset, data, validated, tainted, nx);
	__atomic_fetch_add(&scPatchCalls, 1, __ATOMIC_RELAXED);
	if (!data || !validated || !*validated || (offset & 0xFFF))
		return;
	patchValidatedPage(vnode, const_cast<uint8_t *>(static_cast<const uint8_t *>(data)));
}

// Catalina has no cs_validate_page: pages are validated one at a time through cs_validate_range (xnu-6153), which
// returns whether the range validated. libSC and the Metal driver are separate files there, mapped from disk.
static int wrapCsValidateRange(void *vnode, void *pager, uint64_t offset, const void *data, size_t size,
	unsigned *tainted) {
	int validated = FunctionCast(wrapCsValidateRange, orgCsValidateRange)(vnode, pager, offset, data, size, tainted);
	__atomic_fetch_add(&scPatchCalls, 1, __ATOMIC_RELAXED);
	if (validated && data && size == 0x1000 && !(offset & 0xFFF))
		patchValidatedPage(vnode, const_cast<uint8_t *>(static_cast<const uint8_t *>(data)));
	return validated;
}

// nvram32/33/35 (Catalina): "thread_invoke: preemption_level 1" from IOWorkLoop::threadMain about 85 s after each wake.
// A work loop went to sleep with preemption still disabled: something it ran, or an interrupt it took, left a spinlock
// held or a _disable_preemption unmatched, and the kernel only notices at the loop's next block, with nothing of the
// culprit left on the stack. These wrappers compare the CPU's preemption level (cpu_data, gs-relative; the offset is
// read from _disable_preemption) around each interrupt and timer event source, each run of a work loop's sources and
// the IH interrupt handler, and panic naming whichever leaked. -crnopreemptcheck turns them off.
static int32_t preemptionLevelOff = -1;
static mach_vm_address_t orgIesCheckForWork, orgTimerCheckForWork, orgRunEventSources, orgMessageClient;
static mach_vm_address_t kextLookupByAddress, kextIdentifier;  // OSKext::lookupKextWithAddress, getIdentifierCString

static inline int32_t preemptionLevel() {
	int32_t level;
	asm volatile("movl %%gs:(%1), %0" : "=r"(level) : "r"(static_cast<uint64_t>(preemptionLevelOff)));
	return level;
}

struct EventSourcePeek : IOEventSource {
	static OSObject *ownerOf(IOEventSource *source) { return static_cast<EventSourcePeek *>(source)->owner; }
	static void *actionOf(IOEventSource *source) {
		return reinterpret_cast<void *>(static_cast<EventSourcePeek *>(source)->action);
	}
	static IOEventSource *nextOf(IOEventSource *source) { return static_cast<EventSourcePeek *>(source)->getNext(); }
};

struct WorkLoopPeek : IOWorkLoop {
	static IOEventSource *chainOf(IOWorkLoop *loop) { return static_cast<WorkLoopPeek *>(loop)->eventChain; }
};

static const char *className(const OSObject *object) {
	return object ? object->getMetaClass()->getClassName() : "none";
}

[[noreturn]] static void preemptionLeak(const char *where, IOEventSource *source, int32_t before, int32_t after) {
	OSObject *owner = source ? EventSourcePeek::ownerOf(source) : nullptr;
	panic("CarrizoRed: preemption level %d -> %d across %s: %s, owner %s, action %p", before, after, where,
		className(source), className(owner), source ? EventSourcePeek::actionOf(source) : nullptr);
}

static bool wrapIesCheckForWork(IOEventSource *that) {
	int32_t before = preemptionLevel();
	bool more = FunctionCast(wrapIesCheckForWork, orgIesCheckForWork)(that);
	if (int32_t after = preemptionLevel(); after != before)
		preemptionLeak("IOInterruptEventSource::checkForWork", that, before, after);
	return more;
}

static bool wrapTimerCheckForWork(IOEventSource *that) {
	int32_t before = preemptionLevel();
	bool more = FunctionCast(wrapTimerCheckForWork, orgTimerCheckForWork)(that);
	if (int32_t after = preemptionLevel(); after != before)
		preemptionLeak("IOTimerEventSource::checkForWork", that, before, after);
	return more;
}

// Anything else on the loop (command queues, power management queues, other source classes): name the loop's sources.
static bool wrapRunEventSources(IOWorkLoop *that) {
	int32_t before = preemptionLevel();
	bool ok = FunctionCast(wrapRunEventSources, orgRunEventSources)(that);
	if (int32_t after = preemptionLevel(); after != before) {
		char list[480] = "";
		auto *source = WorkLoopPeek::chainOf(that);
		for (int n = 0; source && n < 12; n++, source = EventSourcePeek::nextOf(source)) {
			size_t len = strlen(list);
			snprintf(list + len, sizeof(list) - len, "%s%s/%s %p", n ? ", " : "", className(source),
				className(EventSourcePeek::ownerOf(source)), EventSourcePeek::actionOf(source));
		}
		panic("CarrizoRed: preemption level %d -> %d across IOWorkLoop::runEventSources, sources: %s", before, after,
			list);
	}
	return ok;
}

// nvram42 (Catalina, after a wake): the leak was in IOPMrootDomain's work loop, which runs every driver's power
// management: its request, work, completion and power-state queues. Those deliver sleep/wake messages (will sleep,
// capability change, will power on) to registered interest handlers through IOService::messageClient, i.e. other
// drivers' code. Check around each delivery and name the client: for an interest notifier (_IOServiceInterestNotifier,
// whose messageClient calls [notifier + 0x20] with target [notifier + 0x28] on Catalina), its handler and the kext that
// holds it.
static const char *kextOf(mach_vm_address_t address) {
	if (!kextLookupByAddress || !kextIdentifier || !address)
		return "?";
	auto *kext = reinterpret_cast<OSObject *(*)(vm_address_t)>(kextLookupByAddress)(static_cast<vm_address_t>(address));
	if (!kext)
		return "none";
	const char *id = reinterpret_cast<const char *(*)(OSObject *)>(kextIdentifier)(kext);
	kext->release();  // the identifier string is the kext's own, and loaded kexts stay
	return id ? id : "?";
}

static IOReturn wrapMessageClient(IOService *that, UInt32 type, OSObject *client, void *arg, vm_size_t size) {
	int32_t before = preemptionLevel();
	IOReturn ret = FunctionCast(wrapMessageClient, orgMessageClient)(that, type, client, arg, size);
	if (int32_t after = preemptionLevel(); after != before) {
		const char *cls = className(client);
		mach_vm_address_t handler = 0, target = 0;
		if (client && !strcmp(cls, "_IOServiceInterestNotifier")) {
			handler = *reinterpret_cast<mach_vm_address_t *>(reinterpret_cast<uint8_t *>(client) + 0x20);
			target = *reinterpret_cast<mach_vm_address_t *>(reinterpret_cast<uint8_t *>(client) + 0x28);
		}
		panic("CarrizoRed: preemption level %d -> %d across IOService::messageClient: message %x from %s to %s, "
			"handler %p in %s, target %p", before, after, type, className(that), cls, reinterpret_cast<void *>(handler),
			kextOf(handler), reinterpret_cast<void *>(target));
	}
	return ret;
}

static void hookPreemptionCheck(KernelPatcher &patcher) {
	if (checkKernelArgument("-crnopreemptcheck"))
		return;
	// _disable_preemption: push rbp; mov rbp, rsp; inc dword gs:[disp32] (65 FF 04 25 disp32).
	auto *code = reinterpret_cast<const uint8_t *>(patcher.solveSymbol(KernelPatcher::KernelID, "__disable_preemption"));
	patcher.clearError();
	for (int i = 0; code && i < 24 && preemptionLevelOff < 0; i++)
		if (code[i] == 0x65 && code[i + 1] == 0xFF && code[i + 2] == 0x04 && code[i + 3] == 0x25)
			preemptionLevelOff = *reinterpret_cast<const int32_t *>(code + i + 4);
	if (preemptionLevelOff < 0) {
		SYSLOG("cr", "preemption check: level offset not found");
		return;
	}
	KernelPatcher::RouteRequest requests[] {
		{"__ZN22IOInterruptEventSource12checkForWorkEv", wrapIesCheckForWork, orgIesCheckForWork},
		{"__ZN18IOTimerEventSource12checkForWorkEv", wrapTimerCheckForWork, orgTimerCheckForWork},
		{"__ZN10IOWorkLoop15runEventSourcesEv", wrapRunEventSources, orgRunEventSources},
		{"__ZN9IOService13messageClientEjP8OSObjectPvm", wrapMessageClient, orgMessageClient},
	};
	kextLookupByAddress = patcher.solveSymbol(KernelPatcher::KernelID, "__ZN6OSKext21lookupKextWithAddressEm");
	patcher.clearError();
	kextIdentifier = patcher.solveSymbol(KernelPatcher::KernelID, "__ZN6OSKext20getIdentifierCStringEv");
	patcher.clearError();
	for (auto &request : requests)
		if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &request, 1)) {
			SYSLOG("cr", "preemption check: failed to hook %s (error %d)", request.symbol, patcher.getError());
			patcher.clearError();
		}
	DBGLOG("cr", "preemption check on, level at gs:%x", preemptionLevelOff);
}

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

static uint64_t *physmapBaseVar, *physmapMaxVar;  // see physmapPtr
// The kernel's message buffer (bsd/sys/msgbuf.h), for mirrorKernelLog.
struct KernelMsgBuf {
	int magic, size, bufx, bufr;
	char *bufc;
};
static KernelMsgBuf **msgbufVar;

void CRDriver::processPatcher(KernelPatcher &patcher) {
	physmapBaseVar = reinterpret_cast<uint64_t *>(patcher.solveSymbol(KernelPatcher::KernelID, "_physmap_base"));
	physmapMaxVar = reinterpret_cast<uint64_t *>(patcher.solveSymbol(KernelPatcher::KernelID, "_physmap_max"));
	msgbufVar = reinterpret_cast<KernelMsgBuf **>(patcher.solveSymbol(KernelPatcher::KernelID, "_msgbufp"));
	patcher.clearError();
	bool anyPagePatch = false;
	for (auto &patch : pagePatches) {
		patch.enabled = checkKernelArgument(patch.arg) == patch.optIn && patch.pageOffset + patch.size <= 0x1000 &&
			patch.at + patch.replaceSize <= patch.size;
		anyPagePatch |= patch.enabled;
	}
	// Video decode routing: with -crvideo only (the engines are powered only then), -crnogva leaves AppleGVA alone.
	gvaPatchOn = checkKernelArgument("-crvideo") && !checkKernelArgument("-crnogva");
	if (gvaPatchOn) {
		if (auto *root = IORegistryEntry::fromPath("/", gIODTPlane)) {
			root->setProperty("hwgva-id", const_cast<char *>("Mac-7BA5B2D9E42DDD94"),
				static_cast<uint32_t>(sizeof("Mac-7BA5B2D9E42DDD94")));
			root->release();
		} else {
			gvaPatchOn = false;
		}
		anyPagePatch |= gvaPatchOn;
	}
	if (anyPagePatch) {
		// Long route, chained with any other wrapper of the same function (RestrictEvents has one; on Catalina, Lilu's user
		// patcher wraps cs_validate_range).
		bool catalina = getKernelVersion() < KernelVersion::BigSur;
		auto &org = catalina ? orgCsValidateRange : orgCsValidatePage;
		KernelPatcher::RouteRequest request = catalina ?
			KernelPatcher::RouteRequest {"_cs_validate_range", wrapCsValidateRange, orgCsValidateRange} :
			KernelPatcher::RouteRequest {"_cs_validate_page", wrapCsValidatePage, orgCsValidatePage};
		scPatchHooked = patcher.routeMultipleLong(KernelPatcher::KernelID, &request, 1) && org && org != EINVAL;
		if (!scPatchHooked) {
			SYSLOG("cr", "failed to hook %s for the Metal patches (error %d)", request.symbol, patcher.getError());
			org = 0;
			patcher.clearError();
		}
	}
	hookPreemptionCheck(patcher);

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
		for (auto &patch : pagePatches)
			patch.enabled = false;
		disableKextCallbacks();
		return;
	}

	gpuBus = iGPU->getBusNumber();
	gpuDevice = iGPU->getDeviceNumber();
	// The libSC patch selects AMD's Stoney back end (SCGfx81Info); libSC gives Carrizo (CZ revisions 1-0x20) the
	// generic GFX 8.0 one, which is right for it (32 LDS banks), so only Stoney gets it.
	if (family != ChipFamily::Stoney)
		pagePatches[0].enabled = false;
	pipeLimit = displayPipes(family);
	pipeGuard = checkKernelArgument("-crpipeguard");
	regLogIntsOff = checkKernelArgument("-crreglogintsoff");
	regLogAll = checkKernelArgument("-crreglogall");
	lbFixOff = checkKernelArgument("-crnolbfix");
	bpcCapOff = checkKernelArgument("-crnobpccap");
	restoreOn = checkKernelArgument("-crrestore");
	assrOff = checkKernelArgument("-crnoassr");
	parseStopAt();
	// Opt-in (-cracceldefer): nvram127-130 showed the start order wasn't the cause (the controller was always ready
	// first), and re-registering the iGPU to re-match the accelerator risks a second instance being started and stopped
	// (nvram132: WindowServer used an accelerator whose VM manager had been torn down by acceleratorWillStop).
	accelDeferOff = !checkKernelArgument("-cracceldefer") || checkKernelArgument("-crnoaccel");

	logPCIInfo();
	if (checkKernelArgument("-crmacgpu"))
		setMacGpuProperties();
	setGpuModel();
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

static mach_vm_address_t orgConfigureDevice;
static bool wrapConfigureDevice(IOService *that, IOPCIDevice *pci);
static mach_vm_address_t orgVideoContextStart, videoGetHWInfoOrg;
static uint32_t wrapVideoContextStart(void *that);
static uint32_t wrapGetHWInfo(void *that, uint32_t *info);
static uint32_t videoSpoofId, videoHWInfoCalls;
static mach_vm_address_t orgUvdContextInit;
static bool wrapUvdContextInit(void *that, void *accel, void *videoContext, uint32_t *createInfo);
static mach_vm_address_t orgUvdStart;
static bool wrapUvdStart(void *that);
static void uvdLiveTick();
static uint64_t ibMcToPhys(uint64_t addr, bool system);
static bool physRangeOk(uint64_t phys, uint32_t bytes);
static mach_vm_address_t orgAddPlistPropertiesToPCIDevice;
static uint64_t wrapAddPlistPropertiesToPCIDevice(IOService *that, const char *name);
// -cruvdpanic: a report 20 s after the first UVD session check, unless UVD's firmware reported by then (nvram13:
// WindowServer hung after it, the disk log was lost and the panic was macOS's userspace watchdog, without the report).
// 0 until then.
static uint64_t uvdPanicMs;
static struct {
	uint32_t calls, skipped, ret, status, vcpu, reset, lmi, tile;
	uint64_t ms;
} uvdStartInfo;
static bool uvdReadyForSession();
static mach_vm_address_t orgVceContextInit;
static bool wrapVceContextInit(void *that, void *accel, void *videoContext, uint32_t *createInfo);

void CRDriver::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size) {
	for (size_t i = 0; i < arrsize(kexts); i++) {
		if (kexts[i].loadIndex != index)
			continue;
		SYSLOG("cr", "%s loaded: slide 0x%llX size 0x%lX", kexts[i].id, static_cast<uint64_t>(slide),
			static_cast<unsigned long>(size));
		// The acceleration kexts load late (IOKit matching), if at all: note it, for the black box.
		if (i == KextRadeonX4000 || i == KextRadeonX4000HWServices || i == KextRadeonX4000HWLibs) {
			const char *name = i == KextRadeonX4000 ? "X4000" : i == KextRadeonX4000HWServices ? "HWServices" : "HWLibs";
			size_t k = i == KextRadeonX4000 ? 0 : i == KextRadeonX4000HWServices ? 1 : 2;
			// Reloadable kexts (see kexts[]) also come back here for an image we've already patched (nvram116: patched
			// twice, invalid opcode in AMDSWScheduler::advance). Only a new load address is a new copy.
			if (accelKextLoads[k] && accelKextSlide[k] == slide) {
				accelKextRepeats[k]++;
				return;
			}
			traceNote("kext:%s ", name);
			accelKextLoads[k]++;
			accelKextSlide[k] = slide;
		}
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
// System sleep and wake in the disk log: sleep (S3) powers the GPU off, and what CarrizoRed set up once per boot (SMU
// microcode load, the FB window move, SCLK DPM, RB+, video power) has to be redone on wake. First, see the sequence.
static void diskLogEvent(const char *fmt, ...);
static uint32_t systemSleeps, systemWakes;
// Set on each wake: the watcher writes a full black-box report to the disk log 30 s later (the GPU's state once macOS
// has brought it back, which the periodic reports miss when the machine dies first).
static uint64_t wakeReportAtMs;
static volatile uint32_t watcherBeat;  // watcher ticks, for the wake watchdog

// Wake watchdog (21:34 sleep: the log ends at "sleep params" and the machine never came back, nothing in NVRAM). 45 s
// after "system will power on", panic with the report if the wake never completed or the watcher (which writes the
// disk log) stopped ticking: the panic log is the one thing that survives the forced restart. It runs on a thread
// call, not the watcher, which may itself be stuck on a disk that didn't wake. -crnowakepanic disables it.
static thread_call_t wakeWatchdogCall;
static uint32_t wakeWakesAtStart, wakeBeatAtStart;

static void wakeWatchdog(thread_call_param_t, thread_call_param_t) {
	CRDriver::wakeWatchdogFired("45 s after kIOMessageSystemWillPowerOn", systemWakes == wakeWakesAtStart,
		watcherBeat - wakeBeatAtStart);
}

// nvram8: black screen on wake and no panic, so the wake never reached kIOMessageSystemWillPowerOn (platform, ACPI or
// PCI restore) or the kernel never ran again. This one is armed at "will sleep": thread call deadlines count awake
// time only, so it fires about 150 s of awake time later, ~30 s before sleep (wifip2pd's timeout) plus ~2 min after
// the CPU resumes. Disarmed by the wake completing (or the sleep being cancelled).
// A dark wake (no display; nvram37, Catalina: woken by EHC1 twice, then maintenance sleep) never sends
// kIOMessageSystemHasPoweredOn either, but the kernel runs: the watcher ticking (every 500 ms) for at least half of
// those 150 s means the machine is alive, so stand down instead of panicking.
static thread_call_t sleepWatchdogCall;
static uint32_t sleepWakesAtStart, sleepBeatAtStart;

static void sleepWatchdog(thread_call_param_t, thread_call_param_t) {
	if (systemWakes != sleepWakesAtStart)
		return;
	uint32_t beats = watcherBeat - sleepBeatAtStart;
	if (beats >= 150) {
		diskLogEvent("sleep watchdog: no full wake 150 s after sleep, but the watcher ticked %u times: dark wake, not "
			"stuck", beats);
		return;
	}
	CRDriver::wakeWatchdogFired("150 s of awake time after kIOMessageSystemWillSleep", true, beats);
}

static IOReturn sleepWakeHandler(void *, void *, UInt32 messageType, IOService *, void *, vm_size_t) {
	switch (messageType) {
		case kIOMessageSystemWillSleep:
			systemSleeps++;
			diskLogEvent("system will sleep (%u)", systemSleeps);
			if (sleepWatchdogCall) {
				sleepWakesAtStart = systemWakes;
				sleepBeatAtStart = watcherBeat;
				uint64_t deadline = 0;
				clock_interval_to_deadline(150, kSecondScale, &deadline);
				thread_call_enter_delayed(sleepWatchdogCall, deadline);
			}
			break;
		case kIOMessageSystemWillPowerOn:
			diskLogEvent("system will power on");
			__atomic_store_n(&edpWakePowerOn, true, __ATOMIC_RELAXED);
			__atomic_store_n(&wakeAuxWatch, true, __ATOMIC_RELAXED);
			__atomic_store_n(&wakeRestorePending, true, __ATOMIC_RELAXED);
			if (wakeWatchdogCall) {
				wakeWakesAtStart = systemWakes;
				wakeBeatAtStart = watcherBeat;
				uint64_t deadline = 0;
				clock_interval_to_deadline(45, kSecondScale, &deadline);
				thread_call_enter_delayed(wakeWatchdogCall, deadline);
			}
			break;
		case kIOMessageSystemHasPoweredOn:
			systemWakes++;
			diskLogEvent("system has powered on (%u)", systemWakes);
			__atomic_store_n(&wakeReportAtMs, uptimeMs() + 30000, __ATOMIC_RELAXED);
			break;
		default:
			break;
	}
	return kIOReturnSuccess;
}

// Kernel log lines about sleep and wake into the disk log (watcher thread). The 15:46 and 21:04 sleeps: "system will
// sleep", then 30 s of normal ticks and the log ends; 30 s is the power manager's acknowledgement timeout, and its
// "PM response took ... / notification timeout" lines name the driver or app that held sleep up.
static void mirrorKernelLog() {
	static int seen = -1;
	static char line[200];
	static uint32_t lineLen, mirrored;
	auto *mb = msgbufVar ? *msgbufVar : nullptr;
	if (!mb || mb->magic != 0x63061 || !mb->bufc || mb->size <= 0 || mirrored >= 3000)
		return;
	int size = mb->size, x = mb->bufx;
	if (x < 0 || x >= size)
		return;
	if (seen < 0 || seen >= size) {
		seen = x;
		return;
	}
	static const char *const keys[] {"PM ", "PMRD", "leep", "ake", "imeout", "ibernat", "IOPCI", "Radeon", "AMD", "ATI",
		"GPU", "isplay", "AGDC", "AGDP", "ccelerator", "Failed to"};
	// IOGraphicsAccelerator2::start reports why it gave up through os_log, which reaches the message buffer but not
	// the verbose screen (Catalina installer: accel.start = 0 after hw.init = 1, and the installer's /var/log is lost).
	// Echo those lines to the screen too, a few dozen at most.
	static uint32_t echoed;
	for (int n = 0; seen != x && n < size; n++, seen = (seen + 1) % size) {
		char c = mb->bufc[seen];
		if (c != '\n' && lineLen + 1 < sizeof(line)) {
			line[lineLen++] = c;
			continue;
		}
		if (c != '\n')
			continue;  // overlong: cut, the rest dropped until the newline
		line[lineLen] = '\0';
		bool want = !strnstr(line, "CarrizoRed", lineLen) && !strnstr(line, "CapabilityChange[", lineLen) &&
			!strnstr(line, "display_change_handler", lineLen);
		bool hit = false;
		for (size_t k = 0; want && !hit && k < arrsize(keys); k++)
			hit = strnstr(line, keys[k], lineLen) != nullptr;
		if (want && hit) {
			diskLogEvent("klog: %s", line);
			mirrored++;
			if (echoed < 40 && (strnstr(line, "ccelerator", lineLen) || strnstr(line, "Failed to", lineLen))) {
				echoed++;
				IOLog("CarrizoRed: klog: %s\n", line);
			}
		}
		lineLen = 0;
	}
}

void CRDriver::wakeWatchdogFired(const char *when, bool notPoweredOn, uint32_t beats) {
	if (!notPoweredOn && beats >= 20)
		return;
	char reason[200];
	snprintf(reason, sizeof(reason), "CarrizoRed: wake stuck %s: %s, watcher ticks %u (sleep %u, wake %u).", when,
		notPoweredOn ? "system never powered on" : "the watcher stopped", beats, systemSleeps, systemWakes);
	panic("%s\n%s", reason, callback->composeReport(0, ""));
}

void CRDriver::startWatcher() {
	if (!checkKernelArgument("-crnowakepanic")) {
		wakeWatchdogCall = thread_call_allocate(wakeWatchdog, nullptr);
		sleepWatchdogCall = thread_call_allocate(sleepWatchdog, nullptr);
	}
	auto *root = IOService::getPMRootDomain();
	if (!root || !root->registerInterest(gIOGeneralInterest, sleepWakeHandler, this))
		SYSLOG("cr", "failed to register for sleep/wake");
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
static bool desktopRunning();

static void writeBreadcrumb(uint32_t t) {
	static IORegistryEntry *options;
	static uint32_t lastHead = ~0U, lastIh;
	uint32_t &writes = breadcrumbWrites;
	static char text[1000];
	uint32_t head = __atomic_load_n(&traceHead, __ATOMIC_RELAXED);
	uint32_t ih = __atomic_load_n(&ihHandlerCalls, __ATOMIC_RELAXED);
	// Each write is a forced NVRAM sync (a flash write): at most every 2 s, and none once Metal is drawing (nvram179:
	// trace events never stop then, and all 160 writes were gone by t=109 s; the GPU stall panic covers later hangs).
	static uint32_t lastT;
	bool ihOnly = head == lastHead && (ih == lastIh || t % 5);
	if (writes >= 160 || ihOnly || desktopRunning() || (writes && t - lastT < 2))
		return;
	lastT = t;
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

static void startInterpFix();  // defined with the black box below
// The interpolation fix's submit hook and its shared state (see wrapCommitIb).
static IOLock *interpLock;
static bool interpFixOff, interpHooked, interpBump, interpSubmit;
static mach_vm_address_t orgCommitIb, orgCommitIbFiji;
static uint64_t wrapCommitIb(void *channel, uint8_t *info);
static uint64_t wrapCommitIbFiji(void *channel, uint8_t *info);
static mach_vm_address_t orgCommitSdma;
static uint64_t wrapCommitSdma(void *channel, uint8_t *info);

// GPU clock: the SMU's current and target SCLK DPM level (ixTARGET_AND_CURRENT_PROFILE_INDEX, SMC 0xD0200014: current
// [20:16], target [25:21]; Linux smu8_read_sensor) and the GFX rail's voltage ID (ixSMUSVI_GFX_CURRENTVID 0xD8230048
// [31:24], mV = (6200 - 25 * vid) / 4), sampled every 500 ms. Read through MP0PUB_IND_INDEX_13/DATA_13 (0x19A/0x19B):
// Apple's PowerPlay uses window 0 (0x180/0x181), and a read between its index write and data access would send its
// access to the wrong address. No SMU messages (the mailbox is Apple's). Levels map to MHz through the VBIOS clock
// table.
static uint32_t sclkCurrent[32], sclkTarget[32], sclkSamples, sclkLast, gfxVidMin = ~0U, gfxVidMax;
static uint32_t sclkPrevLevel, sclkChanges, sclkMaxSeen;
// SMU mailbox use by anyone else (Apple's HWLibs): the message register holding something we didn't send. A message
// sent while ours is in flight would make the SMU act on the wrong message or argument.
static uint32_t smuLastMsg, smuForeignMsgs, smuForeignIds[8], smuForeignNum;

static void watchSmuMailbox() {
	if (!gpuMmio || !smuLastMsg)
		return;
	uint32_t msg = gpuMmio[0x1C0] & 0xFFFF;
	if (msg == smuLastMsg)
		return;
	smuForeignMsgs++;
	smuLastMsg = msg;
	bool known = false;
	for (uint32_t i = 0; i < smuForeignNum && !known; i++)
		known = smuForeignIds[i] == msg;
	if (!known && smuForeignNum < arrsize(smuForeignIds))
		smuForeignIds[smuForeignNum++] = msg;
}
static void smuReadClockTable();  // defined with the SMU loader
static void smuEnableSclkDpm();
static bool sclkDpmRunning();

static uint32_t smuWindowRead(uint32_t address) {
	gpuMmio[0x19A] = address;
	return gpuMmio[0x19B];
}

static void sampleGpuClock() {
	if (!gpuMmio || !smuLoadOk())
		return;
	uint32_t profile = smuWindowRead(0xD0200014);
	if (profile == 0xFFFFFFFF)
		return;
	sclkLast = profile;
	uint32_t level = (profile >> 16) & 0x1F;
	if (sclkSamples && level != sclkPrevLevel)
		sclkChanges++;
	sclkPrevLevel = level;
	if (level > sclkMaxSeen)
		sclkMaxSeen = level;
	sclkCurrent[level]++;
	sclkTarget[(profile >> 21) & 0x1F]++;
	uint32_t vid = smuWindowRead(0xD8230048) >> 24;
	if (vid < gfxVidMin)
		gfxVidMin = vid;
	if (vid > gfxVidMax)
		gfxVidMax = vid;
	sclkSamples++;
}

// Disk log (-crnodisklog disables). A freeze stops the CPU, and kernel NVRAM writes outside a panic never reach the
// flash (nvram183-203), so a hard freeze left nothing. Instead, every second a line of GPU, SMU and DPM state is
// appended to /private/var/log/CarrizoRed-<boot time>.log and forced to disk (open, write, fsync, close each time: a
// freeze loses at most the last second, and nothing stays open at shutdown). Events (every SMU message and its answer,
// DPM steps) are queued in memory from boot and written with the next line; the full black-box report is appended at
// t=120 s and every 300 s. One file per boot: several test boots, then copy them all off in one go.
static char diskLogPath[96];
static bool diskLogOff, diskLogStarted;
static uint32_t diskLogWrites, diskLogErrors, diskLogLastError;
static char diskLogEvents[65536];
// The last events, kept for the report's early block (the panic log keeps the start of the report).
static char diskLogRecent[24][120];
static uint32_t diskLogRecentNext;
static uint32_t diskLogEventLen, diskLogEventsDropped;
static volatile uint32_t diskLogEventLock;

static void diskLogEvent(const char *fmt, ...) {
	// Bounded: an event from a context that can't wait for the holder (preempted on this CPU) is dropped instead.
	for (uint32_t spins = 0; __atomic_exchange_n(&diskLogEventLock, 1, __ATOMIC_ACQUIRE); spins++) {
		if (spins > 200000) {
			diskLogEventsDropped++;
			return;
		}
		__builtin_ia32_pause();
	}
	uint32_t room = sizeof(diskLogEvents) - diskLogEventLen;
	if (room > 160) {
		int n = snprintf(diskLogEvents + diskLogEventLen, room, "[%llu ms] ", uptimeMs());
		if (n > 0 && static_cast<uint32_t>(n) < room) {
			diskLogEventLen += static_cast<uint32_t>(n);
			va_list va;
			va_start(va, fmt);
			n = vsnprintf(diskLogEvents + diskLogEventLen, sizeof(diskLogEvents) - diskLogEventLen - 1, fmt, va);
			va_end(va);
			if (n > 0)
				diskLogEventLen += static_cast<uint32_t>(n) < sizeof(diskLogEvents) - diskLogEventLen - 1 ?
					static_cast<uint32_t>(n) : static_cast<uint32_t>(sizeof(diskLogEvents) - diskLogEventLen - 2);
			diskLogEvents[diskLogEventLen++] = '\n';
			diskLogEvents[diskLogEventLen] = '\0';
			// The line just queued, from its timestamp: into the recent ring.
			char *line = diskLogEvents + diskLogEventLen - 1;
			while (line > diskLogEvents && line[-1] != '\n')
				line--;
			auto &slot = diskLogRecent[diskLogRecentNext++ % arrsize(diskLogRecent)];
			size_t n = static_cast<size_t>(diskLogEvents + diskLogEventLen - 1 - line);
			if (n >= sizeof(slot))
				n = sizeof(slot) - 1;
			memcpy(slot, line, n);
			slot[n] = '\0';
		}
	} else {
		diskLogEventsDropped++;
	}
	__atomic_store_n(&diskLogEventLock, 0, __ATOMIC_RELEASE);
}

// Append text to the log and force it to disk. False until the data volume is writable.
static bool diskLogAppend(const char *text, size_t len) {
	if (diskLogOff || !len)
		return false;
	if (!diskLogPath[0]) {
		clock_sec_t secs = 0;
		clock_usec_t usecs = 0;
		clock_get_calendar_microtime(&secs, &usecs);
		snprintf(diskLogPath, sizeof(diskLogPath), "/private/var/log/CarrizoRed-%lu.log", static_cast<unsigned long>(secs));
	}
	vfs_context_t ctx = vfs_context_create(nullptr);
	if (!ctx)
		return false;
	vnode_t vp = nullptr;
	int err = vnode_open(diskLogPath, O_CREAT | O_APPEND | FWRITE | O_NOFOLLOW, 0644, 0, &vp, ctx);
	if (!err) {
		int resid = 0;
		err = vn_rdwr(UIO_WRITE, vp, const_cast<char *>(text), static_cast<int>(len), 0, UIO_SYSSPACE,
			IO_APPEND | IO_SYNC | IO_NOAUTH, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
		if (!err)
			err = VNOP_FSYNC(vp, MNT_WAIT, ctx);
		vnode_close(vp, FWRITE, ctx);
	}
	vfs_context_rele(ctx);
	if (err) {
		diskLogErrors++;
		diskLogLastError = static_cast<uint32_t>(err);
		return false;
	}
	diskLogWrites++;
	return true;
}

static uint32_t hookCallCount();
static uint32_t interpFixedCount();
static bool videoPowered();
static void censusLog();
static const char *sclkDpmState();

// Every second from the watcher. stallTicks: the GPU stall detector's count (0.5 s ticks with the GFX ring stuck).
#ifndef CR_COMMIT
#define CR_COMMIT unknown
#endif
#ifdef DEBUG
#define CR_BUILD_KIND "debug"
#else
#define CR_BUILD_KIND "release"
#endif

// CPU clock (AMD family 15h: the Excavator cores of Stoney, Bristol Ridge and Carrizo). Sonoma and later feel slow on
// these two-core APUs while the GPU idles at its lowest level; whether the cores ever leave their base clock for boost
// isn't known (neither macOS nor the usual AMD CPU kexts manage family 15h P-states). Once, into the disk log: the
// P-state table (MSRC001_0064..6B: CpuFid [5:0], CpuDid [8:6], frequency 100 MHz * (CpuFid + 0x10) >> CpuDid, enabled
// bit 63), the P-state limit (MSRC001_0061: CurPstateLimit [2:0], PstateMaxVal [6:4]) and boost (HWCR MSRC001_0015
// bit 25, CpbDis). Every status line: the core's current P-state (MSRC001_0063) and its clock while busy: APERF (MSR
// 0xE8, counting at the core clock) over a 200 us spin with interrupts off. BKDG for AMD Family 15h Models 70h-7Fh.
static struct {
	bool checked, pstates, aperf;
} cpuInfo;

static uint64_t cpuRdmsr(uint32_t msr) {
	uint32_t lo, hi;
	asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return static_cast<uint64_t>(hi) << 32 | lo;
}

static void cpuCheck() {
	if (cpuInfo.checked)
		return;
	cpuInfo.checked = true;
	uint32_t a = 0, b, c, d;
	asm volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
	bool amd = b == 0x68747541 && d == 0x69746E65 && c == 0x444D4163;  // "AuthenticAMD"
	uint32_t maxLeaf = a;
	a = 1;
	c = 0;
	asm volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
	uint32_t family = ((a >> 8) & 0xF) + (((a >> 8) & 0xF) == 0xF ? ((a >> 20) & 0xFF) : 0);
	cpuInfo.pstates = amd && family == 0x15;
	if (maxLeaf >= 6) {
		a = 6;
		c = 0;
		asm volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
		cpuInfo.aperf = cpuInfo.pstates && (c & 1);  // APERF/MPERF
	}
	if (!cpuInfo.pstates) {
		diskLogEvent("cpu: not AMD family 15h (family %x), no P-state readings", family);
		return;
	}
	char table[200];
	size_t len = 0;
	for (uint32_t p = 0; p < 8; p++) {
		uint64_t def = cpuRdmsr(0xC0010064 + p);
		if (!(def >> 63))
			continue;
		uint32_t mhz = (100 * ((def & 0x3F) + 0x10)) >> ((def >> 6) & 7);
		int n = snprintf(table + len, sizeof(table) - len, " P%u %u MHz (vid %x)", p, mhz,
			static_cast<uint32_t>((def >> 9) & 0x7F));
		if (n > 0 && static_cast<size_t>(n) < sizeof(table) - len)
			len += static_cast<size_t>(n);
	}
	table[len] = '\0';
	uint64_t limit = cpuRdmsr(0xC0010061), hwcr = cpuRdmsr(0xC0010015);
	diskLogEvent("cpu: P-states%s; limit P%u, lowest P%u; boost %s (HWCR %llx); busy-clock sampling %s", table,
		static_cast<uint32_t>(limit & 7), static_cast<uint32_t>((limit >> 4) & 7), (hwcr >> 25) & 1 ? "disabled" : "enabled",
		hwcr, cpuInfo.aperf ? "on" : "off (no APERF)");
}

// The calling core's P-state and its clock in MHz while kept busy for 200 us (0 if it can't be measured).
static uint32_t cpuBusyMhz(uint32_t &pstate) {
	pstate = cpuInfo.pstates ? static_cast<uint32_t>(cpuRdmsr(0xC0010063) & 7) : 0xFF;
	if (!cpuInfo.aperf)
		return 0;
	uint64_t flags;
	asm volatile("pushfq; popq %0; cli" : "=r"(flags)::"memory");
	uint64_t start = mach_absolute_time(), aperf0 = cpuRdmsr(0xE8), now = start, ns = 0;
	while (ns < 200000) {
		now = mach_absolute_time();
		absolutetime_to_nanoseconds(now - start, &ns);
	}
	uint64_t aperf1 = cpuRdmsr(0xE8);
	if (flags & 0x200)
		asm volatile("sti" ::: "memory");
	return ns ? static_cast<uint32_t>((aperf1 - aperf0) * 1000 / ns) : 0;
}

static void diskLogTick(uint32_t t, uint32_t stallTicks) {
	static uint64_t lastMs;
	if (diskLogOff)
		return;
	uint64_t ms = uptimeMs();
	if (ms - lastMs < 1000)
		return;
	lastMs = ms;
	static char line[1024];
	size_t len = 0;
	auto add = [&](const char *fmt, ...) {
		if (len + 1 >= sizeof(line))
			return;
		va_list va;
		va_start(va, fmt);
		int n = vsnprintf(line + len, sizeof(line) - len, fmt, va);
		va_end(va);
		if (n > 0)
			len += static_cast<size_t>(n) < sizeof(line) - len ? static_cast<size_t>(n) : sizeof(line) - len - 1;
	};
	if (!diskLogStarted) {
		add("CarrizoRed disk log; build %s (%s, " xStringify(MODULE_VERSION) "); boot-args: %s\ncolumns: t ms | sclk cur/tgt "
			"level (profile) | vid gfx/nb | temp C | GRBM_STATUS GRBM_STATUS2 CP_STAT | gfx rptr/wptr | sdma0 rptr/wptr | "
			"ih rptr/wptr | flips perf/irq | submits fixed | smu msg/resp/arg | stall | dpm | ih calls/false alarms/broken "
			"[| uvd ... vce ...] | cpu P-state, clock while busy\n", xStringify(CR_COMMIT), CR_BUILD_KIND,
			PE_boot_args());
		cpuCheck();
	}
	auto *mmio = gpuMmio;
	if (mmio && smuLoadOk()) {
		auto smc = [mmio](uint32_t address) {
			mmio[0x19A] = address;
			return mmio[0x19B];
		};
		uint32_t profile = smc(0xD0200014);
		uint32_t vidGfx = smc(0xD8230048) >> 24, vidNb = smc(0xD8230044) >> 24;
		uint32_t thm = smc(0xD8200CA4), raw = thm >> 21;
		int temp = static_cast<int>(raw / 8) - ((thm >> 19) & 1 ? 49 : 0);
		add("%llu | L%u/L%u (%x) | %u/%u | %d | %08x %08x %08x | %x/%x | %x/%x | %x/%x | %u/%u | %u %u | %x/%x/%x | %u | %s | %u/%u/%u\n",
			ms, (profile >> 16) & 0x1F, (profile >> 21) & 0x1F, profile, vidGfx, vidNb, temp, mmio[0x2004], mmio[0x2002],
			mmio[0x21A0], mmio[0x21C0], mmio[0x3045], mmio[0x3483], mmio[0x3484], mmio[IhReg::RB_RPTR], mmio[IhReg::RB_WPTR],
			__atomic_load_n(&flipPerforms, __ATOMIC_RELAXED), __atomic_load_n(&flipIrqs, __ATOMIC_RELAXED),
			hookCallCount(), interpFixedCount(), mmio[0x1C0], mmio[0x1D0], mmio[0x1E0], stallTicks, sclkDpmState(),
			__atomic_load_n(&ihHandlerCalls, __ATOMIC_RELAXED), ihStormFalseAlarms, ihStormBroken ? 1U : 0U);
		if (videoPowered()) {
			// UVD: STATUS, VCPU_CNTL, SOFT_RESET, LMI_STATUS, ring rptr/wptr; VCE: STATUS, VCPU_CNTL, SOFT_RESET, ring.
			if (len)
				len--;  // drop the newline, append to the same line
			add(" | uvd %x %x %x %x %x/%x vce %x %x %x %x/%x\n", mmio[0x3DAF], mmio[0x3D98], mmio[0x3DA0], mmio[0x3D67],
				mmio[0x3DA4], mmio[0x3DA5], mmio[0x8001], mmio[0x8005], mmio[0x8048], mmio[0x8063], mmio[0x8064]);
		}
	} else {
		add("%llu | t=%u, GPU not up yet\n", ms, t);
	}
	if (cpuInfo.pstates) {
		uint32_t pstate = 0, mhz = cpuBusyMhz(pstate);
		if (len)
			len--;  // drop the newline, append to the same line
		add(" | cpu P%u %u MHz\n", pstate, mhz);
	}
	// Queued events go in first (they happened before this sample).
	static char events[sizeof(diskLogEvents)];
	uint32_t eventLen = 0;
	while (__atomic_exchange_n(&diskLogEventLock, 1, __ATOMIC_ACQUIRE))
		__builtin_ia32_pause();
	if (diskLogEventLen) {
		memcpy(events, diskLogEvents, diskLogEventLen);
		eventLen = diskLogEventLen;
	}
	__atomic_store_n(&diskLogEventLock, 0, __ATOMIC_RELEASE);
	if (eventLen && !diskLogAppend(events, eventLen))
		return;  // not writable yet: keep the events queued
	if (eventLen) {
		while (__atomic_exchange_n(&diskLogEventLock, 1, __ATOMIC_ACQUIRE))
			__builtin_ia32_pause();
		memmove(diskLogEvents, diskLogEvents + eventLen, diskLogEventLen - eventLen);
		diskLogEventLen -= eventLen;
		diskLogEvents[diskLogEventLen] = '\0';
		__atomic_store_n(&diskLogEventLock, 0, __ATOMIC_RELEASE);
	}
	if (diskLogAppend(line, len))
		diskLogStarted = true;
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
	bool autoPanicOff = checkKernelArgument("-crnoautopanic") || checkKernelArgument("-crnosmuload") ||
		checkKernelArgument("-crnodirectucode") ||
		(self->family != ChipFamily::Stoney && self->family != ChipFamily::Carrizo);
	uint32_t panicAt = 0, stallRptr = 0, stallTicks = 0;
	bool stallPanicOff = checkKernelArgument("-crnostallpanic");
	diskLogOff = checkKernelArgument("-crnodisklog");
	if (!PE_parse_boot_argn("crpanicat", &panicAt, sizeof(panicAt)))
		panicAt = 0;
	uint32_t printed = 0;
	last[0] = '\0';
	// Tick every 500 ms (a sleep late in a session needs the log too: no end any more).
	for (uint32_t tick = 0, t = 0;; tick++, t = tick / 2) {
		watcherBeat++;
		if (!gpuMmio)
			self->probeMMIO();  // start-up couldn't map the registers: keep trying
		mirrorKernelLog();
		// After each wake: the power manager's sleep/wake properties (wake reason, sleep failure), into the disk log.
		{
			static uint32_t wakesLogged = 0xFFFFFFFF;  // also once at boot: the last sleep's failure record
			if (systemWakes != wakesLogged) {
				wakesLogged = systemWakes;
				auto *root = IOService::getPMRootDomain();
				auto *props = root ? root->dictionaryWithProperties() : nullptr;
				auto *keys = props ? OSCollectionIterator::withCollection(props) : nullptr;
				while (keys) {
					auto *key = OSDynamicCast(OSSymbol, keys->getNextObject());
					if (!key)
						break;
					const char *name = key->getCStringNoCopy();
					if (!strnstr(const_cast<char *>(name), "Wake", strlen(name)) &&
						!strnstr(const_cast<char *>(name), "Sleep", strlen(name)) &&
						!strnstr(const_cast<char *>(name), "Failure", strlen(name)))
						continue;
					auto *value = props->getObject(key);
					if (auto *str = OSDynamicCast(OSString, value))
						diskLogEvent("pm %s = %s", name, str->getCStringNoCopy());
					else if (auto *num = OSDynamicCast(OSNumber, value))
						diskLogEvent("pm %s = %llx", name, num->unsigned64BitValue());
					else if (auto *b = OSDynamicCast(OSBoolean, value))
						diskLogEvent("pm %s = %s", name, b->isTrue() ? "true" : "false");
					else if (auto *data = OSDynamicCast(OSData, value)) {
						auto *bytes = static_cast<const uint8_t *>(data->getBytesNoCopy());
						char hex[100] {};
						for (uint32_t q = 0; q < data->getLength() && q < 48; q++)
							snprintf(hex + q * 2, 3, "%02x", bytes[q]);
						diskLogEvent("pm %s = data[%u] %s", name, data->getLength(), hex);
					}
				}
				OSSafeReleaseNULL(keys);
				OSSafeReleaseNULL(props);
			}
		}
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
			// Once the desktop runs, interrupts never stop: log them every 30 s, or when they storm.
			uint32_t calls = __atomic_load_n(&ihHandlerCalls, __ATOMIC_RELAXED);
			if (calls != lastCalls && (!desktopRunning() || tick % 60 == 0 || calls - lastCalls > 5000))
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
		uvdLiveTick();
		if (uvdPanicMs && uptimeMs() >= uvdPanicMs && !(uvdStartInfo.status & 2))
			self->panicReport(t, now, "CarrizoRed: deliberate panic (-cruvdpanic), 20 s after the first UVD session.");
		kickStalledFlip();
		// GPU stopped: the GFX ring's read pointer (CP_RB0_RPTR) stuck behind its write pointer (CP_RB0_WPTR) for 10 s.
		// The screen freezes with the kernel still running and nothing in NVRAM, so panic with the report instead.
		// -crnostallpanic disables it.
		if (gpuMmio && smuLoadOk() && !stallPanicOff) {
			uint32_t rptr = gpuMmio[0x21C0], wptr = gpuMmio[0x3045];
			if (rptr != wptr && rptr == stallRptr) {
				if (++stallTicks == 20)
					self->panicReport(t, now, "CarrizoRed: the GPU stopped executing commands (GFX ring read pointer stuck "
						"for 10 s with work queued).");
			} else {
				stallTicks = 0;
				stallRptr = rptr;
			}
		}
		self->setGpuModel();
		checkGpuAlive(gpuMmio);
		// Not while SCLK DPM runs: nvram183/188 froze with it on (both without a report, so the CPU stopped too), and
		// these SMU register reads every 500 ms are the one thing we do that Linux doesn't (it reads them on request).
		bool smu8 = self->family == ChipFamily::Stoney || self->family == ChipFamily::Carrizo;
		if (smu8 && !sclkDpmRunning())
			sampleGpuClock();
		if (smu8)
			watchSmuMailbox();
		{
			static bool stormLogged;
			if (ihStormBroken && !stormLogged) {
				stormLogged = true;
				diskLogEvent("IH storm breaker tripped at %llu ms after %u calls (false alarms before: %u): interrupts off",
					ihSnapshot.ms, ihSnapshot.calls, ihStormFalseAlarms);
			}
		}
		censusLog();
		// After 30 minutes the status line every 10 s; events still go out within the tick.
		if (t <= 1800 || tick % 20 == 0 || __atomic_load_n(&diskLogEventLen, __ATOMIC_RELAXED))
			diskLogTick(t, stallTicks);
		// The full black-box report in the log at 2 minutes and every 5 minutes after.
		if (!diskLogOff && diskLogStarted && t >= 120 && (t - 120) % 300 == 0 && tick % 2 == 0) {
			const char *report = self->composeReport(t, now);
			diskLogEvent("black-box report at t=%u s follows", t);
			diskLogTick(t, stallTicks);
			diskLogAppend(report, strlen(report));
			diskLogAppend("\n", 1);
		}
		if (uint64_t at = __atomic_load_n(&wakeReportAtMs, __ATOMIC_RELAXED); at && uptimeMs() >= at && !diskLogOff &&
			diskLogStarted) {
			__atomic_store_n(&wakeReportAtMs, 0, __ATOMIC_RELAXED);
			bool diag = __atomic_load_n(&wakeDisplayDiag, __ATOMIC_RELAXED);
			const char *report = self->composeReport(t, diag ? "" : now);  // the display sections, not the tree
			__atomic_store_n(&wakeDisplayDiag, false, __ATOMIC_RELAXED);
			diskLogEvent("black-box report %s after wake %u follows", diag ? "1 s after a failed DPCD transfer" : "30 s",
				systemWakes);
			diskLogTick(t, stallTicks);
			diskLogAppend(report, strlen(report));
			diskLogAppend("\n", 1);
		}
		if (smu8 && t >= 60)
			smuReadClockTable();
		if (smu8 && t >= 90)
			smuEnableSclkDpm();
		if (smuLoadOk())
			fixRbPlus(gpuMmio, self->family == ChipFamily::Stoney);
		if (smuLoadOk() && self->family == ChipFamily::Stoney)
			startInterpFix();
		if (accelDeferred && controllerReady) {
			accelDeferred = false;
			accelRematches++;
			self->iGPU->registerService();
			traceNote("accel.rematch@%us ", t);
		}
		// A failed accelerator start (nvram109/113/123: HWLibs ran unpatched, so no SMU load and no GUI) leaves no
		// report on an EFI without crpanicat. If the accelerator kexts loaded but the microcode still isn't loaded at
		// 120 s, panic with the report. -crnoautopanic disables it.
		if (t >= 120 && accelKextLoads[2] && !smuLoadOk() && !autoPanicOff)
			self->panicReport(t, now, gpuDeadMs ?
				"CarrizoRed: the GPU stopped responding (registers read 0xFFFFFFFF), so the accelerator couldn't start. "
				"Not a driver bug: power the machine off fully (hold power, wait 10 s) before the next boot." :
				"CarrizoRed: the accelerator didn't start (no SMU microcode load by 120 s).");
		if (__atomic_load_n(&smuStoppedAt, __ATOMIC_RELAXED))
			self->panicReport(t, now, "CarrizoRed: SMU load stopped by crsmustep.");
		if (auto *stage = __atomic_load_n(&stoppedStage, __ATOMIC_RELAXED)) {
			static char reason[160];
			snprintf(reason, sizeof(reason), "CarrizoRed: stopped on entering %s (#%u, crstopat).", stage, stopAtCount);
			self->panicReport(t, now, reason);
		}

		// Watchdog: a traced call that never returns (a hard hang with this CPU still alive) becomes a panic that
		// names it and stays on screen (debug=0x100). -crnowatchdog disables this.
		// nvram42: fb.open waited on the accelerator's power-up; name every open stage and add the full black-box
		// report. 60 s: CAIL's register waits time out after 3 s each (AtiAppleMcilServices::waitFor counts in ms), and
		// the RLC start-up alone has several (nvram48), so a slow but progressing power-up must not trip it.
		if (watchdog) {
			uint64_t ms = uptimeMs();
			bool stuck = false;
			for (size_t i = 0; i < MaxTraceStages && !stuck; i++)
				stuck = __atomic_load_n(&stageOpen[i], __ATOMIC_RELAXED) && ms - stageEnterMs[i] >
					(getKernelVersion() == KernelVersion::Catalina ? WatchdogMsCatalina : WatchdogMs);
			if (stuck) {
				static char reason[512];
				size_t len = static_cast<size_t>(snprintf(reason, sizeof(reason), "%sCarrizoRed watchdog: open stages:",
					gpuDeadMs ? "GPU NOT RESPONDING (power off fully). " : ""));
				for (size_t i = 0; i < MaxTraceStages && len < sizeof(reason); i++)
					if (__atomic_load_n(&stageOpen[i], __ATOMIC_RELAXED))
						len += static_cast<size_t>(snprintf(reason + len, sizeof(reason) - len, " %s(%llu ms)",
							stageNames[i], ms - stageEnterMs[i]));
				self->panicReport(t, now, reason);
			}
		}

		// The registry walk below: every second during start-up, every 10 s once Metal is drawing.
		if (tick % 2 || (desktopRunning() && tick % 20)) {
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
		// What IOGraphicsAccelerator2::start (the family's start, called first) asks of the AMD accelerator; any of them
		// failing ends it (0 = failed for the pointer results).
		{"__ZN37AMDRadeonX4000_AMDGraphicsAccelerator15configureDeviceEP11IOPCIDevice", "accel.configureDevice", InAccel, true, true, 0},
		{"__ZN37AMDRadeonX4000_AMDGraphicsAccelerator23createStatisticsManagerEv", "accel.createStatisticsManager", InAccel, true, true, 0},
		{"__ZN37AMDRadeonX4000_AMDGraphicsAccelerator15createHWHandlerEv", "accel.createHWHandler", InAccel, true, true, 0},
		{"__ZN37AMDRadeonX4000_AMDGraphicsAccelerator17createHWInterfaceEP11IOPCIDevice", "accel.createHWInterface", InAccel, true, true, 0},
		{"__ZN37AMDRadeonX4000_AMDGraphicsAccelerator19populateAccelConfigEP13IOAccelConfig", "accel.populateAccelConfig", InAccel, true, true, 0},
		{"__ZN37AMDRadeonX4000_AMDGraphicsAccelerator17newDisplayMachineEv", "accel.newDisplayMachine", InAccel, true, true, 0},
		{"__ZN37AMDRadeonX4000_AMDGraphicsAccelerator13newStatisticsEv", "accel.newStatistics", InAccel, true, true, 0},
		{"__ZN37AMDRadeonX4000_AMDGraphicsAccelerator19createAccelChannelsEv", "accel.createAccelChannels", InAccel, true, true, 0},
		// Catalina installer: createAccelChannels never returned. Per hardware channel it creates an AMDAccelChannel
		// (argument: low byte of the hardware channel, then its index) whose init adds a software scheduler queue.
		{"__ZN30AMDRadeonX4000_AMDAccelChannel4initEP22IOGraphicsAccelerator2P28AMDRadeonX4000_IAMDHWChanneljPKc11SS_PRIORITY",
			"chan.init", InAccel, true, true, 0, 4},
		{"__ZN29AMDRadeonX4000_AMDSWScheduler10addSWQueueEm11SS_PRIORITYPVjmPKc", "sched.addSWQueue", InAccel, false, false, 0, 1},
		{"__ZN29AMDRadeonX4000_AMDSWScheduler10addSWQueueEPVjmPKc", "sched.addSWQueue2", InAccel, false, false, 0},
		{"__ZN26AMDRadeonX4000_AMDHardware4initEP11IOPCIDeviceP28AMDRadeonX4000_IAMDHWHandlerRjP16_GART_PARAMETERSP14_FB_PARAMETERS", "hw.init", InAccel, true, true, 0},
		{"__ZN26AMDRadeonX4000_AMDHardware8initCAILEv", "hw.initCAIL", InAccel, true, true, 0},
		{"__ZN26AMDRadeonX4000_AMDHardware9setupCAILEv", "hw.setupCAIL", InAccel, true, true, 0},
		{"__ZN26AMDRadeonX4000_AMDHardware11powerUpCAILEv", "hw.powerUpCAIL", InAccel, true, true, 0},
		{"__ZN32AMDRadeonX4000_AMDBaffinHardware7powerUpEv", "hw.powerUp", InAccel, true, true, 0},
		// Video engines (docs/VIDEO.md; powered only with -crvideo): firmware load and start, results in the summary.
		{"__ZN29AMDRadeonX4000_AMDUVDHWEngine9loadUVDFWEv", "uvd.loadFW", InAccel, true, false, 0},
		{"__ZN29AMDRadeonX4000_AMDUVDHWEngine13startRBEngineEv", "uvd.startRB", InAccel, true, false, 0},
		{"__ZN29AMDRadeonX4000_AMDVCEHWEngine6loadFWEv", "vce.loadFW", InAccel, true, false, 0},
		{"__ZN29AMDRadeonX4000_AMDVCEHWEngine5startEv", "vce.start", InAccel, true, false, 0},
		// Video sessions (stage 2): whether user space (AppleGVA / the AMD video driver) opens one at all, and how far in.
		{"__ZN32AMDRadeonX4000_AMDSMLVIInterface9createUVDEv", "vid.createUVD", InAccel, true, false, 0},
		{"__ZN32AMDRadeonX4000_AMDSMLVIInterface9createVCEEv", "vid.createVCE", InAccel, true, false, 0},
		// What a session does after contextStart (the first stage-2 log stopped there): hardware info, the content-
		// protection connection, power-management commands, the SI layer's context, the UVD capability, and its end.
		{"__ZN35AMDRadeonX4000_AMDAccelVideoContext18createCPConnectionEv", "vid.createCP", InAccel, true, false, 0},
		{"__ZN35AMDRadeonX4000_AMDAccelVideoContext13sendPMCommandEPvS0_yPy", "vid.sendPM", InAccel, true, false, 0},
		{"__ZN35AMDRadeonX4000_AMDAccelVideoContext11submitCPMsgEP18sAMDCPContextMsgInP19sAMDCPContextMsgOutyPy", "vid.submitCP",
			InAccel, true, false, 0},
		{"__ZN32AMDRadeonX4000_AMDSIVideoContext10newContextEP23sVideoContextCreateInfoP23sVideoContextReturnInfoyPy",
			"vid.siNewContext", InAccel, true, false, 0},
		{"__ZN33AMDRadeonX4000_AMDAccelUVDContext17requestCapabilityEP23sVideoContextReturnInfo", "vid.uvdCapability",
			InAccel, true, false, 0},
		{"__ZN35AMDRadeonX4000_AMDAccelVideoContext11contextStopEv", "vid.contextStop", InAccel, true, false, 0},
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
		// User space and the GPU (nvram88/89: accelerator fully up, but at 75 s nothing beyond Apple's start-up packets
		// had reached the ring, and the panel was backlit black). Who opens the accelerator, and does any command
		// buffer get submitted? Counted ("uc:" line in the report), not summarised.
		{"__ZN29AMDRadeonX4000_AMDAccelShared4initEP22IOGraphicsAccelerator2P4task", "uc.shared", InAccel, false, false, 0},
		{"__ZN39AMDRadeonX4000_AMDAccelSharedUserClient5startEP9IOService", "uc.sharedUC", InAccel, false, false, 0},
		{"__ZN35AMDRadeonX4000_AMDAccelCommandQueue18initSubmitChannelsEv", "uc.cmdq", InAccel, false, false, 0},
		{"__ZN35AMDRadeonX4000_AMDAccelCommandQueue14submitCommandsEv", "uc.submitCommands", InAccel, false, false, 0},
		{"__ZN35AMDRadeonX4000_AMDAccelCommandQueue20processCommandBufferEjj", "uc.process", InAccel, false, false, 0},
		{"__ZN29AMDRadeonX4000_AMDSWScheduler19submitCommandBufferEmP30AMD_SUBMIT_COMMAND_BUFFER_INFOP16AMD_BARRIER_INFO",
			"uc.swsubmit", InAccel, false, false, 0},
		// The PM4/DMA channels' own versions tail-call this through the vtable (RIP-relative, not routable).
		{"__ZN27AMDRadeonX4000_AMDHWChannel19submitCommandBufferEP30AMD_SUBMIT_COMMAND_BUFFER_INFO", "uc.hwsubmit",
			InAccel, false, false, 0},
		// Flips (nvram101: SDMA ran WindowServer's work, but the display still scanned out the boot framebuffer at VRAM
		// offset 0, all black): the accelerator's display-pipe transactions and the controller's surface programming.
		{"__ZN34AMDRadeonX4000_AMDAccelDisplayPipe16beginTransactionEP12IOAccelEvent", "uc.dpBegin", InAccel, false,
			false, 0},
		{"__ZN34AMDRadeonX4000_AMDAccelDisplayPipe19validateTransactionEP30IOAccelDisplayPipeTransaction2", "uc.dpValidate",
			InAccel, false, false, 0},
		{"__ZN34AMDRadeonX4000_AMDAccelDisplayPipe17submitTransactionEP30IOAccelDisplayPipeTransaction2", "uc.dpSubmit",
			InAccel, false, false, 0},
		{"__ZN34AMDRadeonX4000_AMDAccelDisplayPipe18executeTransactionEP30IOAccelDisplayPipeTransaction2b", "uc.dpExecute",
			InAccel, false, false, 0},
		{"__ZN34AMDRadeonX4000_AMDAccelDisplayPipe18performTransactionEP30IOAccelDisplayPipeTransaction2", "uc.dpPerform",
			InAccel, false, false, 0},
		{"__ZN34AMDRadeonX4000_AMDAccelDisplayPipe26enableTransactionInterruptEv", "uc.dpIrqOn", InAccel, false, false, 0},
		{"__ZN34AMDRadeonX4000_AMDAccelDisplayPipe27processTransactionInterruptEP8OSObjectPv", "uc.dpIrq", InAccel, false,
			false, 0},
		{"__ZN17AMD9500Controller19hwSetDisplaySurfaceEP11CRTC_PARAMSjj", "uc.setSurface", InController, false, false, 2},
		{"__ZN17AMD9500Controller26waitForSurfaceFlipCompleteEhb", "uc.flipWait", InController, false, false, 0},
		// nvram106: 323 transactions but 322 transaction interrupts, then nothing: the display pipe's interrupt is the
		// framebuffer's (IOFramebuffer::setInterruptState on its sources); last argument = the state (1 on, 0 off).
		{"__ZN14AMDFramebuffer17setInterruptStateEPvj", "uc.fbIrqState", InFramebuffer, false, false, 2},
		{"__ZN14AMDFramebuffer24registerForInterruptTypeEjPFvP8OSObjectPvES1_S2_PS2_", "uc.fbIrqRegister",
			InFramebuffer, false, false, 1},
		// nvram108: the 876th flip was performed at 150.2 s and its 'txni' completion (a software interrupt the
		// framebuffer raises when it finishes a display-pipe transaction) never came, while vblank/v-update interrupts
		// kept arriving. AMDFramebuffer's transaction queue and its vblank/v-update/vsync handlers.
		{"__ZN14AMDFramebuffer28queueDisplayPipeTransactionsEy", "uc.fbQueue", InFramebuffer, false, false, 1},
		{"__ZN14AMDFramebuffer30dequeueDisplayPipeTransactionsEy", "uc.fbDequeue", InFramebuffer, false, false, 1},
		{"__ZN14AMDFramebuffer28handleDisplayPipeTransactionEP21_AMD_PIPE_TRANSACTION", "uc.fbHandle", InFramebuffer, false, false, 0},
		{"__ZN14AMDFramebuffer29processDisplayPipeTransactionEjb", "uc.fbProcess", InFramebuffer, false, false, 3},
		{"__ZN14AMDFramebuffer35displayPipeTransactionVBlankHandlerEP8OSObjectPv", "uc.fbVBlank", InFramebuffer, false, false, 0},
		{"__ZN14AMDFramebuffer36displayPipeTransactionVUpdateHandlerEP8OSObjectPv", "uc.fbVUpdate", InFramebuffer, false, false, 0},
		{"__ZN14AMDFramebuffer34displayPipeTransactionVSyncHandlerEP8OSObjectPv", "uc.fbVSync", InFramebuffer, false, false, 0},
		{"__ZN14AMDFramebuffer27scheduledTransactionHandlerEP8OSObjectP22IOInterruptEventSourcei", "uc.fbScheduled", InFramebuffer, false, false, 0},
		{"__ZN14AMDFramebuffer26deferredTransactionHandlerEP8OSObjectP22IOInterruptEventSourcei", "uc.fbDeferred", InFramebuffer, false, false, 0},
		{"__ZN14AMDFramebuffer36finishDeferredDisplayPipeTransactionEv", "uc.fbFinishDeferred", InFramebuffer, false, false, 0},
		// nvram112: ~96 s in, the graphics engine was re-initialised twice (Apple's hang recovery) and came back with
		// CE/PFP microcode wiped; flips stopped. The hang check and reset path (counted; the first resetHardware also
		// snapshots the GPU, see snapshotHang). Not AMDSWScheduler::signalTimeout: it's 6 bytes long, and the route's
		// 14-byte jump overwrote the start of AMDSWScheduler::advance (nvram115/116: crash there at ~58 s).
		{"__ZN28AMDRadeonX4000_AMDVIHardware18checkASICHangStateEb", "uc.checkHang", InAccel, false, false, 1},
		{"__ZN26AMDRadeonX4000_AMDHardware13resetHardwareEjPj", "uc.resetHW", InAccel, false, false, 1},
		{"__ZN29AMDRadeonX4000_AMDVIPM4Engine5resetE18_eAMD_HW_RING_TYPEPj", "uc.pm4Reset", InAccel, false, false, 1},
		{"__ZN29AMDRadeonX4000_AMDVIPM4Engine14gfxEngineResetEv", "uc.gfxReset", InAccel, false, false, 0},
		{"__ZN29AMDRadeonX4000_AMDVIPM4Engine11cpSoftResetEPj", "uc.cpSoftReset", InAccel, false, false, 0},
		{"__ZN30AMDRadeonX4000_AMDAccelChannel22resetHardwareAndReplayEv", "uc.replay", InAccel, false, false, 0},
	};

	// Six integer/pointer arguments cover every traced function. Events go to the ring (see traceEvent); the watcher
	// prints them, and panics naming the function if a call hasn't returned after 10 s. "sync" stages also print
	// directly and pause, because an instant whole-machine hang leaves the watcher no chance.
	template <size_t N>
	uint64_t wrapStage(void *a, void *b, void *c, void *d, void *e, void *f) {
		static_assert(N < MaxTraceStages, "too many traced stages");
		stageNames[N] = stages[N].name;
		stageEnterMs[N] = uptimeMs();
		__atomic_fetch_add(&stageCalls[N], 1, __ATOMIC_RELAXED);
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
		stageLastArg[N] = arg;
		if (!strcmp(stages[N].name, "uc.dpPerform")) {
			flipPipe = a;
			flipPerformMs = uptimeMs();
			__atomic_fetch_add(&flipPerforms, 1, __ATOMIC_RELAXED);
		} else if (!strcmp(stages[N].name, "uc.dpIrq")) {
			__atomic_fetch_add(&flipIrqs, 1, __ATOMIC_RELAXED);
		}
		uint32_t calls = stageCalls[N];
		if (calls == 1 && !strcmp(stages[N].name, "uc.resetHW"))
			snapshotHang(arg);
		// After a reset the CP microcode is gone and only the SMU can reload it (see smuReloadCp). Not after a wake: the
		// full load has just run, and halting and reloading the CP on top of it stalled the CP (Catalina, both wakes).
		if (!strcmp(stages[N].name, "pm4.initializeMicroEngine")) {
			if (calls > 1 && !smuFreshLoad)
				smuReloadCp();
			else if (calls > 1)
				traceNote("smureload=skipped(fresh load) ");
			smuFreshLoad = false;
		}
		traceEvent(stages[N].name, false, arg);
		if (stages[N].summary)  // start-up stages (and their re-runs on wake) into the disk log
			diskLogEvent("> %s (call %u)", stages[N].name, calls);
		if (stopAtName[0] && !strcmp(stopAtName, stages[N].name) &&
			__atomic_add_fetch(&stopAtHits, 1, __ATOMIC_RELAXED) == stopAtCount) {
			__atomic_store_n(&stoppedStage, stages[N].name, __ATOMIC_RELAXED);
			for (;;)
				IOSleep(1000);
		}
		if (stages[N].sync) {
			IOLog("CarrizoRed: => %s\n", stages[N].name);
			IOSleep(30);
		}
		if (!strcmp(stages[N].name, "hw.setupCAIL")) {
			restoreVbiosImageForSetup();
			checkLiveHwlibs(a);
		}
		if (ppmGuardOff && !strcmp(stages[N].name, "initializePowerPlay") &&
			!*reinterpret_cast<void **>(static_cast<uint8_t *>(a) + ppmGuardOff)) {
			traceNote("initializePowerPlay=skipped(no ppm) ");
			traceEvent(stages[N].name, true, 0xE00002F0);
			__atomic_fetch_sub(&stageOpen[N], 1, __ATOMIC_RELAXED);
			return 0xE00002F0;
		}
		if (!controllerReady && !accelDeferOff &&
			(!strcmp(stages[N].name, "hws.probe") || !strcmp(stages[N].name, "accel.probe"))) {
			accelDeferred = true;
			accelDeferrals++;
			traceNote("%s=deferred ", stages[N].name);
			__atomic_fetch_sub(&stageOpen[N], 1, __ATOMIC_RELAXED);
			return 0;
		}
		auto ret = FunctionCast(wrapStage<N>, stages[N].org)(a, b, c, d, e, f);
		if (!strcmp(stages[N].name, "initializeProjectDependentResources"))
			controllerReady = true;
		traceEvent(stages[N].name, true, static_cast<uint32_t>(ret));
		// Mode 5: d points at a status buffer the call fills (DP training: per-lane status read from the DPCD).
		if (stages[N].arg == 5 && d)
			traceEvent("  status", true, *static_cast<uint32_t *>(d));
		__atomic_fetch_sub(&stageOpen[N], 1, __ATOMIC_RELAXED);
		if (stages[N].summary) {
			traceNote("%s=%x ", stages[N].name, static_cast<uint32_t>(ret));
			diskLogEvent("< %s = %x", stages[N].name, static_cast<uint32_t>(ret));
		}
		return ret;
	}

#define CR_WRAP(n) reinterpret_cast<mach_vm_address_t>(wrapStage<n>)
	void routeStages(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size, TraceKext kext) {
		static_assert(arrsize(stages) == 180, "one wrapper per stage");
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
			CR_WRAP(123), CR_WRAP(124), CR_WRAP(125), CR_WRAP(126), CR_WRAP(127), CR_WRAP(128), CR_WRAP(129),
			CR_WRAP(130), CR_WRAP(131), CR_WRAP(132), CR_WRAP(133), CR_WRAP(134), CR_WRAP(135), CR_WRAP(136), CR_WRAP(137),
			CR_WRAP(138), CR_WRAP(139), CR_WRAP(140), CR_WRAP(141), CR_WRAP(142), CR_WRAP(143), CR_WRAP(144),
			CR_WRAP(145), CR_WRAP(146), CR_WRAP(147), CR_WRAP(148), CR_WRAP(149), CR_WRAP(150), CR_WRAP(151), CR_WRAP(152),
			CR_WRAP(153), CR_WRAP(154), CR_WRAP(155), CR_WRAP(156), CR_WRAP(157), CR_WRAP(158), CR_WRAP(159), CR_WRAP(160),
			CR_WRAP(161), CR_WRAP(162), CR_WRAP(163), CR_WRAP(164), CR_WRAP(165), CR_WRAP(166), CR_WRAP(167), CR_WRAP(168),
			CR_WRAP(169), CR_WRAP(170), CR_WRAP(171), CR_WRAP(172), CR_WRAP(173), CR_WRAP(174), CR_WRAP(175), CR_WRAP(176),
			CR_WRAP(177), CR_WRAP(178), CR_WRAP(179),
		};
		for (size_t i = 0; i < arrsize(stages); i++) {
			if (stages[i].kext != kext)
				continue;
			if (!strcmp(stages[i].name, "initializePowerPlay") && getKernelVersion() == KernelVersion::Catalina) {
				// mov rdx, [rax+0x1A070] (the PowerPlay manager) at +0x7E; see ppmGuardOff.
				static const uint8_t load[] {0x48, 0x8B, 0x90, 0x70, 0xA0, 0x01, 0x00};
				auto func = patcher.solveSymbol(index, stages[i].symbol, slide, size);
				patcher.clearError();
				if (func && !memcmp(reinterpret_cast<const void *>(func + 0x7E), load, sizeof(load)))
					ppmGuardOff = 0x1A070;
				else
					traceNote("ppmguard:nomatch ");
			}
			KernelPatcher::RouteRequest request {stages[i].symbol, wrappers[i], stages[i].org};
			if (routeChecked(patcher, index, request, slide, size)) {
				stageNames[i] = stages[i].name;
			} else {
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
// AMDHardware's capability fields: shader engines, SH per SE, CUs per SH, minimum active CUs per SH (see the caps fix
// route). Monterey and Big Sur have them 8 bytes earlier; Catalina at 0x58, 0x5C, 0x80, 0x84.
static struct {
	uint32_t se, sh, cu, activeCu;
} capsOff {0x64, 0x6C, 0xA0, 0xA4};
// AMD_SUBMIT_COMMAND_BUFFER_INFO (the PM4 and sDMA commit hooks): +0 flags, +4 VMID, then the command buffer count and
// the entries, 0x28 bytes each (dwords at +0, GPU address at +0xC). Monterey and later: count at +0xC, entries from
// +0x44; Big Sur: count at +0x8, entries from +0x3C (both commitIndirectCommandBuffer versions read them there).
// Catalina: count at +0x8, entries from +0x2C, 0x20 bytes each with the GPU address at +4.
static uint32_t submitCountOff = 0xC, submitEntriesOff = 0x44, submitStride = 0x28, submitVaOff = 0xC;
// The vtable slot of the info object's state getter AMDHardware::startHWEngines calls (and wrapStartHWEngines): 0x178,
// Big Sur 0x170.
static uint32_t hwStateSlot = 0x178;
// AMDHardware's engine slots, 8 bytes per engine type (0 PM4, 1 sDMA0, 2 sDMA1): from +0x3B0, Catalina +0x340. The
// multimedia engines the power-up skips (UVD, VCE, SAMU) are at other types there too.
static uint32_t hwEngines = 0x3B0;
static uint32_t hwMmSlots[3] {0x3D8, 0x3E8, 0x400};
// Whether the GFX ring's layout (PM4 engine +0x50, ring +0x30 size, +0x40 CPU mapping, +0x48 GPU address) is known:
// Catalina's ring objects differ, so its ring reads (diagnostics, the fallback shader scan) are skipped.
static bool ringLayoutKnown = true;
// CAIL's flags word, the SMU data buffer's size and the micro-engine block's FB offset (see the direct microcode load):
// Catalina has all three 8 bytes earlier.
static uint32_t cailFlagsOff = 0x6F0, cailSmuSizeOff = 0xCD8, cailSmuBlockOff = 0xCE8;

// The layouts above for the running macOS (Catalina 10.15.7 19H2, Big Sur 11.7.11 20G1443, Monterey 12.7.6).
static void selectAccelLayouts() {
	auto version = getKernelVersion();
	if (version <= KernelVersion::Monterey)
		capsOff = {0x5C, 0x64, 0x98, 0x9C};
	if (version == KernelVersion::BigSur) {
		submitCountOff = 0x8;
		submitEntriesOff = 0x3C;
		hwStateSlot = 0x170;
	}
	if (version == KernelVersion::Catalina) {
		capsOff = {0x58, 0x5C, 0x80, 0x84};
		submitCountOff = 0x8;
		submitEntriesOff = 0x2C;
		submitStride = 0x20;
		submitVaOff = 0x4;
		hwStateSlot = 0x180;
		hwEngines = 0x340;
		hwMmSlots[0] = 0x358;
		hwMmSlots[1] = 0x368;
		hwMmSlots[2] = 0x380;
		ringLayoutKnown = false;
		cailFlagsOff = 0x6E8;
		cailSmuSizeOff = 0xCD0;
		cailSmuBlockOff = 0xCE0;
	}
}
static void *accelHardware;  // AMDBaffinHardware, from allocateHWEngines (for the black box's ring dump)

// The GPU at the first AMDHardware::resetHardware, before Apple's reset changes it: GRBM/SRBM status, CP status and
// stall state, the PM4 and SDMA ring pointers, CP_ME_CNTL, RLC, VM fault status/address; and the PM4 ring before RPTR.
static const uint16_t hangRegs[] {0x2004, 0x2002, 0x2005, 0x0394, 0x0393, 0x21A0, 0x219C, 0x219D, 0x219E, 0x219F,
	0x21A1, 0x21A2, 0x21C0, 0x3045, 0x21B6, 0xEC00, 0xEC04, 0x340D, 0x3483, 0x3484, 0x0536, 0x0537, 0x053E, 0x053F};
static struct {
	uint64_t ms;
	uint32_t arg, regs[arrsize(hangRegs)], rptr, ring[48];
	bool taken;
} hangSnap;

static void snapshotHang(uint32_t arg) {
	auto *mmio = gpuMmio;
	if (hangSnap.taken || !mmio)
		return;
	hangSnap.taken = true;
	hangSnap.ms = uptimeMs();
	hangSnap.arg = arg;
	for (size_t i = 0; i < arrsize(hangRegs); i++)
		hangSnap.regs[i] = mmio[hangRegs[i]];
	hangSnap.rptr = mmio[0x21C0];
	if (auto *hw = ringLayoutKnown ? static_cast<uint8_t *>(accelHardware) : nullptr)
		if (auto *pm4 = *reinterpret_cast<uint8_t **>(hw + hwEngines))
			if (auto *ring = *reinterpret_cast<uint8_t **>(pm4 + 0x50)) {
				auto *cpu = *reinterpret_cast<uint32_t **>(ring + 0x40);
				uint32_t dwords = *reinterpret_cast<uint32_t *>(ring + 0x30);
				if (cpu && dwords)
					for (uint32_t i = 0; i < 48; i++)
						hangSnap.ring[i] = cpu[(hangSnap.rptr + dwords - 40 + i) % dwords];
			}
}
static uint32_t accelMmDropped, accelArbPatched;
static uint32_t accelStartPatched, accelCapsCu, accelSdma1Aliased;
static uint64_t vmFbStart, vmFbEnd, vmCarveout;
static uint32_t vmConverted, vmMcAddresses, vmOffsets, vmOther;
static struct {
	uint64_t from, to;
	uint8_t kind;  // 'D' sDMA update, 'T' getPTEValue, 'P' getPDEValue
} vmSamples[4];

// hex may list alternatives, "a|b": the same instruction as different builds encode it (Ventura's compiler allocates
// other registers, e.g. mov [rbx+0x3B8], r14 there is mov [r15+0x3B8], rbx).
static bool codeHas(mach_vm_address_t fn, size_t len, const char *hex) {
	if (const char *bar = strchr(hex, '|')) {
		char first[40] {};
		size_t n = static_cast<size_t>(bar - hex) < sizeof(first) - 1 ? static_cast<size_t>(bar - hex) : sizeof(first) - 1;
		memcpy(first, hex, n);
		return codeHas(fn, len, first) || codeHas(fn, len, bar + 1);
	}
	uint8_t pattern[16];
	size_t n = 0;
	for (; hex[2 * n] && hex[2 * n] != '|' && n < sizeof(pattern); n++) {
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
static uint32_t accelGuardMisses;  // accelerator patches left out because their code differs (route's checks)
static const char *accelGuardMissLast;

// AMDHWVMM::endVMPTUpdate, replaced: Ventura nvram1/2 (and a Ventura tester) died in it at the first accelerated drawing,
// on a null sDMA channel at +0x28, in a panic of Apple's that carries no report. The same 12 instructions in 13.7.7,
// 14.8.9 and 15.8.1 (checked where it's routed): drop the update count at +0x3C and, at 0, have the channel flush
// (vtable +0x140 ready?, +0x158, then +0x160 with 0). Its first 14 bytes hold a branch target, so it can't be wrapped;
// this copy runs instead, and a null channel panics with the report. -crnovmmcheck leaves Apple's.
static uint64_t wrapEndVMPTUpdate(void *that) {
	auto *self = static_cast<uint8_t *>(that);
	auto &count = *reinterpret_cast<int32_t *>(self + 0x3C);
	if (--count != 0)
		return 0;
	auto *channel = *reinterpret_cast<void **>(self + 0x28);
	if (!channel)
		CRDriver::vmmPanic(that);
	if (!vfunc<uint32_t (*)(void *)>(channel, 0x140)(channel))
		return 0;
	vfunc<void (*)(void *)>(channel, 0x158)(channel);
	channel = *reinterpret_cast<void **>(self + 0x28);
	return vfunc<uint64_t (*)(void *, uint32_t)>(channel, 0x160)(channel, 0);
}
static bool wrapPowerUpHWEngines(void *that);

void CRDriver::patchAccelerator(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size) {
	selectAccelLayouts();
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
				accelGuardMisses++;
				accelGuardMissLast = what;
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

	// The 16-bank LDS interpolation fix, at submission (see wrapCommitIb).
	interpFixOff = checkKernelArgument("-crnointerpfix") || family != ChipFamily::Stoney;
	// Raising a shader's VGPR allocation is safe only when the submit hook sees every command buffer that binds it
	// (else the shader runs with registers it doesn't own): the hook raises RSRC1_PS from an append-only table, even
	// when it can't take the lock. -crnointerpbump disables raising.
	interpBump = !checkKernelArgument("-crnointerpbump") && !checkKernelArgument("-crnointerpsubmit");
	if (!interpFixOff && !interpLock) {
		interpLock = IOLockAlloc();
		// The submit-time hook (AMDFijiPM4Channel is the one Polaris uses) fixes shaders before the GPU runs them. It must
		// never block or allocate: the first version made mappings inside the submission path and froze the machine
		// twice. It reads through the carve-out mapping and the kernel physmap only. -crnointerpsubmit disables it
		// (the once-a-second scan then does the fixing, without raises).
		interpSubmit = !checkKernelArgument("-crnointerpsubmit");
		interpHooked = interpLock && interpSubmit && route("__ZN30AMDRadeonX4000_AMDVIPM4Channel27commitIndirectCommandBufferEP30AMD_SUBMIT_COMMAND_BUFFER_INFO",
			reinterpret_cast<mach_vm_address_t>(wrapCommitIb), orgCommitIb, nullptr, 0, "commitIndirectCommandBuffer");
		if (interpLock && interpSubmit)
			interpHooked |= route("__ZN32AMDRadeonX4000_AMDFijiPM4Channel27commitIndirectCommandBufferEP30AMD_SUBMIT_COMMAND_BUFFER_INFO",
				reinterpret_cast<mach_vm_address_t>(wrapCommitIbFiji), orgCommitIbFiji, nullptr, 0,
				"Fiji commitIndirectCommandBuffer");
		// sDMA submissions, read for the copy list only (-crnosdmawatch leaves them).
		if (interpLock && interpSubmit && !checkKernelArgument("-crnosdmawatch"))
			route("__ZN31AMDRadeonX4000_AMDVIsDMAChannel27commitIndirectCommandBufferEP30AMD_SUBMIT_COMMAND_BUFFER_INFO",
				reinterpret_cast<mach_vm_address_t>(wrapCommitSdma), orgCommitSdma, nullptr, 0,
				"sDMA commitIndirectCommandBuffer");
	}

	if (family == ChipFamily::Stoney && !checkKernelArgument("-crnosdmafix")) {
		// Each "a|b": Sequoia/Sonoma's encoding, then Ventura's, then Monterey's (other registers, same offsets).
		// Big Sur: r14 in allocateHWEngines, the state getter at vtable 0x170 and a 32-bit store of its word.
		// Catalina: the engine slots from +0x340 (sDMA at +0x348, +0x350), and startHWEngines only logs a missing sDMA1
		// (it starts sDMA0 and ignores sDMA1's result), so it is left as it is.
		bool bigSur = getKernelVersion() == KernelVersion::BigSur;
		bool catalina = getKernelVersion() == KernelVersion::Catalina;
		static const char *const allocChecks[] {"4c89b3b8030000|49899fb8030000|49899eb8030000",
			"4c89b3c0030000|49899fc0030000|49899ec0030000", nullptr};
		static const char *const allocChecksCatalina[] {"49899e48030000", "49899e50030000", nullptr};
		static const char *const startChecks[] {"4a8bbcf3b8030000|498bbcdfb8030000|498bbcdeb8030000", "ff9048010000",
			"488b7b18|498b7f18|498b7e18", "ff9078010000", "66895008|66897008", nullptr};
		static const char *const startChecksBigSur[] {"498bbcdeb8030000", "ff9048010000", "498b7e18", "ff9070010000",
			"897008", nullptr};
		if (route("__ZN32AMDRadeonX4000_AMDBaffinHardware17allocateHWEnginesEv",
				reinterpret_cast<mach_vm_address_t>(wrapAllocateHWEngines), orgAllocateHWEngines,
				catalina ? allocChecksCatalina : allocChecks, 0x100, "allocateHWEngines") && !catalina) {
			route("__ZN26AMDRadeonX4000_AMDHardware14startHWEnginesEv",
				reinterpret_cast<mach_vm_address_t>(wrapStartHWEngines), orgStartHWEngines,
				bigSur ? startChecksBigSur : startChecks, 0xA0,
				"startHWEngines");
		}
		// Catalina: after the hardware channels, createAccelChannels adds four more scheduler queues from a fixed list:
		// VMPT (sDMA1, priority 0), GFX_HP, SDMA0_HP and SDMA1_HP (priority 1). With sDMA1 redirected to sDMA0, SDMA1_HP
		// lands on SDMA0_HP's slot; Catalina's AMDSWScheduler::addSWQueue has one queue per (scheduler, priority) and
		// refuses it, the channel's init fails, and so does the accelerator (20th chan.init = 0, accel.start = 0). Stop
		// that loop after three (inc rbx; add r12, 0x18; cmp rbx, 4 -> 3). Lookups of SDMA1_HP go through the scheduler
		// (getAccelChannel, priority 0-1: getSWQueue for sDMA1's channel, i.e. sDMA0's) and find SDMA0_HP; the unused
		// slot in the channel array stays NULL, which stop() and the diagnosis report skip.
		if (catalina) {
			static const uint8_t find[] {0x48, 0xFF, 0xC3, 0x49, 0x83, 0xC4, 0x18, 0x48, 0x83, 0xFB, 0x04, 0x0F, 0x82};
			static const uint8_t repl[] {0x48, 0xFF, 0xC3, 0x49, 0x83, 0xC4, 0x18, 0x48, 0x83, 0xFB, 0x03, 0x0F, 0x82};
			KernelPatcher::LookupPatch patch {&kexts[KextRadeonX4000], find, repl, sizeof(find), 1};
			patcher.applyLookupPatch(&patch);
			if (patcher.getError() == KernelPatcher::Error::NoError) {
				traceNote("queues=3 ");
			} else {
				traceNote("queues=err%d ", patcher.getError());
				patcher.clearError();
			}
		}
		static const char *const powerUpChecks[] {"4b8bbcfeb0030000|498bbcdeb0030000", nullptr};
		static const char *const powerUpChecksCatalina[] {"498bbcde40030000", nullptr};
		route("__ZN26AMDRadeonX4000_AMDHardware16powerUpHWEnginesEv",
			reinterpret_cast<mach_vm_address_t>(wrapPowerUpHWEngines), orgPowerUpHWEngines,
			catalina ? powerUpChecksCatalina : powerUpChecks, 0x30, "powerUpHWEngines");
		static const char *const channelChecks[] {"488bbcc7b0030000", "488b80a0010000", nullptr};
		static const char *const channelChecksCatalina[] {"488bbcc740030000", "488b80a0010000", nullptr};
		route("__ZN26AMDRadeonX4000_AMDHardware12getHWChannelE20_eAMD_HW_ENGINE_TYPE18_eAMD_HW_RING_TYPE",
			reinterpret_cast<mach_vm_address_t>(wrapGetHWChannel), orgGetHWChannel,
			catalina ? channelChecksCatalina : channelChecks, 0x26,
			"getHWChannel");
	}
	// Video decode for AppleGVA (see wrapConfigureDevice); -crnogva leaves it, as it does the board-id routing.
	if (checkKernelArgument("-crvideo") && !checkKernelArgument("-crnogva"))
		route("__ZN37AMDRadeonX4000_AMDGraphicsAccelerator15configureDeviceEP11IOPCIDevice",
			reinterpret_cast<mach_vm_address_t>(wrapConfigureDevice), orgConfigureDevice, nullptr, 0, "configureDevice");
	if (checkKernelArgument("-crvideo")) {
		routeAllowJcc = true;
		route("__ZN35AMDRadeonX4000_AMDAccelVideoContext9getHWInfoEP13sHardwareInfo",
			reinterpret_cast<mach_vm_address_t>(wrapGetHWInfo), videoGetHWInfoOrg, nullptr, 0, "video getHWInfo");
		routeAllowJcc = false;
		route("__ZN33AMDRadeonX4000_AMDAccelUVDContext4initEP37AMDRadeonX4000_AMDGraphicsAcceleratorP35AMDRadeonX4000_"
			  "AMDAccelVideoContextP23sVideoContextCreateInfo",
			reinterpret_cast<mach_vm_address_t>(wrapUvdContextInit), orgUvdContextInit, nullptr, 0, "UVD context init");
		route("__ZN33AMDRadeonX4000_AMDAccelVCEContext4initEP37AMDRadeonX4000_AMDGraphicsAcceleratorP35AMDRadeonX4000_"
			  "AMDAccelVideoContextP23sVideoContextCreateInfo",
			reinterpret_cast<mach_vm_address_t>(wrapVceContextInit), orgVceContextInit, nullptr, 0, "VCE context init");
		route("__ZN29AMDRadeonX4000_AMDUVDHWEngine5startEv", reinterpret_cast<mach_vm_address_t>(wrapUvdStart),
			orgUvdStart, nullptr, 0, "UVD start");
		route("__ZN37AMDRadeonX4000_AMDGraphicsAccelerator29addPlistPropertiesToPCIDeviceEPKc",
			reinterpret_cast<mach_vm_address_t>(wrapAddPlistPropertiesToPCIDevice), orgAddPlistPropertiesToPCIDevice,
			nullptr, 0, "addPlistPropertiesToPCIDevice");
		route("__ZN35AMDRadeonX4000_AMDAccelVideoContext12contextStartEv",
			reinterpret_cast<mach_vm_address_t>(wrapVideoContextStart), orgVideoContextStart, nullptr, 0,
			"video contextStart");
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
			// Catalina has the first one in AMDFijiHardware::powerUp too.
			{&kexts[KextRadeonX4000], find1, repl1, sizeof(find1), getKernelVersion() == KernelVersion::Catalina ? 3u : 2u},
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
	// Stoney and Carrizo both have one shader engine with one shader array (Linux gfx_v8_0), against Polaris 11's two.
	if ((family == ChipFamily::Stoney || family == ChipFamily::Carrizo) && !checkKernelArgument("-crnocapsfix")) {
		static const char *const capsChecks[] {"c7476402000000", "c7476c01000000", "c787a000000008000000", nullptr};
		// Monterey's AMDHardware has these fields 8 bytes earlier (SEs 0x5C, SH per SE 0x64, CUs per SH 0x98, the
		// active CUs AMDVIHardware's setupAndInitializeHWCapabilities stores 0x9C).
		static const char *const capsChecksMonterey[] {"c7475c02000000", "c7476401000000", "c7879800000008000000",
			nullptr};
		// Catalina: SEs and SH per SE in one 64-bit store to 0x58 (2, 1), CUs per SH at 0x80, active CUs at 0x84.
		static const char *const capsChecksCatalina[] {"48b80200000001000000", "48894758", "c7878000000008000000",
			nullptr};
		auto version = getKernelVersion();
		route("__ZN32AMDRadeonX4000_AMDBaffinHardware32setupAndInitializeHWCapabilitiesEv",
			reinterpret_cast<mach_vm_address_t>(wrapSetupHWCapabilities), orgSetupHWCapabilities,
			version == KernelVersion::Catalina ? capsChecksCatalina :
			version <= KernelVersion::Monterey ? capsChecksMonterey : capsChecks, 0x30, "setupAndInitializeHWCapabilities");
	}
	if (!checkKernelArgument("-crnovmmcheck")) {
		static const char *const vmmChecks[] {"ff4f3c", "488b7f28", "ff9040010000", "ff9058010000", "488b8060010000", nullptr};
		const char *name = "__ZN23AMDRadeonX4000_AMDHWVMM13endVMPTUpdateEv";
		auto fn = patcher.solveSymbol(index, name, slide, size);
		patcher.clearError();
		bool same = fn != 0;
		for (size_t i = 0; same && vmmChecks[i]; i++)
			same = codeHas(fn, 0x50, vmmChecks[i]);
		KernelPatcher::RouteRequest request {name, wrapEndVMPTUpdate};
		if (!same) {
			traceNote("vmmcheck=nomatch ");
		} else if (patcher.routeMultipleLong(index, &request, 1, slide, size)) {
			traceNote("vmmcheck=on ");
		} else {
			traceNote("vmmcheck=err%d ", patcher.getError());
			patcher.clearError();
		}
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
	auto &sdma1 = *reinterpret_cast<OSObject **>(static_cast<uint8_t *>(that) + hwEngines + 0x10);
	if (sdma1) {
		sdma1->release();
		sdma1 = nullptr;
		accelSdma1Dropped = true;
	}
	traceEvent("hw.allocateHWEngines (sDMA1 dropped)", true, ret);
	return ret;
}

// UVD 6.3 (+0x3D8), VCE 3.4 (+0x3E8) and SAMU (+0x400; Catalina +0x358, +0x368, +0x380, see hwMmSlots): on the APU the SMU keeps them power-gated until a PowerPlay
// back end asks it to power them up (Linux smu8: PPSMC_MSG_UVDPowerON etc.); the Dummy back end never does, and
// programming a gated block can hang the bus. Releasing the engines made AMDGraphicsAccelerator::start fail (nvram68,
// then a NULL dereference in its clean-up path), so they stay; only their power-up is skipped: the slots are emptied
// for the duration of AMDHardware::powerUpHWEngines, whose loop skips empty slots. -crmm powers them up too.
// -crvideo (stage 1 of docs/VIDEO.md): first have the SMU power UVD and VCE up (UVDPowerON / VCEPowerON, as Linux
// smu8_dpm_powergate_uvd/vce do), then let Apple power those two up; SAMU stays skipped.
static void smuVideoPowerOn();

static bool wrapPowerUpHWEngines(void *that) {
	auto &multimedia = hwMmSlots;
	void *saved[arrsize(multimedia)] {};
	bool video = checkKernelArgument("-crvideo");
	if (video)
		smuVideoPowerOn();
	bool skip = !checkKernelArgument("-crmm");
	// Restore only the slots emptied here: restoring all three wrote the unsaved nullptr over UVD and VCE with -crvideo,
	// and every video session then failed in AMDAccelUVDContext::init (getHWEngine(5) = 0).
	size_t first = video ? 2 : 0;
	if (skip) {
		for (size_t i = first; i < arrsize(multimedia); i++) {
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
		for (size_t i = first; i < arrsize(multimedia); i++)
			*reinterpret_cast<void **>(static_cast<uint8_t *>(that) + multimedia[i]) = saved[i];
	traceEvent("hw.powerUpHWEngines", true, ret);
	traceNote("hw.powerUpHWEngines=%x(mm skipped %u) ", ret, skip ? accelMmDropped : 0);
	return ret;
}

// -crvideo, AppleGVA side (docs/VIDEO.md stage 3). With the board-id routed, the Sonoma log still had
// "[HEVC] GVA ERROR: IOGVACodec not set" and the AMD video context closed 2 ms after opening. Two causes:
//  - AppleGVA only takes a GPU whose registry name is GFX0, GFXA, IGPU, IOPP or display (it compares the name before
//    reading IOGVACodec / IOGVAHEVCDecode); ours is the ACPI name, VGA. Rename it to GFX0, as WhateverGreen does.
//  - AMDGraphicsAccelerator::configureDevice publishes IOGVAHEVCDecode (+ capabilities) only when the PCI device's
//    MM_EnableHEVCDecode and MM_EnableHEVCEncode are both true, and AMD9500Controller's aty_properties set both false
//    for every Polaris. Force them true for that call (configureDevice is their only reader in 13.x-15.x), then take
//    HEVC encode back out: Stoney's UVD 6.2 has no encode rings (Polaris encodes HEVC on UVD).

static bool wrapConfigureDevice(IOService *that, IOPCIDevice *pci) {
	if (pci) {
		const char *name = pci->getName();
		if (!name || (strcmp(name, "GFX0") && strcmp(name, "IGPU") && strcmp(name, "display"))) {
			pci->setName("GFX0");
			gvaHevcState |= 8;
		}
		pci->setProperty("MM_EnableHEVCDecode", kOSBooleanTrue);
		pci->setProperty("MM_EnableHEVCEncode", kOSBooleanTrue);
		gvaHevcState |= 1;
	}
	if (pci) {
		uint32_t id = 0;
		if (WIOKit::getOSDataValue(pci, "device-id", id))
			videoSpoofId = id & 0xFFFF;
	}
	bool ret = FunctionCast(wrapConfigureDevice, orgConfigureDevice)(that, pci);
	if (pci)
		pci->setProperty("MM_EnableHEVCEncode", kOSBooleanFalse);
	if (that->getProperty("IOGVAHEVCDecode"))
		gvaHevcState |= 2;
	// With UVD started (b298f8a) the first session, the lock screen's aerial (HEVC Main10, which UVD 6.2 can't decode),
	// left UVD busy and the GPU restart never finished (nvram15). Until Main10 is filtered out of what is published,
	// HEVC stays in software and only H.264 reaches UVD; the capabilities go to the log for that filter. -cruvdhevc keeps
	// HEVC decode published.
	if (auto *caps = that->getProperty("IOGVAHEVCDecodeCapabilities")) {
		if (auto *s = OSSerialize::withCapacity(512)) {
			if (caps->serialize(s))
				diskLogEvent("hevc decode capabilities: %.900s", s->text());
			s->release();
		}
	}
	if ((gvaHevcState & 2) && !checkKernelArgument("-cruvdhevc")) {
		that->removeProperty("IOGVAHEVCDecode");
		that->removeProperty("IOGVAHEVCDecodeCapabilities");
		gvaHevcState |= 0x10;
	}
	if (that->getProperty("IOGVAHEVCEncode") || that->getProperty("IOGVAHEVCEncodeCapabilities")) {
		that->removeProperty("IOGVAHEVCEncode");
		that->removeProperty("IOGVAHEVCEncodeCapabilities");
		gvaHevcState |= 4;
	}
	traceNote("configureDevice=%d hevc %x ", ret, gvaHevcState);
	return ret;
}

// With the decoder published, AppleGVA opens an AMD video context per attempt (H.264 and HEVC, decode and encode) and
// AMD's user-space driver gives up at once: "AVD_api.Create failed err=5", no call reaches the context between
// contextStart and contextStop. The first thing it can read is AMDAccelVideoContext::getHWInfo, which can't be routed
// (a jump in its first 14 bytes); it only copies fields, so call it after contextStart and log what user space gets.
static uint32_t videoContextStarts;

static uint32_t wrapVideoContextStart(void *that) {
	uint32_t n = ++videoContextStarts;
	traceEvent("vid.contextStart", false, 0);
	uint32_t ret = FunctionCast(wrapVideoContextStart, orgVideoContextStart)(that);
	traceEvent("vid.contextStart", true, ret);
	traceNote("vid.contextStart=%x ", ret);
	(void)n;
	return ret;
}

// The answer (2026-10-27 log): getHWInfo's +4 is the hardware's device ID, the real 0x98E4, which AMD's user-space
// driver doesn't know (the rest is Polaris 11's: family 0x82, external revision 0x5A = Polaris 11 rev 0 + 0x5A). Hand
// user space the device-id the system sees (the spoof) instead; the accelerator itself keeps the real one.
static uint32_t wrapGetHWInfo(void *that, uint32_t *info) {
	uint32_t ret = FunctionCast(wrapGetHWInfo, videoGetHWInfoOrg)(that, info);
	uint32_t n = ++videoHWInfoCalls;
	(void)that;
	if (ret == 0 && info && videoSpoofId && (info[1] & 0xFFFF) != videoSpoofId) {
		if (n <= 2)
			diskLogEvent("video hwinfo: family %x device %x -> %x, rev %x", info[0], info[1], videoSpoofId, info[2]);
		info[1] = videoSpoofId;
	}
	return ret;
}

// Next (same log): AMDSIVideoContext::newContext -> AMDAccelUVDContext::init returns false (kIOReturnUnsupported).
// init fails when the create info's instance (+0x30) is neither 0 nor 1, or when the hardware's engine for it
// (getHWEngine(5 + instance) = hardware[0x3D8 / 0x3E0]) isn't an AMDUVDHWEngine; Stoney has one UVD. Log both.
static const char *className(void *object) {
	if (!object)
		return "-";
	auto *meta = static_cast<OSMetaClassBase *>(object)->getMetaClass();
	return meta ? meta->getClassName() : "?";
}

static bool wrapUvdContextInit(void *that, void *accel, void *videoContext, uint32_t *createInfo) {
	static uint32_t calls;
	// A context whose UVD doesn't respond would hang the decode channel, and the GPU restart that follows never comes
	// back (2026-10-27: "UVDDec event timeout", black screen). Refuse it: user space then decodes in software.
	if (!uvdReadyForSession()) {
		traceNote("vid.uvdInit=refused ");
		return false;
	}
	bool ret = FunctionCast(wrapUvdContextInit, orgUvdContextInit)(that, accel, videoContext, createInfo);
	traceNote("vid.uvdInit=%d ", ret);
	if (++calls <= 3 && createInfo) {
		diskLogEvent("uvd init = %d: create %08x %08x %08x %08x %08x %08x %08x %08x | %08x %08x %08x %08x %08x %08x %08x "
			"%08x %08x", ret, createInfo[0], createInfo[1], createInfo[2], createInfo[3], createInfo[4], createInfo[5],
			createInfo[6], createInfo[7], createInfo[8], createInfo[9], createInfo[10], createInfo[11], createInfo[12],
			createInfo[13], createInfo[14], createInfo[15], createInfo[16]);
		if (accelHardware) {
			auto *hw = static_cast<uint8_t *>(accelHardware);
			void *uvd0 = *reinterpret_cast<void **>(hw + 0x3D8), *uvd1 = *reinterpret_cast<void **>(hw + 0x3E0);
			diskLogEvent("uvd init: engine 5 %p %s, engine 6 %p %s", uvd0, className(uvd0), uvd1, className(uvd1));
		}
	}
	return ret;
}

// AMDHardware::startHWEngines starts both sDMA engines and fails if either is missing; with one engine, start it and set
// the same "sDMA started" bit (bit 9 of the 16-bit word at +8 of the info block from this+0x18, vtable +0x178).
bool CRDriver::wrapStartHWEngines(void *that) {
	auto *self = static_cast<uint8_t *>(that);
	if (*reinterpret_cast<void **>(self + hwEngines + 0x10))
		return FunctionCast(wrapStartHWEngines, callback->orgStartHWEngines)(that);
	void *sdma0 = *reinterpret_cast<void **>(self + hwEngines + 8);
	bool ok = sdma0 && vfunc<bool (*)(void *)>(sdma0, 0x148)(sdma0);
	void *info = *reinterpret_cast<void **>(self + 0x18);
	auto *state = vfunc<uint8_t *(*)(void *)>(info, hwStateSlot)(info);
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
	uint32_t activeCu = *reinterpret_cast<uint32_t *>(self + capsOff.activeCu);  // minimum active CUs per SH, from CAIL
	*reinterpret_cast<uint32_t *>(self + capsOff.se) = 1;                         // shader engines
	*reinterpret_cast<uint32_t *>(self + capsOff.sh) = 1;                         // SH per SE
	if (activeCu)
		*reinterpret_cast<uint32_t *>(self + capsOff.cu) = activeCu;              // CUs per SH
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
	auto &flags = *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(cail) + cailFlagsOff);
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
	CRDriver::ensureMmio();
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
	static constexpr uint16_t GetMaxSclkLevel = 0x3B, SetClkTableAddrHi = 0x49, SetClkTableAddrLo = 0x4A,
	                          ClkTableXferToDram = 0x267, GetFeatureStatus = 0x2, EnableAllSmuFeatures = 0x3,
	                          SetSclkSoftMin = 0x12, SetSclkSoftMax = 0x13, SetSclkHardMin = 0x14,
	                          SetMinDeepSleepSclk = 0x11, SetWatermarkFrequency = 0x26E;  // cz_ppsmc.h
	static constexpr uint32_t SclkDpmMask = 0x80000, SclkDpmOn = 0x200000;  // SCLK_DPM_MASK, scoreboard SclkDpmOn
	static constexpr uint32_t CgFreqTranVoting0 = 0xD02001A8, VotingRightsDefault = 0x3FFFC102;
	static constexpr uint16_t UvdPowerOn = 0x8, VcePowerOn = 0xA, UvdPowerOff = 0x7;
	static constexpr uint32_t UvdDpmMask = 0x10000, VceDpmMask = 0x400000;
	// UCODE_ID_* (smu_ucode_xfer_cz.h); Stoney has no SDMA1 or MEC2, Linux maps them to SDMA0 and MEC_JT1.
	static constexpr uint8_t SDMA0 = 0, CE = 2, PFP = 3, ME = 4, MEC_JT1 = 5, RLC_G = 8, RLC_SCRATCH = 9,
	                         RLC_SRM_ARAM = 10, RLC_SRM_DRAM = 11;
	static constexpr uint32_t StoneyLoadMask = 1U << SDMA0 | 1U << CE | 1U << PFP | 1U << ME | 1U << MEC_JT1 | 1U << RLC_G;
	// Carrizo / Bristol Ridge also have SDMA1 and a second MEC (its own jump table), as in Linux smu8_smumgr.
	static constexpr uint8_t SDMA1 = 1, MEC_JT2 = 6;
	static constexpr uint32_t CarrizoLoadMask = StoneyLoadMask | 1U << SDMA1 | 1U << MEC_JT2;
	static constexpr uint8_t TaskUcodeLoad = 1, TaskUcodeSave = 2, TaskInitialize = 5;  // TASK_TYPE_*
	static constexpr uint8_t ArgInitMmPwrLog = 0, ArgInitClkTable = 1;                 // TASK_ARG_INIT_*
	static constexpr uint32_t JobListEntries = 32;                                     // NUM_JOBLIST_ENTRIES
	static constexpr uint16_t EndOfTaskList = 0xFFFF;
	// Region layout: TOC, the scratch buffers (Linux smu_buffer), then the images, 4 KB aligned; then the IH ring and its
	// write-back word (see moveIhRing).
	static constexpr uint32_t TocOffset = 0, ScratchOffset = 0x1000, UcodeOffset = 0x5000, IhOffset = 0x60000,
	                          IhMaxSize = 0x40000, RegionSize = IhOffset + IhMaxSize + 0x1000;
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
// CAIL's Stoney_UcodeInfo or Carrizo_UcodeInfo: RLC, SDMA0, SDMA1, CE, PFP, ME, MEC1, MEC2 (Stoney's SDMA1 and MEC2 are
// "SkipLoading" entries with no data).
static Smu8::CailUcode **stoneyUcodeInfo;
static mach_vm_address_t ucodeGoldenSettings;  // stripped HWLibs: GetGpuGoldenSettings(cail), whose +0x10 is the table
static bool smuCarrizo;  // the SMU load is for Carrizo / Bristol Ridge (SDMA1, MEC_JT2)
static uint32_t smuFormatCalls, smuCailSize, smuRegionStart;
static uint16_t smuCpReloadTask = 0xFFFF, smuClkTableJob = 0xFFFF;
static uint32_t smuClkTableAt;  // the clock table scratch buffer's offset in our region
static struct {
	uint32_t count, before, after, ms, resp;
	uint64_t at;
} smuReload;
static struct {
	const char *result;
	uint64_t blockOffset, mc, phys;
	uint32_t before, after, ms, resp[6], mecBase[3], tasks;
} smuLoad;

static uint64_t wrapStoneyFormatSmuDram(void *cail) {
	if (!stoneyUcodeInfo && ucodeGoldenSettings) {
		auto *golden = reinterpret_cast<uint8_t *(*)(void *)>(ucodeGoldenSettings)(cail);
		if (golden)
			stoneyUcodeInfo = *reinterpret_cast<Smu8::CailUcode ***>(golden + 0x10);
	}
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
	smuLastMsg = msg;
	uint32_t resp = wait();
	diskLogEvent("smu msg %x arg %x -> resp %x, out arg %x", msg, arg, resp, mmio[Smu8::ARG]);
	return resp ? resp : 0xC0000 | msg;  // never answered
}

static uint32_t smuReadSram(uint32_t address) {
	gpuMmio[Smu8::MP0PUB_IND_INDEX] = Smu8::SramBase + address;
	return gpuMmio[Smu8::MP0PUB_IND_DATA];
}

// Steps: 1 region written, 2 DriverDramAddrHi/Lo + InitJobs, 3 ARAM save job, 4 power-profiling job, 5 boot-up job
// and the wait, 6 MEC base set, 7 CAIL's Stoney_LoadRlcUcode done (RLC started).
static void smuStep(uint32_t step) {
	if (step < smuStepLimit)
		return;
	smuLoad.result = "stopped by crsmustep";
	__atomic_store_n(&smuStoppedAt, step, __ATOMIC_RELAXED);
	for (;;)
		IOSleep(1000);
}

static void smuResetAfterPowerLoss();
static uint32_t smuLoadCalls, smuPowerLosses;

static void smuLoadFirmware(void *cail) {
	using namespace Smu8;
	CRDriver::ensureMmio();
	auto *mmio = gpuMmio;
	if (!mmio)
		return;
	smuLoadCalls++;
	if (smuLoad.result) {
		// CAIL initialises the ASIC again after a wake. S3 powers the APU's GPU off, SMU included: its firmware has
		// forgotten our load (UcodeLoadStatus no longer shows the engines) and the clock levels, so load it again.
		uint32_t status = smuReadSram(UcodeLoadStatus), mask = smuCarrizo ? CarrizoLoadMask : StoneyLoadMask;
		bool lost = !strcmp(smuLoad.result, "loaded") && (status & mask) != mask;
		diskLogEvent("smu load call %u: load status %x (%s)", smuLoadCalls, status, lost ? "lost: loading again" : "kept");
		if (!lost)
			return;
		smuPowerLosses++;
		smuResetAfterPowerLoss();
	}
	auto *base = static_cast<uint8_t *>(cail);
	smuLoad.blockOffset = *reinterpret_cast<uint64_t *>(base + cailSmuBlockOff);
	if (!stoneyUcodeInfo) {
		smuLoad.result = "no UcodeInfo";
		return;
	}
	if (!smuRegionStart || !smuLoad.blockOffset || *reinterpret_cast<uint32_t *>(base + cailSmuSizeOff) != smuCailSize) {
		smuLoad.result = "CAIL's micro-engine block isn't reserved";
		return;
	}
	uint64_t regionFb = smuLoad.blockOffset + smuRegionStart;
	uint64_t fbBase = static_cast<uint64_t>(mmio[Reg::MC_VM_FB_LOCATION] & 0xFFFF) << 24;
	smuLoad.mc = fbBase + regionFb;
	smuLoad.phys = (static_cast<uint64_t>(mmio[Reg::MC_VM_FB_OFFSET] & 0x3FFFF) << 22) + regionFb;
	// Never write outside the carve-out (an FB offset past its end would be some other system RAM).
	uint64_t fbSize = ((static_cast<uint64_t>(mmio[Reg::MC_VM_FB_LOCATION] >> 16) + 1) << 24) - fbBase;
	if (regionFb + RegionSize > fbSize) {
		smuLoad.result = "region outside the carve-out";
		return;
	}

	// The images: RLC, SDMA0, CE, PFP, ME, MEC1 (UcodeInfo slots 0, 1, 3, 4, 5, 6); on Carrizo also SDMA1 (slot 2) and
	// MEC2 (slot 7), whose jump table alone is loaded (the MEC runs from the image at CP_CPC_IC_BASE).
	static const uint8_t slots[] {0, 1, 3, 4, 5, 6, 2, 7};
	const uint32_t slotNum = smuCarrizo ? 8 : 6, imageNum = smuCarrizo ? 7 : 6;
	const CailUcode *ucode[arrsize(slots)] {};
	for (size_t i = 0; i < slotNum; i++) {
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
	uint32_t offset = UcodeOffset, ucodeAt[arrsize(slots)] {}, ucodeSize[arrsize(slots)] {}, jtAt = 0, jtSize = 0, jt2At = 0,
		jt2Size = 0;
	bool fits = true;
	for (size_t i = 0; i < imageNum && fits; i++) {
		ucodeAt[i] = offset;
		ucodeSize[i] = ucode[i]->dwords * 4;
		offset = (offset + ucodeSize[i] + 0xFFF) & ~0xFFFU;
		fits = offset <= IhOffset;
		if (fits)
			memcpy(region + ucodeAt[i], ucode[i]->data, ucodeSize[i]);
	}
	auto *mec = ucode[5];
	if (fits && mec->jtSize && mec->jtOffset + mec->jtSize <= mec->dwords) {
		jtAt = offset;
		jtSize = mec->jtSize * 4;
		fits = jtAt + jtSize <= IhOffset;
		if (fits)
			memcpy(region + jtAt, mec->data + mec->jtOffset, jtSize);
	}
	if (fits && smuCarrizo) {
		auto *mec2 = ucode[7];
		if (mec2->jtSize && mec2->jtOffset + mec2->jtSize <= mec2->dwords) {
			jt2At = (jtAt + jtSize + 0xFFF) & ~0xFFFU;
			jt2Size = mec2->jtSize * 4;
			fits = jt2At + jt2Size <= IhOffset;
			if (fits)
				memcpy(region + jt2At, mec2->data + mec2->jtOffset, jt2Size);
		}
	}
	if (!fits || !jtSize || (smuCarrizo && !jt2Size)) {
		OSSafeReleaseNULL(map);
		OSSafeReleaseNULL(desc);
		smuLoad.result = fits ? "MEC has no jump table" : "images don't fit";
		return;
	}
	enum { Rlc, Sdma0, Ce, Pfp, Me, Mec, Sdma1 };

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
		else if (arg == MEC_JT2)
			task(TaskUcodeLoad, arg, jt2At, jt2Size, last);
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
	load(smuCarrizo ? MEC_JT2 : MEC_JT1, Mec, false);  // Linux loads JT1 twice on Stoney
	load(RLC_G, Rlc, false);
	scratch(TaskUcodeLoad, RLC_SCRATCH, Scratch, false);
	scratch(TaskUcodeLoad, RLC_SRM_ARAM, Aram, false);
	scratch(TaskUcodeLoad, RLC_SRM_DRAM, Dram, true);
	uint16_t powerProfilingJob = used;
	scratch(TaskInitialize, ArgInitMmPwrLog, PowerLog, true);
	uint16_t bootupJob = used;
	load(SDMA0, Sdma0, false);
	if (smuCarrizo)
		load(SDMA1, Sdma1, false);
	load(CE, Ce, false);
	load(PFP, Pfp, false);
	load(ME, Me, false);
	load(MEC_JT1, Mec, false);
	if (smuCarrizo)
		load(MEC_JT2, Mec, false);
	load(RLC_G, Rlc, true);
	smuClkTableJob = used;
	smuClkTableAt = scratchAt[ClkTable];
	scratch(TaskInitialize, ArgInitClkTable, ClkTable, true);
	// Not in Linux's TOC: the CP alone (CE, PFP, ME, MEC jump table), for reloading after Apple's GPU reset.
	smuCpReloadTask = used;
	load(CE, Ce, false);
	load(PFP, Pfp, false);
	load(ME, Me, false);
	load(MEC_JT1, Mec, !smuCarrizo);
	if (smuCarrizo)
		load(MEC_JT2, Mec, true);
	smuLoad.tasks = used;
	__sync_synchronize();
	(void)*reinterpret_cast<volatile uint32_t *>(region + TocOffset);

	// smu8_request_smu_load_fw.
	uint64_t start = uptimeMs();
	smuLoad.before = smuReadSram(UcodeLoadStatus);
	smuStep(1);
	mmio[MP0PUB_IND_INDEX] = SramBase + UcodeLoadStatus;
	mmio[MP0PUB_IND_DATA] = 0;
	smuLoad.resp[0] = smuSendMessage(DriverDramAddrHi, static_cast<uint32_t>(smuLoad.mc >> 32));
	smuLoad.resp[1] = smuSendMessage(DriverDramAddrLo, static_cast<uint32_t>(smuLoad.mc));
	smuLoad.resp[2] = smuSendMessage(InitJobs, 0);
	smuStep(2);
	smuLoad.resp[3] = smuSendMessage(ExecuteJob, aramJob);
	smuStep(3);
	smuLoad.resp[4] = smuSendMessage(ExecuteJob, powerProfilingJob);
	smuStep(4);
	smuLoad.resp[5] = smuSendMessage(ExecuteJob, bootupJob);
	uint32_t status = 0;
	const uint32_t loadMask = smuCarrizo ? CarrizoLoadMask : StoneyLoadMask;
	for (uint32_t i = 0; i < 100000; i++) {  // 1 s
		status = smuReadSram(UcodeLoadStatus);
		if ((status & loadMask) == loadMask)
			break;
		IODelay(10);
	}
	smuLoad.after = status;
	smuLoad.ms = static_cast<uint32_t>(uptimeMs() - start);
	smuStep(5);

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
	smuStep(6);

	OSSafeReleaseNULL(map);
	OSSafeReleaseNULL(desc);
	smuLoad.result = (status & loadMask) == loadMask ? "loaded" : "timed out";
	smuFreshLoad = true;
	traceNote("smuload=%x->%x ", smuLoad.before, smuLoad.after);
}

// Apple's hang recovery (AMDVIPM4Engine reset, then pm4.initializeMicroEngine again) soft-resets the CP, which wipes
// the microcode the SMU loaded (nvram112: CE reads 0, PFP only its first word), and CAIL's direct reload does nothing
// on Stoney. On every re-initialisation after the first, halt the CP and have the SMU run the CP-only job.
// -crnosmureload disables it.
static void smuReloadCp() {
	using namespace Smu8;
	auto *mmio = gpuMmio;
	if (!mmio || !smuLoad.result || strcmp(smuLoad.result, "loaded") || smuCpReloadTask == 0xFFFF ||
		checkKernelArgument("-crnosmureload"))
		return;
	constexpr uint32_t CP_ME_CNTL = 0x21B6;
	const uint32_t mask = 1U << CE | 1U << PFP | 1U << ME | 1U << MEC_JT1 | (smuCarrizo ? 1U << MEC_JT2 : 0);
	mmio[CP_ME_CNTL] = mmio[CP_ME_CNTL] | 0x15000000;  // CE, PFP, ME halted
	uint64_t start = uptimeMs();
	uint32_t before = smuReadSram(UcodeLoadStatus);
	smuReload.before = before;
	mmio[MP0PUB_IND_INDEX] = SramBase + UcodeLoadStatus;
	mmio[MP0PUB_IND_DATA] = before & ~mask;
	smuReload.resp = smuSendMessage(ExecuteJob, smuCpReloadTask);
	uint32_t status = 0;
	for (uint32_t i = 0; i < 100000; i++) {
		status = smuReadSram(UcodeLoadStatus);
		if ((status & mask) == mask)
			break;
		IODelay(10);
	}
	smuReload.after = status;
	smuReload.ms = static_cast<uint32_t>(uptimeMs() - start);
	smuReload.at = start;
	smuReload.count++;
	traceNote("smureload=%x ", status);
}

static bool smuLoadOk() {
	return smuLoad.result && !strcmp(smuLoad.result, "loaded");
}

// -crvideo: UVD and VCE powered up by the SMU (static power gating off: argument 0) and their DPM on, before Apple
// powers the engines. Their registers may only be read once this has answered (a gated block can hang the bus).
static struct {
	bool done;
	uint32_t resp[3];
	uint64_t ms;
} smuVideo;

static bool videoClocksSet(const char *when);

// UVD reads all zeros at the session check (clocks running, SRBM reset clear, SMU UVDPowerON answering 1). Whether it
// was ever alive: snapshots at the MMIO probe (before Apple's driver), around the boot power-on and at the session
// check. Besides status and the GPCOM_DATA0 write test, the UVD power registers CAIL's
// stoney_power_down_uvd_block_for_dpg_mode uses: POWER_STATUS 0x38C4, PGFSM_CONFIG 0x38C0, tile status 0x38C2/3/5/6/8,
// 0x38EE/F (it writes 0x5FF / 0x6FF to CONFIG and expects tiles 0x12 / 0x10; nothing in CAIL powers them back up).
struct UvdSnapshot {
	bool taken, responds;
	uint64_t ms;
	uint32_t status, data0, power, pgfsm, tiles[7], srbm;
};
static UvdSnapshot uvdAtProbe;

static void uvdSnapshot(UvdSnapshot &s) {
	static const uint32_t tileRegs[] {0x38C2, 0x38C3, 0x38C5, 0x38C6, 0x38C8, 0x38EE, 0x38EF};
	auto *mmio = gpuMmio;
	s.ms = uptimeMs();
	s.status = mmio[0x3DAF];
	s.data0 = mmio[0x3BC4];
	mmio[0x3BC4] = 0x5AC3A55C;
	s.responds = mmio[0x3BC4] == 0x5AC3A55C;
	mmio[0x3BC4] = s.data0;
	s.power = mmio[0x38C4];
	s.pgfsm = mmio[0x38C0];
	for (size_t i = 0; i < arrsize(tileRegs); i++)
		s.tiles[i] = mmio[tileRegs[i]];
	s.srbm = mmio[0x398];
	s.taken = true;
}

static void uvdSnapshotLog(const char *when, const UvdSnapshot &s) {
	if (!s.taken)
		return;
	diskLogEvent("uvd state %s (%llu ms): responds %d status %x power %x pgfsm %x tiles %x %x %x %x %x %x %x srbm %x", when,
		s.ms, s.responds, s.status, s.power, s.pgfsm, s.tiles[0], s.tiles[1], s.tiles[2], s.tiles[3], s.tiles[4],
		s.tiles[5], s.tiles[6], s.srbm);
}

static void uvdSnapshotNow(const char *when) {
	UvdSnapshot s {};
	uvdSnapshot(s);
	uvdSnapshotLog(when, s);
}

static void smuVideoPowerOn() {
	using namespace Smu8;
	if (smuVideo.done || !gpuMmio)
		return;
	smuVideo.done = true;
	smuVideo.ms = uptimeMs();
	uvdSnapshotLog("at probe", uvdAtProbe);
	uvdSnapshotNow("before power on");
	smuVideo.resp[0] = smuSendMessage(UvdPowerOn, 0);
	smuVideo.resp[1] = smuSendMessage(VcePowerOn, 0);
	smuVideo.resp[2] = smuSendMessage(EnableAllSmuFeatures, UvdDpmMask | VceDpmMask);
	diskLogEvent("video power on: UVDPowerON %x, VCEPowerON %x, UVD/VCE DPM %x", smuVideo.resp[0], smuVideo.resp[1],
		smuVideo.resp[2]);
	IODelay(1000);
	uvdSnapshotNow("after power on");
	if (smuVideo.resp[0] == 1 && smuVideo.resp[1] == 1)
		videoClocksSet("boot");
	traceNote("video=%x/%x/%x ", smuVideo.resp[0], smuVideo.resp[1], smuVideo.resp[2]);
}

static bool videoPowered() {
	return smuVideo.done && smuVideo.resp[0] == 1 && smuVideo.resp[1] == 1;
}

// UVD/VCE clocks. On an APU Linux sets them itself before the engines start (vi_set_uvd_clocks / vi_set_vce_clocks:
// DCLK in GNB_CLK1, VCLK in GNB_CLK2, ECLK in GNB_CLK3; DFS divider in DFS_CNTL[6:0], then wait for STATUS bit 16);
// Apple leaves them to PowerPlay, whose Dummy back end does nothing. With no clock UVD's registers don't take writes
// and VCE's processor never leaves reset (both seen 2026-10-27). The DFS VCO is 3600 MHz (the SMU's SCLK table: DID 0x10
// = /4 = 900 MHz, 0x24 = /9 = 400 MHz), so DID 0x24 gives 400 MHz. A clock whose divider is 0 (none) gets it; the log
// has what each held. -crnovclk leaves them.
static constexpr uint32_t VideoClockDid = 0x24;
static const struct {
	const char *name;
	uint32_t cntl, status;
} videoClocks[] {{"dclk", 0xD82200F0, 0xD822010C}, {"vclk", 0xD8220110, 0xD822012C}, {"eclk", 0xD8220130, 0xD822014C}};

static uint32_t smcRead(uint32_t reg) {
	gpuMmio[Smu8::MP0PUB_IND_INDEX] = reg;
	return gpuMmio[Smu8::MP0PUB_IND_DATA];
}

static void smcWrite(uint32_t reg, uint32_t value) {
	gpuMmio[Smu8::MP0PUB_IND_INDEX] = reg;
	gpuMmio[Smu8::MP0PUB_IND_DATA] = value;
}

static bool videoClocksSet(const char *when) {
	bool allReady = true;
	for (auto &clock : videoClocks) {
		uint32_t cntl = smcRead(clock.cntl), status = smcRead(clock.status), now = cntl;
		if (!(cntl & 0x7F) && !checkKernelArgument("-crnovclk")) {
			now = (cntl & ~0x7FU) | VideoClockDid;
			smcWrite(clock.cntl, now);
			for (uint32_t i = 0; i < 1000 && !((status = smcRead(clock.status)) & 0x10000); i++)
				IODelay(1000);
			now = smcRead(clock.cntl);
		}
		allReady &= (now & 0x7F) && (status & 0x10000);
		diskLogEvent("video clock %s (%s): cntl %x -> %x, status %x", clock.name, when, cntl, now, status);
	}
	return allReady;
}

static bool eclkRunning() {
	return (smcRead(videoClocks[2].cntl) & 0x7F) && (smcRead(videoClocks[2].status) & 0x10000);
}

// First UVD session (2026-10-27): Apple's UVD start "succeeded" (loadFW, startRB, start = 1) yet UVD_STATUS, VCPU_CNTL,
// SOFT_RESET and LMI_STATUS read 0 before and after it, while VCE's read real values: UVD's registers don't take
// writes, so it isn't really powered or clocked, the decode fence never retired, and the restart left a black screen.
// Before a UVD context is made, check that a plain read/write register (UVD_GPCOM_VCPU_DATA0) keeps a value; if not,
// try the SMU power-up again (as at boot, then off + on with dynamic gating) and log what each gives; a UVD that still
// doesn't respond gets no session. -crnouvdcheck skips the check.
// Clocks weren't it (nvram11 session: DCLK/VCLK/ECLK dividers 12/f/f, all ready, UVD still all zeros). Linux's
// uvd_v6_0_start takes the block out of SRBM_SOFT_RESET (SOFT_RESET_UVD) before touching it, and a block held there
// reads 0 and drops writes: the check clears that bit first (-crnouvdunreset leaves it).
static constexpr uint32_t UvdGpcomData0 = 0x3BC4;
static constexpr uint32_t SrbmSoftReset = 0x398, SrbmResetUvd = 0x40000;
static uint32_t uvdState;  // 0 unchecked, 1 alive, 2 dead, 3 firmware didn't boot (2 and 3: sessions refused)

static bool uvdResponds() {
	uint32_t old = gpuMmio[UvdGpcomData0];
	gpuMmio[UvdGpcomData0] = 0x5AC3A55C;
	bool ok = gpuMmio[UvdGpcomData0] == 0x5AC3A55C;
	gpuMmio[UvdGpcomData0] = old;
	return ok;
}

static void uvdRegs(const char *when) {
	diskLogEvent("uvd regs %s: status %x vcpu %x reset %x lmi %x data0 %x cgc %x srbm %x", when, gpuMmio[0x3DAF],
		gpuMmio[0x3D98], gpuMmio[0x3DA0], gpuMmio[0x3D67], gpuMmio[UvdGpcomData0], gpuMmio[0x3D2C], gpuMmio[SrbmSoftReset]);
}

// The cause (7013a3b snapshots): UVD answered at the MMIO probe, but by the boot power-on PGFSM_CONFIG held 0x5FF (all
// tiles, POWER_DOWN, P1_SELECT), every tile read 0x12 and POWER_STATUS 1: CAIL's stoney_power_down_uvd_block_for_dpg_mode
// had run, and SMU UVDPowerON doesn't undo it. Its own retry path powers the tiles up with 0x6FF (POWER_UP) and expects
// 0x10. Before every session (CAIL may power them down again), do that, then POWER_STATUS back to 0 as at the probe, with
// dynamic power gating mode off as Linux's uvd_v6_0_start has it. -crnouvdtiles leaves them.
static constexpr uint32_t UvdPgfsmConfig = 0x38C0, UvdPgfsmTile1 = 0x38C2, UvdPowerStatus = 0x38C4;
static constexpr uint32_t UvdTileOff = 0x12, UvdTileOn = 0x10;

static void uvdPowerTilesUp() {
	auto *mmio = gpuMmio;
	uint32_t config = mmio[UvdPgfsmConfig], tile = mmio[UvdPgfsmTile1], power = mmio[UvdPowerStatus];
	if (checkKernelArgument("-crnouvdtiles") || (tile != UvdTileOff && (power & 3) == 0))
		return;
	mmio[UvdPgfsmConfig] = 0x6FF;
	for (uint32_t i = 0; i < 100 && (tile = mmio[UvdPgfsmTile1]) != UvdTileOn; i++)
		IODelay(10);
	mmio[UvdPowerStatus] = power & ~0x7U;
	IODelay(100);
	diskLogEvent("uvd tiles up: pgfsm %x -> %x, tile1 %x, power %x -> %x", config, mmio[UvdPgfsmConfig], tile, power,
		mmio[UvdPowerStatus]);
	traceNote("uvdtiles=%x ", tile);
}

// On hold (docs/VIDEO.md): with the firmware running, decode still hangs UVD, because Apple's driver gives it GPU
// virtual addresses (IB VMID 7, nvram17) and Stoney's UVD 6.2 has no VM support (Linux runs it in physical mode,
// rewriting every buffer address). Until that translation exists, UVD sessions are refused unless -cruvd: user space
// decodes in software, as before.
static bool uvdReadyForSession() {
	using namespace Smu8;
	if (!checkKernelArgument("-cruvd")) {
		static bool logged;
		if (!logged) {
			logged = true;
			diskLogEvent("uvd: sessions refused (decode on hold, -cruvd to try)");
		}
		return false;
	}
	if (!uvdPanicMs && checkKernelArgument("-cruvdpanic"))
		uvdPanicMs = uptimeMs() + 20000;
	if (gpuMmio && videoPowered())
		uvdPowerTilesUp();
	if (uvdState || checkKernelArgument("-crnouvdcheck"))
		return uvdState == 1;
	if (!gpuMmio || !videoPowered()) {
		uvdState = 2;
		diskLogEvent("uvd check: video not powered, sessions refused");
		return false;
	}
	videoClocksSet("session");
	uvdRegs("before");
	uvdSnapshotNow("at session");
	uint32_t srbm = gpuMmio[SrbmSoftReset];
	if ((srbm & SrbmResetUvd) && !checkKernelArgument("-crnouvdunreset")) {
		gpuMmio[SrbmSoftReset] = srbm & ~SrbmResetUvd;
		IODelay(5000);
		uvdRegs("unreset");
	}
	diskLogEvent("uvd check: SRBM_SOFT_RESET %x -> %x", srbm, gpuMmio[SrbmSoftReset]);
	bool alive = uvdResponds();
	diskLogEvent("uvd check: responds %d", alive);
	if (!alive) {
		auto on = smuSendMessage(UvdPowerOn, 0);
		auto dpm = smuSendMessage(EnableAllSmuFeatures, UvdDpmMask);
		IODelay(1000);
		alive = uvdResponds();
		diskLogEvent("uvd check: UVDPowerON(0) again %x, DPM %x -> responds %d", on, dpm, alive);
		uvdRegs("retry 1");
	}
	if (!alive) {
		auto off = smuSendMessage(UvdPowerOff, 0);
		IODelay(1000);
		auto on = smuSendMessage(UvdPowerOn, 1);
		auto dpm = smuSendMessage(EnableAllSmuFeatures, UvdDpmMask);
		IODelay(1000);
		alive = uvdResponds();
		diskLogEvent("uvd check: UVDPowerOFF %x, UVDPowerON(1) %x, DPM %x -> responds %d", off, on, dpm, alive);
		uvdRegs("retry 2");
	}
	uvdState = alive ? 1 : 2;
	traceNote("uvdcheck=%u ", uvdState);
	return alive;
}

// With the tiles up (e004346) UVD answered, and nvram12 shows what came next: a session ran, decode timed out, the GPU
// restart (uc.resetHW) called AMDUVDHWEngine::start again and that never returned (watchdog panic, 57 s open); UVD was
// left with its VCPU in reset (SOFT_RESET 80008, VCPU_CNTL ff20000: no CLK_EN), i.e. the firmware never booted. Linux's
// uvd_v6_0_start waits for UVD_STATUS bit 1 (VCPU report) after releasing the VCPU. So after Apple's start, wait up to
// 100 ms for that bit; without it fail the start, refuse sessions from then on, and fail later starts (the restart's)
// without calling Apple's, which is what hung. The registers go into the report: a frozen boot loses its disk log.
static bool wrapUvdStart(void *that) {
	auto &info = uvdStartInfo;
	info.calls++;
	if (uvdState == 3 && !checkKernelArgument("-crnouvdfwcheck")) {
		info.skipped++;
		traceNote("uvd.start=skipped ");
		return false;
	}
	if (gpuMmio && videoPowered())
		uvdPowerTilesUp();
	bool ret = FunctionCast(wrapUvdStart, orgUvdStart)(that);
	if (!gpuMmio)
		return ret;
	uint32_t status = gpuMmio[0x3DAF];
	for (uint32_t i = 0; i < 100 && ret && !(status & 2); i++) {
		IODelay(1000);
		status = gpuMmio[0x3DAF];
	}
	info.ret = ret;
	info.status = status;
	info.vcpu = gpuMmio[0x3D98];
	info.reset = gpuMmio[0x3DA0];
	info.lmi = gpuMmio[0x3D67];
	info.tile = gpuMmio[UvdPgfsmTile1];
	info.ms = uptimeMs();
	diskLogEvent("uvd start #%u = %d: status %x vcpu %x reset %x lmi %x tile1 %x", info.calls, ret, status, info.vcpu,
		info.reset, info.lmi, info.tile);
	traceNote("uvd.start=%d/%x ", ret, status);
	if (ret && !(status & 2) && !checkKernelArgument("-crnouvdfwcheck")) {
		uvdState = 3;
		diskLogEvent("uvd start: firmware didn't report (UVD_STATUS bit 1 clear), sessions refused");
		return false;
	}
	return ret;
}

// With the firmware running, H.264 decode still hung (nvram16: GPU restart 7 s into the first session, then resetHW
// never finished), and the report only had UVD's state after the restart. Linux runs Stoney's UVD in physical mode (the
// kernel rewrites every buffer address in a decode command to a memory-controller one) and only Polaris and later in VM
// mode, which is what Apple's driver uses. So every 500 ms while the firmware runs (UVD_STATUS bit 1; a restart stops
// it, keeping the last snapshot from before) record the ring and IB address registers and VMIDs, the VCPU command
// registers, and the ring's first 32 dwords (its first IB packets), for the report.
static struct {
	uint64_t ms;
	uint32_t samples, status, vcpu, rptr, wptr, rbCntl, rbLo, rbHi, rbVmid, ibLo, ibHi, ibVmid, ibSize, lmi, cmd, data0,
		data1, ctxId, semaTimeout, ring[32];
	bool ringRead;
} uvdLive;

static void uvdLiveTick() {
	auto *m = gpuMmio;
	if (uvdState != 1 || !m || !(m[0x3DAF] & 2))
		return;
	auto &s = uvdLive;
	uint32_t wptr = m[0x3DA5];
	bool wptrMoved = wptr != s.wptr || !s.samples;
	s.ms = uptimeMs();
	s.samples++;
	s.status = m[0x3DAF];
	s.vcpu = m[0x3D98];
	s.rptr = m[0x3DA4];
	s.wptr = wptr;
	s.rbCntl = m[0x3DA9];
	s.rbLo = m[0x3C69];
	s.rbHi = m[0x3C68];
	s.rbVmid = m[0x3DA3];
	s.ibLo = m[0x3C67];
	s.ibHi = m[0x3C66];
	s.ibVmid = m[0x3DA1];
	s.ibSize = m[0x3DA2];
	s.lmi = m[0x3D67];
	s.cmd = m[0x3BC3];
	s.data0 = m[0x3BC4];
	s.data1 = m[0x3BC5];
	s.ctxId = m[0x3DBD];
	s.semaTimeout = m[0x3DB0];
	if (!wptrMoved || s.rbVmid)
		return;
	uint64_t phys = ibMcToPhys(static_cast<uint64_t>(s.rbHi) << 32 | s.rbLo, false);
	if (!physRangeOk(phys, sizeof(s.ring)))
		return;
	auto *desc = IOMemoryDescriptor::withPhysicalAddress(phys, sizeof(s.ring), kIODirectionIn);
	auto *map = desc ? desc->map(kIOMapInhibitCache) : nullptr;
	if (map) {
		memcpy(s.ring, reinterpret_cast<const void *>(map->getVirtualAddress()), sizeof(s.ring));
		s.ringRead = true;
	}
	OSSafeReleaseNULL(map);
	OSSafeReleaseNULL(desc);
}

// Why the VCPU never started (nvram14: UVD_STATUS 0, VCPU_CNTL ff20000 without CLK_EN): CAIL's tonga_init_uvd_engine
// has two paths. With the power gating flags UVD (0x20) and UVD dynamic (0x4000) both set it takes the DPG one: the
// register values go to the DPG staging memory (Tonga_set_reg_in_uvd_dpg_mode, VCPU_CNTL ff20200) for the power
// management to apply when it powers UVD on demand, which nothing does here. The same two flags make
// stoney_power_down_uvd_block_for_dpg_mode power the tiles down at boot. Otherwise it is Linux's uvd_v6_0_start:
// registers written directly, VCPU released, then a 3 s wait for UVD_STATUS bit 1. The flags come from the registry key
// DisableUVDPowerGating (sets both disable bits, cail+0x6F8), which CAIL reads from the PCI device, where
// AMDHardware::setupCAIL has addPlistPropertiesToPCIDevice copy the accelerator's cail_properties; the UVD ring reads
// the same key there to pick the plain ring registers over the DPG ones. So add CAIL_DisableUVDPowerGating = 1 to
// cail_properties before the copy (13-15). Linux doesn't power-gate Stoney's UVD this way either. -crnouvdpgoff leaves it.
static bool uvdPgOff;

static uint64_t wrapAddPlistPropertiesToPCIDevice(IOService *that, const char *name) {
	if (that && name && !strcmp(name, "cail_properties") && !checkKernelArgument("-crnouvdpgoff")) {
		auto *dict = OSDynamicCast(OSDictionary, that->getProperty(name));
		auto *copy = dict ? OSDictionary::withDictionary(dict) : OSDictionary::withCapacity(1);
		auto *one = OSNumber::withNumber(1ULL, 32);
		if (copy && one) {
			copy->setObject("CAIL_DisableUVDPowerGating", one);
			that->setProperty(name, copy);
			uvdPgOff = true;
		}
		OSSafeReleaseNULL(one);
		OSSafeReleaseNULL(copy);
		traceNote("uvdpg=%s ", uvdPgOff ? "off" : "failed");
	}
	return FunctionCast(wrapAddPlistPropertiesToPCIDevice, orgAddPlistPropertiesToPCIDevice)(that, name);
}

// Encode: the screen recording's VCE session hung the same way ("VCELLQ event timeout"; VCE_SOFT_RESET still 1 after
// Apple's start). Decode comes first, so VCE contexts are refused (software encode) unless -crvce, and even then only
// with ECLK running.
static bool wrapVceContextInit(void *that, void *accel, void *videoContext, uint32_t *createInfo) {
	static uint32_t refused;
	if (!checkKernelArgument("-crvce") || !gpuMmio || !videoPowered() || !eclkRunning()) {
		// newContext calls AMDAccelVCEContext::free when init fails, and free reads the accelerator from this+0x50
		// without a check (13-15); init sets it only after its super init, so a refused context faulted (nvram11).
		auto &ctxAccel = *reinterpret_cast<void **>(static_cast<uint8_t *>(that) + 0x50);
		if (!ctxAccel)
			ctxAccel = accel;
		if (!refused++)
			diskLogEvent("vce init refused (eclk cntl %x status %x)", gpuMmio ? smcRead(videoClocks[2].cntl) : 0,
				gpuMmio ? smcRead(videoClocks[2].status) : 0);
		traceNote("vid.vceInit=refused ");
		return false;
	}
	bool ret = FunctionCast(wrapVceContextInit, orgVceContextInit)(that, accel, videoContext, createInfo);
	traceNote("vid.vceInit=%d ", ret);
	return ret;
}

// The SMU's graphics clock table, read once (nvram180: the current/target DPM level never left 0 and Apple's
// PowerPlay runs its dummy back end, so nobody started SCLK DPM). What Linux's smu8_download_pptable_settings does:
// SetClkTableAddrHi/Lo, ExecuteJob(the clock table job), ClkTableXferToDram; then the SclkBreakdownTable
// (SMU8_Fusion_ClkTable's first 116 bytes: 8 levels and the DPM-off level, 12 bytes each - GnbVid, GfxVid, DfsDid,
// DeepSleepDid, DfsBypass, Frequency in 10 kHz - then SclkValidMask and MaxSclkIndex). Also GetMaxSclkLevel. Nothing
// is written to the SMU's state. -crnoclkread disables it.
static struct {
	bool done;
	uint32_t resp[5], maxLevel;
	uint8_t table[116];
} smuClk;

static void smuReadClockTable() {
	using namespace Smu8;
	if (smuClk.done || !smuLoadOk() || smuClkTableJob == 0xFFFF || checkKernelArgument("-crnoclkread"))
		return;
	smuClk.done = true;
	uint64_t mc = smuLoad.mc + smuClkTableAt, phys = smuLoad.phys + smuClkTableAt;
	smuClk.resp[0] = smuSendMessage(GetMaxSclkLevel, 0);
	smuClk.maxLevel = gpuMmio[ARG];
	smuClk.resp[1] = smuSendMessage(SetClkTableAddrHi, static_cast<uint32_t>(mc >> 32));
	smuClk.resp[2] = smuSendMessage(SetClkTableAddrLo, static_cast<uint32_t>(mc));
	smuClk.resp[3] = smuSendMessage(ExecuteJob, smuClkTableJob);
	smuClk.resp[4] = smuSendMessage(ClkTableXferToDram, 0);
	auto *desc = IOMemoryDescriptor::withPhysicalAddress(phys, sizeof(smuClk.table), kIODirectionIn);
	auto *map = desc ? desc->map(kIOMapInhibitCache) : nullptr;
	if (map)
		memcpy(smuClk.table, reinterpret_cast<const void *>(map->getVirtualAddress()), sizeof(smuClk.table));
	OSSafeReleaseNULL(map);
	OSSafeReleaseNULL(desc);
	traceNote("smuclk=%x ", smuClk.resp[4]);
	for (uint32_t level = 0; level < 9; level++) {
		uint32_t freq = 0;
		memcpy(&freq, smuClk.table + level * 12 + 8, 4);
		diskLogEvent("smu clock table %s%u: %u MHz did %x gfxvid %x gnbvid %x", level == 8 ? "off level" : "L",
			level == 8 ? 0 : level, freq / 100, smuClk.table[level * 12 + 2], smuClk.table[level * 12 + 1],
			smuClk.table[level * 12]);
	}
}

// SCLK DPM, opt-in (-crsclkdpm): what Linux's smu8_enable_dpm_tasks and smu8_update_sclk_limit send, once the clock
// table has been read and holds every level (nvram181: 200-900 MHz, as in the VBIOS). Voting clients
// (CG_FREQ_TRAN_VOTING_0), EnableAllSmuFeatures(SCLK_DPM_MASK), then hard min crsclkmin (default level 2, 400 MHz:
// the clock the firmware boots with), soft min the same, soft max crsclkmax (default 6). crsclkmin = crsclkmax pins one
// level: nvram183-203 froze on 5 of 11 DPM boots, so pinning separates the switching from the clock itself.
// GetFeatureStatus before and after (SclkDpmOn 0x200000).
static struct {
	bool done;
	uint32_t featuresBefore, featuresAfter, resp[7], hardMin, softMax;
} smuDpm;


static void smuEnableSclkDpm() {
	using namespace Smu8;
	if (smuDpm.done || !smuClk.done || checkKernelArgument("-crnosclkdpm"))
		return;
	smuDpm.done = true;
	uint32_t top = smuClk.maxLevel;
	bool tableOk = smuClk.resp[4] == 1 && top >= 1 && top < 8;
	for (uint32_t level = 0; level <= top && tableOk; level++) {
		uint32_t freq = 0;
		memcpy(&freq, smuClk.table + level * 12 + 8, 4);
		tableOk = freq >= 10000 && freq <= 120000;  // 100-1200 MHz
	}
	if (!tableOk) {
		smuDpm.resp[0] = 0xDEAD;
		return;
	}
	// Ceiling: crsclkmax, default the top level the SMU reports (900 MHz on the D509BA's A9-9425, 800 MHz on an
	// A9-9420). The "DPM freezes" were the IH storm breaker (disk log 1792383022), not the clock.
	uint32_t ceiling = top;
	PE_parse_boot_argn("crsclkmax", &ceiling, sizeof(ceiling));
	if (ceiling < top)
		top = ceiling;
	smuDpm.softMax = top;
	uint32_t hardMin = 2;
	PE_parse_boot_argn("crsclkmin", &hardMin, sizeof(hardMin));
	smuDpm.hardMin = hardMin = hardMin > top ? top : hardMin;
	smuSendMessage(GetFeatureStatus, 0);
	smuDpm.featuresBefore = gpuMmio[ARG];
	gpuMmio[0x19A] = CgFreqTranVoting0;
	gpuMmio[0x19B] = VotingRightsDefault;
	smuDpm.resp[0] = smuSendMessage(EnableAllSmuFeatures, SclkDpmMask);
	smuDpm.resp[1] = smuSendMessage(SetSclkHardMin, hardMin);
	smuDpm.resp[2] = smuSendMessage(SetSclkSoftMin, hardMin);  // = hard min: crsclkmin=crsclkmax pins one level
	smuDpm.resp[3] = smuSendMessage(SetSclkSoftMax, top);
	// smu8_set_power_state_tasks also sets the deep-sleep floor (SMU8_MIN_DEEP_SLEEP_SCLK, 8 MHz, when the display
	// gives none) and the watermark frequency (the soft max clock, 10 kHz units).
	uint32_t topFreq = 0;
	memcpy(&topFreq, smuClk.table + top * 12 + 8, 4);
	smuDpm.resp[5] = smuSendMessage(SetMinDeepSleepSclk, 800);
	smuDpm.resp[6] = smuSendMessage(SetWatermarkFrequency, topFreq);
	smuDpm.resp[4] = smuSendMessage(GetFeatureStatus, 0);
	smuDpm.featuresAfter = gpuMmio[ARG];
	traceNote("sclkdpm=%x/%x ", smuDpm.resp[0], smuDpm.featuresAfter);
	diskLogEvent("sclk dpm enabled: L%u-L%u, features %x -> %x, resp %x %x %x %x %x %x %x", smuDpm.hardMin,
		smuDpm.softMax, smuDpm.featuresBefore, smuDpm.featuresAfter, smuDpm.resp[0], smuDpm.resp[1], smuDpm.resp[2],
		smuDpm.resp[3], smuDpm.resp[4], smuDpm.resp[5], smuDpm.resp[6]);
}


// After the SMU lost power: our load, SCLK DPM and the video power-up are done again (the load now, the others by
// their usual triggers).
static void smuResetAfterPowerLoss() {
	smuLoad = {};
	smuDpm.done = false;
	smuVideo.done = false;
}

static bool sclkDpmRunning() {
	return smuDpm.done && smuDpm.resp[0] == 1;
}

static const char *sclkDpmState() {
	if (!smuDpm.done)
		return checkKernelArgument("-crnosclkdpm") ? "off" : "pending";
	return smuDpm.resp[0] == 1 ? "on" : "failed";
}

static uint64_t wrapStoneyLoadRlcUcode(void *cail) {
	smuLoadFirmware(cail);
	auto ret = FunctionCast(wrapStoneyLoadRlcUcode, orgStoneyLoadRlcUcode)(cail);
	if (smuLoad.result)
		smuStep(7);  // CAIL has started the RLC, now with real microcode
	return ret;
}

// Metal device creation. The user-space Metal driver (AMDMTLBronzeDriver) asks AMDAccelDevice::getHardwareInfo for
// the GPU's description, and it fails (0xE00002BC) if refClkFreq, sysClkFreq, memClkFreq or the fourth clock (+0xC0..
// +0xD8: AMDHardware::initHWInfo takes them from the VBIOS firmware info, then PowerPlay's engine/memory clocks) or the
// device ID / raster / shader pipe counts are zero. nvram102: 7 processes opened the accelerator, none ever created a
// command queue, and WindowServer never flipped. On the APU the VBIOS has no memory clock and the Dummy PowerPlay back
// end reports none either. Record what it returns, and fill zero clocks with Stoney's (10 kHz units, as the VBIOS):
// 100 MHz reference, 847 MHz engine, 1066 MHz memory (DDR4-2133). -crnohwinfofix only records.
static mach_vm_address_t orgGetHardwareInfo;
static uint32_t hwInfoCalls, hwInfoFixed, hwInfoSpoofed, hwInfoRet = 0xFFFFFFFF;
static uint64_t hwInfoValues[7];  // ref, sys, mem, +0xD8 clocks; device ID, raster pipes, shader pipes (as returned)
static uint32_t hwInfoCopy[0x204 / 4];  // the whole block as returned (after our changes), for the report

static uint32_t wrapGetHardwareInfo(void *that, uint8_t *info) {
	auto ret = FunctionCast(wrapGetHardwareInfo, orgGetHardwareInfo)(that, info);
	hwInfoCalls++;
	if (!info)
		return ret;
	auto *clocks = reinterpret_cast<uint64_t *>(info + 0xC0);
	for (int i = 0; i < 4; i++)
		hwInfoValues[i] = clocks[i];
	hwInfoValues[4] = *reinterpret_cast<uint16_t *>(info);
	hwInfoValues[5] = *reinterpret_cast<uint64_t *>(info + 0x18);
	hwInfoValues[6] = *reinterpret_cast<uint64_t *>(info + 0x20);
	hwInfoRet = ret;
	memcpy(hwInfoCopy, info, sizeof(hwInfoCopy));
	// nvram103: the call succeeds (clocks 48/933/400 MHz), but reports device ID 0x98E4: AMDHardware reads it from
	// config space, so the Polaris Metal driver sees an ID it doesn't know while everything else sees the spoofed one.
	// Report the spoofed ID (the iGPU's device-id property). -crnohwinfospoof leaves the real one.
	if (vbiosOwner && !checkKernelArgument("-crnohwinfospoof"))
		if (auto *prop = OSDynamicCast(OSData, vbiosOwner->getProperty("device-id")))
			if (prop->getLength() >= 2) {
				uint16_t spoof = *static_cast<const uint16_t *>(prop->getBytesNoCopy());
				auto &device = *reinterpret_cast<uint16_t *>(info);
				if (spoof && device != spoof) {
					device = spoof;
					hwInfoSpoofed++;
					// +2 is config word 8 (revision ID, class prog-if): take a spoofed revision-id too, if there is one.
					if (auto *rev = OSDynamicCast(OSData, vbiosOwner->getProperty("revision-id")))
						if (rev->getLength() >= 1)
							info[2] = *static_cast<const uint8_t *>(rev->getBytesNoCopy());
				}
			}
	if (ret == 0xE00002BC && hwInfoValues[4] && hwInfoValues[5] && hwInfoValues[6] &&
		!checkKernelArgument("-crnohwinfofix")) {
		static const uint64_t stoney[4] {10000, 84700, 106600, 84700};
		for (int i = 0; i < 4; i++)
			if (!clocks[i])
				clocks[i] = stoney[i];
		hwInfoFixed++;
		ret = 0;
	}
	return ret;
}

// Graphics power gating. CAIL's Stoney start-up ends with Cail_Stoney_EnablePowerGating: SCLK slow-down around power
// changes, CP power gating (Stoney_EnableCpPowerGating) and GFX power gating (Stoney_UpdateGfxPowerGating), leaving
// RLC_PG_CNTL at 0x60015. Powering the GFX block down and back up needs the SMU's save/restore jobs, which Linux's
// SMU8 power manager drives and Apple's Dummy PowerPlay doesn't. nvram118: after ~100 s of desktop use the CP hung in
// the context's CLEAR_STATE / PREAMBLE_CNTL (GRBM: only CP/CPF busy, no faults), Apple reset the GPU, and flips
// stopped. Keep it all off: the three are no-ops and RLC_PG_CNTL stays CP_PG_DISABLE (0x8000). -crgating keeps them.
static mach_vm_address_t orgCailStoneyEnablePg, orgStoneyUpdateGfxPg, orgStoneyEnableCpPg;
static uint32_t pgBlocked;

static void pgOff() {
	pgBlocked++;
	if (auto *mmio = gpuMmio)
		mmio[0xEC43] = 0x8000;  // RLC_PG_CNTL: CP_PG_DISABLE, nothing enabled
}

static uint64_t wrapCailStoneyEnablePg(void *) {
	pgOff();
	traceNote("pg=off ");
	return 0;
}

static uint64_t wrapStoneyUpdateGfxPg(void *, uint64_t) {
	pgOff();
	return 0;
}

static uint64_t wrapStoneyEnableCpPg(void *, uint64_t) {
	pgOff();
	return 0;
}

// Which of the HWLibs hooks below were installed (bit per hook, in HwlibsHook order) and which failed. nvram72 showed
// none of them taking effect (no PowerPlay key notes, flags untouched, ATS not skipped) with no "noroute" note.
enum HwlibsHook {
	HookPeciRead, HookPeciSize, HookWaitFor, HookAts, HookHaltRlc, HookFlags, HookSmuFormat, HookSmuLoad, HookEarlyInit,
	HookPgEnable, HookPgGfx, HookPgCp
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
// Where HWLibs was patched, and the live copy's earlyInitialize (from the CAIL object AMDHardware+0x338 that setupCAIL
// calls through vtable +0x10). If they differ, re-apply the HWLibs patches at the live copy's address.
static KernelPatcher *hwlibsPatcher;
static size_t hwlibsIndex, hwlibsSize;
static mach_vm_address_t hwlibsSlide, hwlibsEarlyInit, hwlibsLiveEarlyInit;
static uint32_t hwlibsRepatches, hwlibsLiveChecks;
static IOMemoryDescriptor *hwlibsWired;
static IOReturn hwlibsWireResult = kIOReturnNotReady;
static uint8_t hwlibsAfterBytes[8];  // earlyInitialize's first bytes right after re-patching
// Physical page behind earlyInitialize when we patched it and at setupCAIL (pmap_find_phys on the kernel pmap, solved at
// run time), and a test write: did a byte written there read back? (nvram133: re-patch reported success but the bytes
// read back unchanged.)
static mach_vm_address_t kernelPmapFindPhys, kernelPmap;
static uint64_t hwlibsPhysAtPatch, hwlibsPhysAtCheck;
static int hwlibsProbeWrite = -1;  // -1 not tried; else the byte read back after writing 0xCC

static uint64_t physPageOf(mach_vm_address_t va) {
	if (!kernelPmapFindPhys || !kernelPmap)
		return 0;
	auto pmap = *reinterpret_cast<void **>(kernelPmap);
	auto ppn = reinterpret_cast<uint32_t (*)(void *, uint64_t)>(kernelPmapFindPhys)(pmap, va);
	return static_cast<uint64_t>(ppn) << 12;
}
static uint8_t hwlibsEntryBytes[8];  // earlyInitialize's first bytes at setupCAIL (a Lilu route starts with a jump)
static bool hwlibsUnpatched;
static void (*hwlibsRepatch)(KernelPatcher &, size_t, mach_vm_address_t, size_t);

static void checkLiveHwlibs(void *hardware) {
	hwlibsLiveChecks++;
	if (!hardware || !hwlibsEarlyInit)
		return;
	auto *cail = *reinterpret_cast<uint8_t **>(static_cast<uint8_t *>(hardware) + 0x338);
	if (!cail)
		return;
	auto *vtable = *reinterpret_cast<mach_vm_address_t **>(cail);
	if (!vtable)
		return;
	hwlibsLiveEarlyInit = vtable[2];
	memcpy(hwlibsEntryBytes, reinterpret_cast<const void *>(hwlibsLiveEarlyInit), sizeof(hwlibsEntryBytes));
	hwlibsPhysAtCheck = physPageOf(hwlibsLiveEarlyInit);
	// Probe: write 0xCC over the first byte with write protection off, read it back, restore it. Nothing runs this
	// function concurrently: setupCAIL (this thread) calls it next.
	if (hwlibsLiveEarlyInit == hwlibsEarlyInit && !checkKernelArgument("-crnohwlibsprobe")) {
		auto *entry = reinterpret_cast<volatile uint8_t *>(hwlibsLiveEarlyInit);
		uint8_t original = entry[0];
		if (MachInfo::setKernelWriting(true, nullptr) == KERN_SUCCESS) {
			entry[0] = 0xCC;
			hwlibsProbeWrite = entry[0];
			entry[0] = original;
			MachInfo::setKernelWriting(false, nullptr);
		} else {
			hwlibsProbeWrite = 0x100;
		}
	}
	// Still Apple's prologue (push rbp; mov rbp, rsp) where our route's jump should be: the patches are gone. Apply
	// them again at the same address.
	if (hwlibsLiveEarlyInit == hwlibsEarlyInit) {
		hwlibsUnpatched = hwlibsEntryBytes[0] == 0x55 && hwlibsEntryBytes[1] == 0x48;
		if (hwlibsUnpatched && hwlibsPatcher && hwlibsRepatch && !checkKernelArgument("-crnohwlibsrepatch")) {
			hwlibsRepatches++;
			traceNote("hwlibs=repatch ");
			hwlibsRepatch(*hwlibsPatcher, hwlibsIndex, hwlibsSlide, hwlibsSize);
			memcpy(hwlibsAfterBytes, reinterpret_cast<const void *>(hwlibsLiveEarlyInit), sizeof(hwlibsAfterBytes));
		}
		return;
	}
	if (!hwlibsPatcher || !hwlibsRepatch || checkKernelArgument("-crnohwlibsrepatch"))
		return;
	// Same image elsewhere: a page-aligned offset, and a normal prologue (push rbp; mov rbp, rsp) at the live entry.
	auto *entry = reinterpret_cast<const uint8_t *>(hwlibsLiveEarlyInit);
	if (((hwlibsLiveEarlyInit - hwlibsEarlyInit) & 0xFFF) || entry[0] != 0x55 || entry[1] != 0x48 || entry[2] != 0x89 ||
		entry[3] != 0xE5) {
		traceNote("hwlibs=live?%llx ", static_cast<uint64_t>(hwlibsLiveEarlyInit));
		return;
	}
	mach_vm_address_t liveSlide = hwlibsSlide + (hwlibsLiveEarlyInit - hwlibsEarlyInit);
	hwlibsRepatches++;
	traceNote("hwlibs=live@%llx ", static_cast<uint64_t>(liveSlide));
	hwlibsPatcher->updateRunningInfo(hwlibsIndex, liveSlide, hwlibsSize, true);
	hwlibsSlide = liveSlide;
	hwlibsRepatch(*hwlibsPatcher, hwlibsIndex, liveSlide, hwlibsSize);
}

static void restoreVbiosImageForSetup() {
	binImageSetupChecks++;
	if (vbiosImage && vbiosOwner && !vbiosOwner->getProperty(VBIOS::ImageProperty)) {
		vbiosOwner->setProperty(VBIOS::ImageProperty, vbiosImage);
		binImageSetupMissing++;
		traceNote("binimage@setupCAIL=restored ");
	}
}

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
	if (kext == KextRadeonX4000) {
		flipProcessIrq = patcher.solveSymbol(index,
			"__ZN34AMDRadeonX4000_AMDAccelDisplayPipe27processTransactionInterruptEP8OSObjectPv", slide, size);
		patcher.clearError();
		KernelPatcher::RouteRequest hwInfo {"__ZN29AMDRadeonX4000_AMDAccelDevice15getHardwareInfoEP24_sAMD_GET_HW_INFO_VALUES",
			wrapGetHardwareInfo, orgGetHardwareInfo};
		if (!routeChecked(patcher, index, hwInfo, slide, size)) {
			traceNote("noroute:getHardwareInfo(%d) ", patcher.getError());
			patcher.clearError();
		}
	}
	if (kext == KextRadeonX4000HWLibs) {
		// Wire the image before patching: nvram129 had the CAIL object call exactly the patched earlyInitialize, yet
		// no HWLibs hook ever ran, as if its code pages had been dropped and re-read from the kext collection (losing
		// every patch). Locked pages can't be.
		// Only the executable segment (nvram130: wiring the whole image failed, it includes __LINKEDIT, which the
		// kernel discards after linking).
		// The executable segment: wired below, and the range hwsigFind searches (a kext's range in a kernel collection
		// isn't one mapping, so the whole of it can't be read).
		{
			mach_vm_address_t text = 0;
			uint64_t textSize = 0;
			auto *header = reinterpret_cast<const mach_header_64 *>(slide);
			if (header->magic == MH_MAGIC_64) {
				// Segment addresses relative to __TEXT (which starts at the header, i.e. at slide): the header's own
				// vmaddr values may be unslid in a kernel collection.
				uint64_t textBase = 0;
				bool haveBase = false;
				auto *cmd = reinterpret_cast<const load_command *>(header + 1);
				for (uint32_t i = 0; i < header->ncmds; i++) {
					if (cmd->cmd == LC_SEGMENT_64) {
						auto *seg = reinterpret_cast<const segment_command_64 *>(cmd);
						if (!haveBase && !strncmp(seg->segname, "__TEXT", sizeof(seg->segname))) {
							textBase = seg->vmaddr;
							haveBase = true;
						}
						if (haveBase && !strncmp(seg->segname, "__TEXT_EXEC", sizeof(seg->segname)) && seg->vmsize &&
							seg->vmaddr >= textBase && seg->vmaddr - textBase + seg->vmsize <= size && !text) {
							text = slide + (seg->vmaddr - textBase);
							textSize = seg->vmsize;
						}
						// The code itself: the __text section, in __TEXT_EXEC in Sequoia's kernel collection but in
						// __TEXT on Ventura (Ventura nvram3: no __TEXT_EXEC, "text 0+0", so no signature was searched
						// and the SMU load never ran). It is also what the signatures' occurrences count over.
						auto *sect = reinterpret_cast<const section_64 *>(seg + 1);
						for (uint32_t k = 0; haveBase && k < seg->nsects; k++, sect++) {
							if (!strncmp(sect->sectname, "__text", sizeof(sect->sectname)) && sect->size &&
								sect->addr >= textBase && sect->addr - textBase + sect->size <= size) {
								text = slide + (sect->addr - textBase);
								textSize = sect->size;
							}
						}
					}
					cmd = reinterpret_cast<const load_command *>(reinterpret_cast<const uint8_t *>(cmd) + cmd->cmdsize);
				}
			}
			hwlibsTextStart = text;
			hwlibsTextSize = textSize;
		}
		if (!hwlibsWired && !checkKernelArgument("-crnohwlibswire")) {
			mach_vm_address_t text = hwlibsTextStart;
			uint64_t textSize = hwlibsTextSize;
			if (text && textSize) {
				// Touch every page first, then wire.
				volatile uint8_t sink = 0;
				for (uint64_t off = 0; off < textSize; off += PAGE_SIZE)
					sink = sink + *reinterpret_cast<volatile const uint8_t *>(text + off);
				(void)sink;
				// Read-only first: the code is mapped r-x, and asking for write access (InOut) fails on it since the range is
				// the __text section (Sequoia and Ventura logs: hwlibs=notwired). Wiring only needs the pages resident.
				static const IODirection directions[] {kIODirectionOut, kIODirectionInOut};
				for (auto direction : directions) {
					hwlibsWired = IOMemoryDescriptor::withAddressRange(text, textSize, direction, kernel_task);
					hwlibsWireResult = hwlibsWired ? hwlibsWired->prepare() : kIOReturnNoMemory;
					if (hwlibsWireResult == kIOReturnSuccess)
						break;
					OSSafeReleaseNULL(hwlibsWired);
				}
			}
			traceNote("hwlibs=%s ", hwlibsWired ? "wired" : "notwired");
		}
		hwlibsPatcher = &patcher;
		hwlibsIndex = index;
		hwlibsSize = size;
		hwlibsSlide = slide;
		hwlibsEarlyInit = solveOrSignature(patcher, index,
			"__ZN15AmdCailServices15earlyInitializeEP23_MCIL_SERVICE_CALLBACKSP15_CailInitialize", slide, size);
		patcher.clearError();
		if (!kernelPmapFindPhys) {
			kernelPmapFindPhys = patcher.solveSymbol(KernelPatcher::KernelID, "_pmap_find_phys");
			kernelPmap = patcher.solveSymbol(KernelPatcher::KernelID, "_kernel_pmap");
			patcher.clearError();
		}
		if (hwlibsEarlyInit && !hwlibsPhysAtPatch)
			hwlibsPhysAtPatch = physPageOf(hwlibsEarlyInit);
		if (!hwlibsRepatch)
			hwlibsRepatch = [](KernelPatcher &p, size_t i, mach_vm_address_t sl, size_t sz) {
				callback->traceKext(p, i, sl, sz, KextRadeonX4000HWLibs);
			};
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
		if (family == ChipFamily::Stoney && !checkKernelArgument("-crgating")) {
			KernelPatcher::RouteRequest pg[] {
				{"_Cail_Stoney_EnablePowerGating", wrapCailStoneyEnablePg, orgCailStoneyEnablePg},
				{"_Stoney_UpdateGfxPowerGating", wrapStoneyUpdateGfxPg, orgStoneyUpdateGfxPg},
				{"_Stoney_EnableCpPowerGating", wrapStoneyEnableCpPg, orgStoneyEnableCpPg},
			};
			static const HwlibsHook pgHooks[] {HookPgEnable, HookPgGfx, HookPgCp};
			for (size_t i = 0; i < arrsize(pg); i++)
				routeHwlibs(patcher, index, pg[i], slide, size, pgHooks[i]);
		}
		if ((family == ChipFamily::Stoney || family == ChipFamily::Carrizo) && !checkKernelArgument("-crats")) {
			KernelPatcher::RouteRequest ats {"_Cail_Spectre_EnableAts", wrapCailEnableAts, orgCailEnableAts};
			routeHwlibs(patcher, index, ats, slide, size, HookAts);
		}
		if ((family == ChipFamily::Stoney || family == ChipFamily::Carrizo) && !checkKernelArgument("-crnodirectucode")) {
			// The flags word store, and the RLC loader's test of the flag (test byte [rbx+0x6F2], 0x20). Catalina's CAIL
			// has both fields 8 bytes earlier (mov [r12+0x6E8], eax; test byte [r12+0x6EA], 0x20; the SMU buffer size
			// stored at +0xCD0); its Carrizo code isn't checked, so it stays off there.
			selectAccelLayouts();
			bool catalina = getKernelVersion() == KernelVersion::Catalina;
			auto fn = solveOrSignature(patcher, index, "_CailReadinOverrideRegistrySetting", slide, size);
			bool carrizo = family == ChipFamily::Carrizo;
			const char *loadRlcName = carrizo ? "_Carrizo_LoadRlcUcode" : "_Stoney_LoadRlcUcode";
			const char *formatName = carrizo ? "_Carrizo_FormatSmuDramDataBuffer" : "_Stoney_FormatSmuDramDataBuffer";
			auto rlc = solveOrSignature(patcher, index, loadRlcName, slide, size);
			patcher.clearError();
			KernelPatcher::RouteRequest request {"_CailReadinOverrideRegistrySetting",
				wrapCailReadinOverrideRegistrySetting, orgCailReadinOverrideRegistrySetting};
			if (!checkKernelArgument("-crnorlcreset")) {
				const char *haltName = carrizo ? "_carrizo_halt_rlc" : "_stoney_halt_rlc";
				KernelPatcher::RouteRequest halt {haltName, wrapStoneyHaltRlc, orgStoneyHaltRlc};
				auto haltFn = solveOrSignature(patcher, index, haltName, slide, size);
				patcher.clearError();
				// RLC_CNTL = 0 (mov esi, 0xEC00), then the serdes idle wait.
				if (!codeHas(haltFn, 0x20, "be00ec0000"))
					traceNote("rlcreset=nomatch ");
				else
					routeHwlibs(patcher, index, halt, slide, size, HookHaltRlc);
			}
			// Stoney's RLC loader tests DisableCailLoadUcode ([cail+0x6F2] bit 0x20) near its start; Carrizo's has a
			// different flow (the KDK's Carrizo_LoadRlcUcode doesn't test that bit), so only the registry reader is checked.
			if (!codeHas(fn, 0x100, catalina ? "41898424e8060000" : "8983f0060000|41898424f0060000") ||
				(!carrizo && !codeHas(rlc, 0x60, catalina ? "41f68424ea06000020" : "f683f206000020|41f68424f206000020")) ||
				!rlc || (catalina && carrizo)) {
				traceNote("directucode=nomatch ");
			} else if (routeHwlibs(patcher, index, request, slide, size, HookFlags) && !checkKernelArgument("-crnosmuload")) {
				// See smuLoadFirmware. The size function must end by storing its total at [cail+0xCD8]
				// (mov [rbx+0xCD8], eax), which the wrapper relies on.
				// The size function must store its total at [cail+0xCD8]: Stoney mov [rbx+0xCD8], eax; Carrizo
				// mov [rbx+0xCD8], r14d.
				auto info = patcher.solveSymbol(index, carrizo ? "_Carrizo_UcodeInfo" : "_Stoney_UcodeInfo", slide, size);
				patcher.clearError();
				auto format = solveOrSignature(patcher, index, formatName, slide, size);
				patcher.clearError();
				// Stripped builds: no symbol for the microcode table. The format function starts with
				// mov rbx, rdi; call GetGpuGoldenSettings; mov rax, [rax+0x10] (the table, as GoldenSettings+0x10
				// holds it in 15.8.1), so take the call's target and ask it in wrapStoneyFormatSmuDram.
				if (!info && format && codeHas(format, 0x12, "4889fbe8|4989fee8") && codeHas(format + 0x16, 4, "488b4010")) {
					int32_t rel;
					memcpy(&rel, reinterpret_cast<const void *>(format + 0x12), sizeof(rel));
					ucodeGoldenSettings = format + 0x16 + rel;
				}
				if ((!info && !ucodeGoldenSettings) ||
					!codeHas(format, 0x130, catalina ? "418986d00c0000" :
						carrizo ? "4489b3d80c0000|4589a6d80c0000" : "8983d80c0000|418986d80c0000")) {
					traceNote("smuload=nomatch ");
				} else {
					stoneyUcodeInfo = reinterpret_cast<Smu8::CailUcode **>(info);
					smuCarrizo = carrizo;
					PE_parse_boot_argn("crsmustep", &smuStepLimit, sizeof(smuStepLimit));
					KernelPatcher::RouteRequest smu[] {
						{formatName, wrapStoneyFormatSmuDram, orgStoneyFormatSmuDram},
						{loadRlcName, wrapStoneyLoadRlcUcode, orgStoneyLoadRlcUcode},
					};
					if (routeHwlibs(patcher, index, smu[0], slide, size, HookSmuFormat))
						routeHwlibs(patcher, index, smu[1], slide, size, HookSmuLoad);
				}
			}
		}
		if (!checkKernelArgument("-crnotrace"))
			routeStages(patcher, index, slide, size, InHWLibs);
		if (hwsigResolved || hwsigMissed)
			traceNote("hwsig=%u/%u ", hwsigResolved, hwsigMissed);  // HWLibs functions found by signature / not found
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
	SYSLOG("cr", "-crmacgpu: built-in and AAPL,slot-name set");
}

// The GPU's name in About This Mac and System Information: the iGPU's "model". With the Polaris device ID Apple's
// controller names it "AMD Radeon Polaris". Use the APU's own name for its graphics, from the CPU brand string
// ("AMD A9-9425 RADEON R5, 5 COMPUTE CORES 2C+3G" -> "AMD Radeon R5 Graphics"), or a "model" from DeviceProperties if
// one was given. Set at start and re-checked by the watcher, since the controller may set its own later.
// -crnomodel leaves it alone.
void CRDriver::setGpuModel() {
	static char model[64];
	static bool disabled;
	if (disabled || !iGPU)
		return;
	if (!model[0]) {
		if (checkKernelArgument("-crnomodel")) {
			disabled = true;
			return;
		}
		auto *given = OSDynamicCast(OSData, iGPU->getProperty("model"));
		if (given && given->getLength() > 1 && given->getLength() < sizeof(model) &&
			!strnstr(static_cast<char *>(const_cast<void *>(given->getBytesNoCopy())), "Polaris", given->getLength())) {
			lilu_os_strlcpy(model, static_cast<const char *>(given->getBytesNoCopy()), given->getLength());
		} else {
			uint32_t brand[13] {};
			for (uint32_t i = 0; i < 3; i++) {
				uint32_t a = 0x80000002 + i, b, c, d;
				asm volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
				brand[i * 4] = a;
				brand[i * 4 + 1] = b;
				brand[i * 4 + 2] = c;
				brand[i * 4 + 3] = d;
			}
			auto *name = reinterpret_cast<char *>(brand);
			auto *radeon = strnstr(name, "RADEON R", 48);
			if (radeon && radeon[8] >= '0' && radeon[8] <= '9')
				snprintf(model, sizeof(model), "AMD Radeon R%c Graphics", radeon[8]);
			else
				lilu_os_strlcpy(model, "AMD Radeon Graphics", sizeof(model));
		}
		SYSLOG("cr", "GPU model: %s", model);
	}
	auto *current = OSDynamicCast(OSData, iGPU->getProperty("model"));
	size_t len = strlen(model) + 1;
	if (!current || current->getLength() != len || memcmp(current->getBytesNoCopy(), model, len))
		iGPU->setProperty("model", model, static_cast<unsigned>(len));
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
	ihSetRingBufferLocation = patcher.solveSymbol(index, "__ZN22BaffinInterruptManager21setRingBufferLocationEP8SRAMInfo",
		slide, size);
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

// The IH ring. AMD9500Controller allocates it in system memory and gives the IH its physical address, which the GPU
// reached through the system aperture (MC addresses below the FB/GART map straight to RAM). relocateFb puts the FB at
// MC 0 for the accelerator, over that same range, so the IH's writes land in the carve-out or the GART instead
// (nvram91/92: IH WPTR 0x10/0x20, but the CPU saw an empty ring and a zero write-back; a ring at physical 0x26F20000 sat
// past the 512 MB FB, in the GART). Every handler call then found nothing, wrote RPTR 0, and the IH fired again: the
// storm that left the accelerator's submissions waiting forever. Move the ring into our VRAM region (after the SMU
// data) with Apple's own BaffinInterruptManager::setRingBufferLocation({cpu, gpu, size}), as the accelerator does on a
// discrete card; the CPU reads it through an uncached mapping of the carve-out. -crnoihmove disables it.
// A wake re-initialises the IH with Apple's original ring (Catalina, both wakes: RB_BASE 25c300 instead of ours, RPTR
// stuck at 0x20 while WPTR lapped the ring, then the storm breaker and fence time-outs), so the enable wrapper checks
// RB_BASE and moves the ring again (reapplyIhRing).
static IOMemoryMap *ihRingMap;
static const char *ihMoveResult;
static uint64_t ihMovedGpu;
static uint32_t ihRingSize, ihRingMoves;

static void reapplyIhRing(void *manager) {
	memset(reinterpret_cast<void *>(ihRingMap->getVirtualAddress()), 0, ihRingSize + 0x1000);
	struct {
		uint64_t cpu, gpu, size;
	} info {static_cast<uint64_t>(ihRingMap->getVirtualAddress()), ihMovedGpu, ihRingSize};
	reinterpret_cast<uint32_t (*)(void *, void *)>(ihSetRingBufferLocation)(manager, &info);
	ihRingMoves++;
}

static void moveIhRing(void *manager) {
	using namespace Smu8;
	if (ihMoveResult)
		return;
	if (!ihSetRingBufferLocation || checkKernelArgument("-crnoihmove")) {
		ihMoveResult = "off";
		return;
	}
	if (!smuLoad.result || !smuLoad.phys || !smuLoad.mc) {
		ihMoveResult = "no VRAM region";
		return;
	}
	uint32_t size = *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(manager) + 0x3C);
	if (!size || size > IhMaxSize) {
		ihMoveResult = "ring size doesn't fit";
		return;
	}
	auto *desc = IOMemoryDescriptor::withPhysicalAddress(smuLoad.phys + IhOffset, size + 0x1000, kIODirectionInOut);
	ihRingMap = desc ? desc->map(kIOMapInhibitCache) : nullptr;
	OSSafeReleaseNULL(desc);
	if (!ihRingMap) {
		ihMoveResult = "can't map the ring";
		return;
	}
	ihMovedGpu = smuLoad.mc + IhOffset;
	ihRingSize = size;
	reapplyIhRing(manager);
	ihMoveResult = "moved";
	traceNote("ihring=%llx ", ihMovedGpu);
}

void CRDriver::wrapIhSetHardwareEnabled(void *that, bool enable) {
	if (enable)
		moveIhRing(that);
	bool off = enable && checkKernelArgument("-crnoih");
	ihEnableCalls++;
	ihLastEnable = enable && !off;
	ihManager = that;
	traceEvent(off ? "IH enable (forced off by -crnoih)" : enable ? "IH enable" : "IH disable", true, enable);
	FunctionCast(wrapIhSetHardwareEnabled, callback->orgIhSetHardwareEnabled)(that, enable && !off);
	if (enable && !off && gpuMmio && ihMoveResult && !strcmp(ihMoveResult, "moved") &&
		gpuMmio[IhReg::RB_BASE] != static_cast<uint32_t>(ihMovedGpu >> 8)) {
		uint32_t was = gpuMmio[IhReg::RB_BASE];
		FunctionCast(wrapIhSetHardwareEnabled, callback->orgIhSetHardwareEnabled)(that, false);
		reapplyIhRing(that);
		FunctionCast(wrapIhSetHardwareEnabled, callback->orgIhSetHardwareEnabled)(that, true);
		diskLogEvent("ih ring moved again: RB_BASE %x -> %x", was, gpuMmio[IhReg::RB_BASE]);
		traceNote("ihring again(%u) ", ihRingMoves);
	}
	// A new enable starts from a freshly programmed ring: give the storm breaker a clean slate (it tripped on the stale
	// interrupts right after a wake, before the GPU was re-initialised, and then ignored every interrupt for good).
	if (enable && !off && __atomic_load_n(&ihStormBroken, __ATOMIC_RELAXED)) {
		ihWindowStart = 0;
		ihWindowCalls = 0;
		__atomic_store_n(&ihStormBroken, false, __ATOMIC_RELAXED);
		diskLogEvent("ih storm breaker re-armed on IH enable");
	}
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

// Storm breaker: more than 500 handler calls within 100 ms *with no new entries in the ring* means the IH keeps
// re-raising an interrupt the driver believes it has consumed. Disable the ring and interrupts directly (so the
// machine survives instead of freezing), record the IH state and the write-pointer write-back the CPU sees, and ignore
// further calls. -crnostormbreak disables this. If the ring's write pointer moved during the window, the calls were
// real work (a busy GPU at a higher clock finishes command buffers fast): count a false alarm and carry on. Disk log
// 1792383022: the old test (calls alone) tripped on such a burst at 847 MHz and turned interrupts off for good; the
// screen froze with the kernel running, which is what every "freeze" with SCLK DPM since nvram183 was.
void CRDriver::wrapIhHandler(void *that, void *owner, void *arg, void *nub, int source) {
	uint32_t calls = __atomic_add_fetch(&ihHandlerCalls, 1, __ATOMIC_RELAXED);
	if (ihStormBroken)
		return;
	uint64_t ms = uptimeMs();
	if (ms - ihWindowStart > 100) {
		ihWindowStart = ms;
		ihWindowCalls = 0;
		ihWindowWptr = gpuMmio ? gpuMmio[IhReg::RB_WPTR] : 0;
	}
	if (++ihWindowCalls > 500 && gpuMmio && callback->stormBreak) {
		auto *mmio = gpuMmio;
		uint32_t wptr = mmio[IhReg::RB_WPTR];
		if (wptr != ihWindowWptr) {
			// New entries arrived: a burst, not a storm.
			ihStormFalseAlarms++;
			ihWindowStart = ms;
			ihWindowCalls = 0;
			ihWindowWptr = wptr;
			FunctionCast(wrapIhHandler, callback->orgIhHandler)(that, owner, arg, nub, source);
			return;
		}
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
	int32_t before = preemptionLevelOff >= 0 ? preemptionLevel() : 0;
	FunctionCast(wrapIhHandler, callback->orgIhHandler)(that, owner, arg, nub, source);
	if (int32_t after = preemptionLevelOff >= 0 ? preemptionLevel() : 0; after != before)
		panic("CarrizoRed: preemption level %d -> %d across BaffinInterruptManager::handler (IH rptr %x wptr %x)", before,
			after, gpuMmio ? gpuMmio[IhReg::RB_RPTR] : 0, gpuMmio ? gpuMmio[IhReg::RB_WPTR] : 0);
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
	diskLogEvent("pci fn%u setPowerState(%lu) from %s", fn, state,
		whatDevice ? whatDevice->getMetaClass()->getClassName() : "?");
	auto ret = org(that, state, whatDevice);
	pmLog(true, "pci.setPowerState ret", ret, "<= PCI fn%u setPowerState(%lu) = 0x%x", fn, state, ret);
	diskLogEvent("pci fn%u setPowerState(%lu) = %x", fn, state, ret);
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
	auto powerOnEdp = [&]() {
		uint8_t power[32] {};
		power[3] = 12;  // ATOM_TRANSMITTER_ACTION_POWER_ON
		uint8_t powered = FunctionCast(wrapExecuteBiosCommandTable, callback->orgExecuteBiosCommandTable)(that,
			AtomTransmitterTable, power, flag);
		AtomCall injected {};
		injected.table = static_cast<uint16_t>(AtomTransmitterTable);
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
		return powered;
	};
	if (table == AtomTransmitterTable && params && !edpPowerOnOff) {
		auto *bytes = static_cast<uint8_t *>(params);
		if (bytes[3] == 1 && bytes[5] == 0x14) {
			edpSeen = true;
			__atomic_store_n(&edpWakePowerOn, false, __ATOMIC_RELAXED);
			powerOnEdp();
		}
	}
	// The same after a wake from S3 (Catalina, log 1793304455): the panel comes back unpowered (LVTMA_PWRSEQ_CNTL
	// 0x20211 before sleep, 0x2000011 after), and Apple first wakes the sink with DPCD 0x600 = 1 over AUX, which failed
	// 75 times (kIOReturnIOError) and left the CRTC stopped, so it never got as far as a transmitter ENABLE. Power the
	// panel before the first AUX transaction of a full wake.
	// An eDP panel is also known from the first VBIOS call of the boot: the firmware left LVTMA_PWRSEQ_CNTL.LVTMA_DIGON
	// (0x10000) set for the panel it lit (D509BA: 0x30311).
	static bool pwrseqChecked;
	if (!pwrseqChecked && gpuMmio) {
		pwrseqChecked = true;
		if (gpuMmio[0x481B] & 0x10000)
			edpSeen = true;
	}
	if (aux && __atomic_exchange_n(&wakeRestorePending, false, __ATOMIC_RELAXED)) {
		uint32_t restored = restoreAfterWake();
		diskLogEvent("wake: %u display registers restored to their boot values before the first AUX transaction", restored);
	}
	if (aux && edpSeen && !edpPowerOnOff && __atomic_exchange_n(&edpWakePowerOn, false, __ATOMIC_RELAXED)) {
		uint8_t powered = powerOnEdp();
		diskLogEvent("eDP power on before the first AUX transaction after wake: ok %u", powered & 1);
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
	// Link set-up (0x100-0x10A) and sink power (0x600) writes into the disk log: whether the panel was trained again
	// after a wake, and with what.
	if (write && data && length && ((address >= 0x100 && address <= 0x10A) || address == 0x600))
		diskLogEvent("dpcd write %x = %02x%s (len %u) -> %x", address, data[0], length > 1 ? "..." : "", length, ret);
	if (ret != kIOReturnSuccess && __atomic_exchange_n(&wakeAuxWatch, false, __ATOMIC_RELAXED)) {
		__atomic_store_n(&wakeDisplayDiag, true, __ATOMIC_RELAXED);
		__atomic_store_n(&wakeReportAtMs, uptimeMs() + 1000, __ATOMIC_RELAXED);
	}
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
	{0x1898, 0x30},  // DC_HPD1-6 (INT_STATUS, INT_CONTROL, CONTROL, ...)
	{0x5C00, 0xB0},  // DP_AUX0-5 (AUX_CONTROL, arbitration, DPHY TX/RX set-up)
};
static constexpr uint32_t DiffRegCount = 0x60 + 0x80 + 0x200 + 0x100 + 0x100 + 0x30 + 0xB0;
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

// What S3 resets and nothing sets again (Catalina wake #10, log 1793321673: the display report 1 s after the first
// failed DPCD transfer). At a cold boot the firmware's GOP programs these; after S3 nobody posts the display block, Apple's
// driver doesn't touch them, and they read their reset values:
// - DP_AUX0-5 AUX_DPHY_TX_REF_CONTROL, TX_CONTROL, RX_CONTROL0/1 (0x5C08-0x5C0B, 0x1C apart): TX_REF_DIV 0x32 (50, for
//   a 100 MHz reference) instead of the firmware's 0x18 (24), so AUX ran at the wrong bit rate. The VBIOS AUX table
//   reported success every time (364 calls, 0 failed), yet the panel never answered: every DPCD transfer kIOReturnIOError.
// - LVTMA_PWRSEQ_REF_DIV, DELAY1, DELAY2 (0x481D-0x481F): 0, so the panel power sequencer has no timing for POWER_ON.
// - BL_PWM_CNTL, CNTL2, PERIOD_CNTL (0x4820-0x4822): 0, the backlight PWM off.
// Write back the boot values (taken from the firmware's state in the diff snapshot) where they differ. -crnowakerestore
// disables.
static uint32_t restoreAfterWake() {
	static int off = -1;
	if (off < 0)
		off = checkKernelArgument("-crnowakerestore");
	if (off || !gpuMmio)
		return 0;
	// Wake #11 (log 1793323817): with the AUX PHY and panel registers back, AUX_SW_STATUS still read 0xA1 (done, receive
	// timeout) and the panel never answered. Also reset: the display reference clock and its time bases (REFCLK_CNTL 0
	// for 3; the millisecond/microsecond dividers AUX times its reply window with), the DENTIST display clock (Apple's
	// setDisplayClock can't set it here: no SetDCEClock table for its parser), the AUX pads' impedance calibration
	// (period 0, AUXP/AUXN calibration off), the panel power sequencer's GPIO pad mask/output/enable (the pad readback
	// lost bit 16) and UNIPHY0's macro settings. So, in order: clocks, pads, PHY, then AUX and panel power as before.
	// Wake #12 (log 1793326653): with those back too (39 writes), still 0xA1. Add the DCCG deep-sleep reference DTO
	// (DCCG_DS_DTO_INCR/MODULO 0, DCCG_DS_CNTL 0 for 0x10201: enable, hardware calibration, XTALIN divider) and
	// DC_MEM_GLOBAL_PWR_REQ_CNTL, and reset each enabled AUX engine afterwards (below): it ran on the wrong clocks before.
	static const uint32_t early[] {0x109, 0x130, 0x13B, 0x113, 0x114, 0x115, 0x132, 0x124, 0x131, 0x483A, 0x483B,
		0x483C, 0x4890, 0x4891, 0x4892};
	uint32_t regs[arrsize(early) + 0x18 + 6 * 4 + 6], n = 0, written = 0;
	for (auto r : early)
		regs[n++] = r;
	for (uint32_t r = 0x48C0; r < 0x48D8; r++)
		regs[n++] = r;
	for (uint32_t ch = 0; ch < 6; ch++)
		for (uint32_t r = 0; r < 4; r++)
			regs[n++] = 0x5C08 + ch * 0x1C + r;
	for (uint32_t r = 0x481D; r <= 0x4822; r++)
		regs[n++] = r;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t boot;
		if (diffBootValue(regs[i], boot) && gpuMmio[regs[i]] != boot) {
			gpuMmio[regs[i]] = boot;
			written++;
		}
	}
	// AUX_CONTROL (0x5C00 + 0x1C per channel): pulse AUX_RESET (0x10) on each enabled engine (AUX_EN, 0x1) and wait for
	// AUX_RESET_DONE (0x20), up to 1 ms.
	for (uint32_t ch = 0; ch < 6; ch++) {
		volatile uint32_t &control = gpuMmio[0x5C00 + ch * 0x1C];
		uint32_t value = control;
		if (!(value & 1))
			continue;
		control = value | 0x10;
		for (uint32_t us = 0; us < 1000 && !(control & 0x20); us += 10)
			IODelay(10);
		control = value & ~0x10U;
	}
	return written;
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

// The GPU's registers (BAR5). IOKit's mapDeviceMemoryWithRegister can fail when CarrizoRed starts before the PCI
// family has registered the device's memory ranges (nvram4-6: black screens, gpuMmio never set, so no SMU microcode
// load, no FB move, nothing): then map BAR5 straight from config space (256 KB, uncached). Retried on demand
// (ensureMmio) from the watcher, the SMU loader and the RLC halt hook.
static volatile uint32_t *mapGpuMmio(IOPCIDevice *dev) {
	if (auto *map = dev->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5)) {
		mmioMapMethod = 1;
		probePhys = map->getPhysicalAddress();
		return reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
	}
	mmioMapFailures++;
	uint32_t bar = dev->configRead32(kIOPCIConfigBaseAddress5) & ~0xFU;
	if (!bar || bar == 0xFFFFFFF0U)
		return nullptr;
	if (!(dev->configRead16(kIOPCIConfigCommand) & 2))
		dev->configWrite16(kIOPCIConfigCommand, dev->configRead16(kIOPCIConfigCommand) | 2);  // memory space on
	auto *desc = IOMemoryDescriptor::withPhysicalAddress(bar, 0x40000, kIODirectionInOut);
	auto *map = desc ? desc->map(kIOMapInhibitCache) : nullptr;
	OSSafeReleaseNULL(desc);
	if (!map)
		return nullptr;
	mmioMapMethod = 2;
	probePhys = bar;
	return reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
}

void CRDriver::ensureMmio() {
	if (!gpuMmio && callback && callback->iGPU)
		callback->probeMMIO();
}

void CRDriver::probeMMIO() {
	auto *mmio = mapGpuMmio(iGPU);
	if (!mmio) {
		SYSLOG("cr", "failed to map MMIO BAR, skipping register probe (will retry)");
		return;
	}
	// Keep the mapping: the IH storm breaker writes registers directly (see wrapIhHandler), and the watcher reads the
	// display registers (checkDisplayRegs).
	gpuMmio = mmio;
	auto memSize = mmio[Reg::CONFIG_MEMSIZE];
	auto fbLocation = mmio[Reg::MC_VM_FB_LOCATION];
	auto fbOffset = mmio[Reg::MC_VM_FB_OFFSET];
	auto grbmStatus = mmio[Reg::GRBM_STATUS];
	probeValues[0] = memSize;
	probeValues[1] = fbLocation;
	probeValues[2] = fbOffset;
	probeValues[3] = grbmStatus;
	probeMs = uptimeMs();
	if (!uvdAtProbe.taken && !gpuRegistersDead(mmio) && checkKernelArgument("-crvideo"))
		uvdSnapshot(uvdAtProbe);
	if (gpuRegistersDead(mmio)) {
		gpuDeadAtStart = true;
		checkGpuAlive(mmio);
		SYSLOG("cr", "the GPU isn't responding at start-up (registers read 0xFFFFFFFF): power the machine off fully");
		iGPU->setProperty("CarrizoRed,gpu-dead", true);
	}
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

// What the Metal driver programs: the most recent user command buffers (INDIRECT_BUFFER packets with a VMID on the
// GFX ring), read through that VMID's page tables. GFX8 VM: one VM_CONTEXT1_CNTL for VMIDs 1-15 (PAGE_TABLE_DEPTH
// [2:1], PAGE_TABLE_BLOCK_SIZE [27:24]: 2^(9 + size) entries per page table), page directory base per VMID
// (VM_CONTEXT1-7_PAGE_TABLE_BASE_ADDR 0x550-0x556, VM_CONTEXT8-15 0x50E-0x515), start at
// VM_CONTEXT1_PAGE_TABLE_START_ADDR (0x558). Entries: [0] valid, [1] system, [39:12] address; VRAM entries hold the
// carve-out's physical address once the VM fix has converted them, else an FB offset. Packets: every context register
// SET_CONTEXT_REG writes (last value; up to four distinct values for the ones that decide blending, colour formats,
// pixel shader inputs and primitive set-up), the UCONFIG primitive and index types, and the draw packets by type.
// The carve-out ("VRAM"), mapped once: command buffers, page tables and shader code mostly live there, and a mapping per
// 4 KB read is too slow for the submit-time shader fix.
static IOMemoryMap *carveMapObj;
static uint8_t *carveMap;
static uint64_t carveBase, carveSize;
static bool carveInitStarted;

static bool carveInit() {
	if (carveMap)
		return true;
	if (!gpuMmio || __atomic_exchange_n(&carveInitStarted, true, __ATOMIC_ACQ_REL))
		return false;
	auto *mmio = gpuMmio;
	uint64_t base = static_cast<uint64_t>(mmio[Reg::MC_VM_FB_OFFSET] & 0x3FFFF) << 22;
	uint64_t size = ((static_cast<uint64_t>(mmio[Reg::MC_VM_FB_LOCATION] >> 16) + 1) << 24) -
		(static_cast<uint64_t>(mmio[Reg::MC_VM_FB_LOCATION] & 0xFFFF) << 24);
	if (!base || !size || size > 0x40000000ULL)
		return false;
	auto *desc = IOMemoryDescriptor::withPhysicalAddress(base, size, kIODirectionInOut);
	auto *map = desc ? desc->map(kIOMapInhibitCache) : nullptr;
	OSSafeReleaseNULL(desc);
	if (!map)
		return false;
	carveBase = base;
	carveSize = size;
	carveMapObj = map;
	__atomic_store_n(&carveMap, reinterpret_cast<uint8_t *>(map->getVirtualAddress()), __ATOMIC_RELEASE);
	return true;
}

// Set while the submit hook runs: it must neither block nor allocate (it runs inside the accelerator's submission
// path; a mapping made there can wait on memory the accelerator has to give back, which needs that same path: the two
// boots with the first version of the hook froze). Only the carve-out mapping, made beforehand by the scan thread.
// The kernel's physmap (every physical page at physmap_base + address, write-back): command buffers and shader code
// are mostly in system memory, which the submit hook can't map (nvram164: it saw 32760 command buffers and fixed
// nothing). Reads flush the lines first (the Metal driver writes through its own write-combined mapping); writes flush
// after (the GPU may read system memory without snooping).
static uint8_t *physmapPtr(uint64_t phys, uint32_t bytes) {
	if (!interpSubmit || !physmapBaseVar || !physmapMaxVar)
		return nullptr;
	uint64_t base = *physmapBaseVar, max = *physmapMaxVar;
	if (!base || base + phys + bytes > max || base + phys < base)
		return nullptr;
	return reinterpret_cast<uint8_t *>(base + phys);
}

static void flushLines(const uint8_t *p, uint32_t bytes) {
	uintptr_t a = reinterpret_cast<uintptr_t>(p) & ~static_cast<uintptr_t>(63);
	for (; a < reinterpret_cast<uintptr_t>(p) + bytes; a += 64)
		__builtin_ia32_clflush(reinterpret_cast<const void *>(a));
	__builtin_ia32_mfence();
}

static uint32_t hookDepth;
static inline bool inHook() {
	return __atomic_load_n(&hookDepth, __ATOMIC_ACQUIRE) != 0;
}

static uint8_t *carvePtr(uint64_t phys, uint32_t bytes) {
	if (!interpSubmit)
		return nullptr;
	if (!__atomic_load_n(&carveMap, __ATOMIC_ACQUIRE) && (inHook() || !carveInit()))
		return nullptr;
	if (phys < carveBase || phys + bytes > carveBase + carveSize)
		return nullptr;
	return carveMap + (phys - carveBase);
}

// coherent: system memory the GPU snoops (PTE bits system + snooped), which the CPU caches stay coherent with: no flush
// needed. Other system memory is read through the physmap after flushing the lines. The carve-out goes through its
// uncached mapping (8 bytes per memory access) unless -crcarvewb (the physmap, a 64-byte line fill: faster, but see
// below).
// Physical memory we may touch: RAM (below TOP_MEM, or from 4 GB up to TOM2: AMD's MSRs C001_001A / C001_001D) and the
// carve-out (MC_VM_FB_OFFSET, the size of the FB window), never the MMIO hole or anything above. These used to be the
// D509BA's numbers (3 GB / 8.5 GB): a tester's A9 with more RAM has its 1 GB carve-out at 0x2EF000000, so the
// interpolation fix read none of its command buffers.
static uint64_t ramTopLow, ramTopHigh;

static bool physRangeOk(uint64_t phys, uint32_t bytes) {
	if (!phys || phys + bytes < phys)
		return false;
	if (!ramTopLow) {
		auto rdmsr = [](uint32_t msr) {
			uint32_t lo, hi;
			asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
			return static_cast<uint64_t>(hi) << 32 | lo;
		};
		uint64_t topMem = rdmsr(0xC001001A) & 0xFFFFFF800000ULL;
		uint64_t tom2 = rdmsr(0xC001001D) & 0xFFFFFF800000ULL;
		ramTopHigh = tom2 > 0x100000000ULL ? tom2 : 0;
		__atomic_store_n(&ramTopLow, topMem ? topMem : 0xC0000000ULL, __ATOMIC_RELEASE);
	}
	if (phys + bytes <= ramTopLow)
		return true;
	if (phys >= 0x100000000ULL && phys + bytes <= ramTopHigh)
		return true;
	auto *mmio = gpuMmio;
	if (!mmio)
		return false;
	uint32_t location = mmio[Reg::MC_VM_FB_LOCATION];
	uint64_t carve = static_cast<uint64_t>(mmio[Reg::MC_VM_FB_OFFSET] & 0x3FFFF) << 22;
	uint64_t size = ((static_cast<uint64_t>(location >> 16) + 1) << 24) - (static_cast<uint64_t>(location & 0xFFFF) << 24);
	return location != 0xFFFFFFFF && carve && phys >= carve && phys + bytes <= carve + size;
}

static bool ibReadPhys(uint64_t phys, uint32_t bytes, void *out, bool coherent = false) {
	if (!physRangeOk(phys, bytes))
		return false;
	// The carve-out through its uncached mapping (as before nvram180) unless -crcarvewb. Reading it through the
	// physmap's cached mapping as well aliases two memory types on the same RAM, which x86 leaves undefined, and the
	// freezes seen since (6 of 15 boots, all with SCLK DPM on, nvram183-123) started three boots after that change.
	static int carveWbOff = -1;
	if (carveWbOff < 0)
		carveWbOff = checkKernelArgument("-crcarvewb") ? 0 : 1;
	if (carveWbOff) {
		if (auto *fast = carvePtr(phys, bytes)) {
			memcpy(out, fast, bytes);
			return true;
		}
	}
	if (auto *direct = physmapPtr(phys, bytes)) {
		if (!coherent)
			flushLines(direct, bytes);
		memcpy(out, direct, bytes);
		return true;
	}
	if (auto *fast = carvePtr(phys, bytes)) {
		memcpy(out, fast, bytes);
		return true;
	}
	if (inHook())
		return false;
	auto *desc = IOMemoryDescriptor::withPhysicalAddress(phys, bytes, kIODirectionIn);
	auto *map = desc ? desc->map(kIOMapInhibitCache) : nullptr;
	bool mapped = map != nullptr;
	if (mapped)
		memcpy(out, reinterpret_cast<const void *>(map->getVirtualAddress()), bytes);
	OSSafeReleaseNULL(map);
	OSSafeReleaseNULL(desc);
	return mapped;
}

// GPU (MC) address of a page table structure or VRAM page -> system physical address. The frame buffer location and
// carve-out don't change once the driver has set them, so they are read from the GPU once: each register read is an
// uncached access, and a translation used to make about 12 (nvram176: the submit hook took ~200 us per command buffer).
static uint64_t mcFbBase, mcFbTop, mcCarveout;
static bool mcKnown;

static uint64_t ibMcToPhys(uint64_t addr, bool system) {
	if (!__atomic_load_n(&mcKnown, __ATOMIC_ACQUIRE)) {
		auto *mmio = gpuMmio;
		uint32_t location = mmio[Reg::MC_VM_FB_LOCATION];
		uint64_t fbBase = static_cast<uint64_t>(location & 0xFFFF) << 24;
		uint64_t fbTop = (static_cast<uint64_t>(location >> 16) + 1) << 24;
		uint64_t carveout = static_cast<uint64_t>(mmio[Reg::MC_VM_FB_OFFSET] & 0x3FFFF) << 22;
		if (!carveout || fbTop <= fbBase || location == 0xFFFFFFFF)
			return !system && addr >= fbBase && addr < fbTop ? carveout + (addr - fbBase) : addr;
		mcFbBase = fbBase;
		mcFbTop = fbTop;
		mcCarveout = carveout;
		__atomic_store_n(&mcKnown, true, __ATOMIC_RELEASE);
	}
	if (!system && addr >= mcFbBase && addr < mcFbTop)
		return mcCarveout + (addr - mcFbBase);
	return addr;
}

// Translations made while holding interpLock (the submit hook or the scan; ibVaToPhysLocked), remembered until it lets
// go: page tables don't change under a command buffer being submitted. Other callers walk the tables every time.
struct VmCacheEntry {
	uint64_t page, phys;
	uint32_t vmid, gen, flags;  // flags: the PTE's low bits ([0] valid, [1] system, [2] snooped)
};
static VmCacheEntry vmCache[64];
static uint32_t vmCacheGen;
static uint32_t vmWalks;
// VM_CONTEXT1_CNTL and the page table start, read once the VM is enabled.
static uint32_t vmCntl, vmStart;
static bool vmKnown;

// Called right after taking interpLock: forget the last holder's translations.
static void vmCacheBegin() {
	if (++vmCacheGen == 0)
		vmCacheGen = 1;
}

static uint64_t ibVaToPhysWalk(uint32_t vmid, uint64_t va, VmCacheEntry *slot, uint32_t *flags = nullptr) {
	auto *mmio = gpuMmio;
	if (!vmid || vmid > 15)
		return 0;
	uint64_t page = va >> 12;
	vmWalks++;
	uint32_t cntl, start;
	if (__atomic_load_n(&vmKnown, __ATOMIC_ACQUIRE)) {
		cntl = vmCntl;
		start = vmStart;
	} else {
		cntl = mmio[0x505];
		start = mmio[0x558];
		if ((cntl & 1) && cntl != 0xFFFFFFFF) {
			vmCntl = cntl;
			vmStart = start;
			__atomic_store_n(&vmKnown, true, __ATOMIC_RELEASE);
		}
	}
	uint32_t depth = (cntl >> 1) & 3, block = (cntl >> 24) & 0xF;
	uint64_t pdb = static_cast<uint64_t>(vmid < 8 ? mmio[0x550 + vmid - 1] : mmio[0x50E + vmid - 8]) << 12;
	uint64_t pfn = page - start;
	uint64_t entry = 0;
	if (depth == 0) {
		if (!ibReadPhys(ibMcToPhys(pdb, false) + pfn * 8, 8, &entry))
			return 0;
	} else {
		uint32_t bits = 9 + block;
		uint64_t pde = 0;
		if (!ibReadPhys(ibMcToPhys(pdb, false) + (pfn >> bits) * 8, 8, &pde) || !(pde & 1))
			return 0;
		uint64_t pt = pde & 0xFFFFFFFFF000ULL;
		if (!ibReadPhys(ibMcToPhys(pt, pde & 2) + (pfn & ((1ULL << bits) - 1)) * 8, 8, &entry))
			return 0;
	}
	if (!(entry & 1))
		return 0;
	uint64_t phys = ibMcToPhys(entry & 0xFFFFFFFFF000ULL, entry & 2);
	if (slot)
		*slot = {page, phys, vmid, vmCacheGen, static_cast<uint32_t>(entry & 0xFFF)};
	if (flags)
		*flags = static_cast<uint32_t>(entry & 0xFFF);
	return phys + (va & 0xFFF);
}

static uint64_t ibVaToPhys(uint32_t vmid, uint64_t va) {
	return ibVaToPhysWalk(vmid, va, nullptr);
}

// Only with interpLock held.
static uint64_t ibVaToPhysLocked(uint32_t vmid, uint64_t va, uint32_t *flags = nullptr) {
	uint64_t page = va >> 12;
	VmCacheEntry *slot = &vmCache[(page ^ (page >> 6) ^ vmid) & (arrsize(vmCache) - 1)];
	if (slot->gen == vmCacheGen && slot->vmid == vmid && slot->page == page) {
		if (flags)
			*flags = slot->flags;
		return slot->phys + (va & 0xFFF);
	}
	return ibVaToPhysWalk(vmid, va, slot, flags);
}

// GFX8 (GCN3) instruction length in dwords, for walking shader code: SOP2/SOPK/SOP1/SOPC/SOPP and VOP1/VOP2/VOPC and
// VINTRP are one dword (+ a literal when a source operand is 0xFF; + one for SDWA/DPP, src0 0xF9/0xFA), SMEM, VOP3, DS,
// FLAT, MUBUF, MTBUF, MIMG and EXP two.
static uint32_t gcn3InstLength(const uint32_t *code, uint32_t left) {
	uint32_t w = code[0];
	if (!(w >> 31)) {  // VOP2 / VOP1 / VOPC
		uint32_t src0 = w & 0x1FF;
		// v_madmk_f32/v_madak_f32/v_madmk_f16/v_madak_f16 (VOP2 0x17, 0x18, 0x24, 0x25) always carry a literal.
		uint32_t op2 = (w >> 25) & 0x3F, enc7 = w >> 25;
		bool vop2 = enc7 != 0x3E && enc7 != 0x3F;
		if (vop2 && (op2 == 0x17 || op2 == 0x18 || op2 == 0x24 || op2 == 0x25))
			return src0 == 0xF9 || src0 == 0xFA ? 3 : 2;
		return src0 == 0xFF || src0 == 0xF9 || src0 == 0xFA ? 2 : 1;
	}
	if ((w >> 30) == 2) {  // scalar ALU
		// SOPP, SOP1 and SOPC first: they share SOPK's top bits (1011). nvram176: checked the other way round,
		// `s_mov_b32 s0, 0xff7fffff` was one dword and its literal was taken for an instruction.
		uint32_t top9 = w >> 23;
		if (top9 == 0x17F)  // SOPP
			return 1;
		if (top9 == 0x17D)  // SOP1
			return (w & 0xFF) == 0xFF ? 2 : 1;
		if (top9 == 0x17E)  // SOPC
			return (w & 0xFF) == 0xFF || ((w >> 8) & 0xFF) == 0xFF ? 2 : 1;
		if ((w >> 28) == 0xB)  // SOPK; s_setreg_imm32_b32 (op 0x14) carries a literal
			return ((w >> 23) & 0x1F) == 0x14 ? 2 : 1;
		return (w & 0xFF) == 0xFF || ((w >> 8) & 0xFF) == 0xFF ? 2 : 1;  // SOP2
	}
	switch (w >> 26) {
		case 0x35:  // VINTRP (GCN3; 0x32 on GCN1/2)
			return 1;
		default:
			return left >= 2 ? 2 : 1;
	}
}

// One past the program's last instruction in code[0..got): the s_endpgm no branch jumps beyond (an early s_endpgm, for
// waves with every pixel killed say, has more code after it). 0 if it isn't in code[0..got).
static uint32_t gcn3ProgramEnd(const uint32_t *code, uint32_t got) {
	uint32_t furthest = 0;
	for (uint32_t i = 0; i < got;) {
		uint32_t w = code[i];
		if ((w >> 23) == 0x17F) {
			uint32_t op = (w >> 16) & 0x7F;
			if (op == 0x01 && furthest <= i)
				return i + 1;
			if (op == 0x02 || (op >= 0x04 && op <= 0x09) || (op >= 0x17 && op <= 0x1A)) {
				int32_t to = static_cast<int32_t>(i) + 1 + static_cast<int16_t>(w & 0xFFFF);
				if (to > static_cast<int32_t>(furthest))
					furthest = static_cast<uint32_t>(to);
			}
		}
		i += gcn3InstLength(code + i, got - i);
	}
	return 0;
}

// Pixel shaders the command buffer runs (SPI_SHADER_PGM_LO/HI_PS, SH registers 0x08/0x09): read each through the VM and
// count its v_interp instructions. Stoney has 16 LDS banks: v_interp_p1_f32 must not write the VGPR it reads (its I
// barycentric); a back end for 32-bank chips may do that, which gives a wrong attribute (nvram141 photos: textures
// sampled with one coordinate fixed, only one triangle of each quad visible). Reports, per shader, the p1/p2/mov
// counts, how many p1 have vdst == vsrc, and the first such instruction.
static void reportPixelShaders(uint32_t vmid, const uint64_t *addrs, uint32_t count,
	void (*put)(void *, const char *, ...), void *ctx) {
	static uint32_t code[0x800];
	for (uint32_t k = 0; k < count; k++) {
		uint64_t va = addrs[k];
		uint32_t got = 0;
		while (got < arrsize(code)) {
			uint64_t a = va + got * 4ULL;
			uint32_t chunk = static_cast<uint32_t>((0x1000 - (a & 0xFFF)) / 4);
			if (chunk > arrsize(code) - got)
				chunk = arrsize(code) - got;
			uint64_t phys = ibVaToPhys(vmid, a);
			if (!phys || !ibReadPhys(phys, chunk * 4, code + got))
				break;
			got += chunk;
		}
		uint32_t p1 = 0, p2 = 0, mov = 0, bad = 0, firstBad = 0, shown[6] {}, shownNum = 0;
		uint32_t len = gcn3ProgramEnd(code, got);
		for (uint32_t i = 0; i < (len ? len : got);) {
			uint32_t w = code[i];
			if ((w >> 26) == 0x35) {
				uint32_t op = (w >> 16) & 3, vdst = (w >> 18) & 0xFF, vsrc = w & 0xFF;
				if (op == 0) {
					p1++;
					if (shownNum < arrsize(shown))
						shown[shownNum++] = w;
					if (vdst == vsrc && !bad++)
						firstBad = w;
				} else if (op == 1) {
					p2++;
				} else {
					mov++;
				}
			}
			i += gcn3InstLength(code + i, got - i);
		}
		put(ctx, " ps%u %llx: %u dw, p1 %u p2 %u mov %u bad %u", k, va, len ? len : got, p1, p2, mov, bad);
		if (bad)
			put(ctx, " (%08x)", firstBad);
		for (uint32_t q = 0; q < shownNum; q++)
			put(ctx, q ? ",a%u%c:v%u<-v%u" : " [a%u%c:v%u<-v%u", (shown[q] >> 10) & 0x3F, "xyzw"[(shown[q] >> 8) & 3],
				(shown[q] >> 18) & 0xFF, shown[q] & 0xFF);
		if (shownNum)
			put(ctx, "]");
	}
}

// Stoney's 16-bank LDS interpolation hazard, fixed in the compiled pixel shaders. On chips with 16 LDS banks (Kabini,
// Stoney) v_interp_p1_f32 must not write the VGPR it reads (its I barycentric): LLVM marks the 16-bank variant's vdst
// early-clobber. Apple's shader compiler doesn't, for its Stoney back end either, and reuses the I register for the last
// component once I is otherwise dead (nvram153: three of eight WindowServer pixel shaders end their vec4 with
// `v_interp_p1_f32 v0, v0, attr0.w`). The result is a wrong value on part of each wave: vibrant text drawn as bars,
// broken highlights and window shadows, Dock icons cut on a diagonal. Plain text and the login screen use shaders
// without the pattern.
//
// So rewrite those shaders in GPU memory: give the bad p1 and its p2 a VGPR the shader never touches but has allocated
// (SPI_SHADER_PGM_RSRC1_PS.VGPRS, granules of 4), and rename the result's later reads. Every change replaces one dword
// in place; nothing moves. A shader whose later code we can't fully account for (an encoding whose VGPR operands we
// don't decode touching the register, a backward branch, a register range containing it) is left alone and counted.
// A thread scans the recent user command buffers every second for pixel shaders (SPI_SHADER_PGM_LO/HI_PS, RSRC1_PS)
// and remembers what it has seen (VMID, address and a hash of the code). -crnointerpfix disables it.

struct InterpFixStats {
	uint32_t scans, shaders, hazards, fixed, skipped, lastSkip, writes, writeFails, deadRegs, bumps;
	uint32_t lastSkipWhy, lastLoopTarget, lastShaderLen, lastHazardAt, lastUnknownWord, lastUnknownAt;
	uint32_t probeReverts, blockMoves, lastBlockFail, revertMoved, blockGrows;
};
static InterpFixStats interpFix;

static bool ibWritePhys(uint64_t phys, uint32_t bytes, const void *in) {
	if (!physRangeOk(phys, bytes))
		return false;
	if (auto *fast = carvePtr(phys, bytes)) {
		memcpy(fast, in, bytes);
		return true;
	}
	if (auto *direct = physmapPtr(phys, bytes)) {
		memcpy(direct, in, bytes);
		flushLines(direct, bytes);
		return true;
	}
	if (inHook())
		return false;
	auto *desc = IOMemoryDescriptor::withPhysicalAddress(phys, bytes, kIODirectionOut);
	auto *map = desc ? desc->map(kIOMapInhibitCache) : nullptr;
	bool mapped = map != nullptr;
	if (mapped)
		memcpy(reinterpret_cast<void *>(map->getVirtualAddress()), in, bytes);
	OSSafeReleaseNULL(map);
	OSSafeReleaseNULL(desc);
	return mapped;
}

// A VGPR operand field of an instruction: dword (0 or 1 within the instruction), bit position, width (8, or 9 for a
// "src" field where VGPRs are 256-511), how many consecutive VGPRs it names, and whether it is read and/or written.
struct VgprField {
	uint8_t dword, shift, width, count;
	bool read, write;
	bool exact;  // count is the instruction's real register count (else an upper bound)
};

// GCN3 (GFX8) VGPR operands of the instruction at code[0]. Returns false for an encoding whose operands we don't model.
static bool gcn3VgprFields(const uint32_t *code, uint32_t len, VgprField *f, uint32_t &n) {
	n = 0;
	uint32_t w = code[0];
	auto add = [&](uint8_t dword, uint8_t shift, uint8_t width, uint8_t count, bool read, bool write,
				   bool exact = false) {
		f[n++] = {dword, shift, width, count, read, write, exact};
	};
	if (!(w >> 31)) {
		uint32_t enc7 = w >> 25;
		uint32_t src0 = w & 0x1FF;
		bool sdwaDpp = src0 == 0xF9 || src0 == 0xFA;
		if (enc7 == 0x3F) {  // VOP1
			uint32_t op = (w >> 9) & 0xFF;
			if (op >= 0x36 && op <= 0x38)  // v_movrel*: M0-relative
				return false;
			if (sdwaDpp) {
				if (len < 2)
					return false;
				add(1, 0, 8, 1, true, false);
			} else {
				add(0, 0, 9, 1, true, false);
			}
			if (op != 0x02)  // v_readfirstlane_b32 writes an SGPR
				add(0, 17, 8, 1, false, true);
			return true;
		}
		if (enc7 == 0x3E) {  // VOPC
			if (sdwaDpp) {
				if (len < 2)
					return false;
				add(1, 0, 8, 1, true, false);
			} else {
				add(0, 0, 9, 1, true, false);
			}
			add(0, 9, 8, 1, true, false);
			return true;
		}
		// VOP2
		uint32_t op = (w >> 25) & 0x3F;
		if (sdwaDpp) {
			if (len < 2)
				return false;
			add(1, 0, 8, 1, true, false);
		} else {
			add(0, 0, 9, 1, true, false);
		}
		add(0, 9, 8, 1, true, false);
		bool macLike = op == 0x16 || op == 0x23;  // v_mac_f32 / v_mac_f16 also read vdst
		add(0, 17, 8, 1, macLike, true);
		return true;
	}
	if ((w >> 30) == 2)  // scalar ALU / SOPP: no VGPRs
		return true;
	if (len < 1)
		return false;
	switch (w >> 26) {
		case 0x30:  // SMEM
			return true;
		case 0x35: {  // VINTRP
			uint32_t op = (w >> 16) & 3;
			add(0, 0, 8, 1, op != 2, false);
			add(0, 18, 8, 1, op == 1, true);  // p2 accumulates into vdst
			return true;
		}
		case 0x34: {  // VOP3
			if (len < 2)
				return false;
			uint32_t op = (w >> 16) & 0x3FF;
			add(1, 0, 9, 1, true, false);
			add(1, 9, 9, 1, true, false);
			add(1, 18, 9, 1, true, false);
			if (op >= 0x100 && op != 0x289)  // VOPC as VOP3 and v_readlane write SGPRs
				add(0, 0, 8, 1, false, true);
			return true;
		}
		case 0x31:  // EXP
			if (len < 2)
				return false;
			add(1, 0, 8, 1, true, false);
			add(1, 8, 8, 1, true, false);
			add(1, 16, 8, 1, true, false);
			add(1, 24, 8, 1, true, false);
			return true;
		case 0x3C: {  // MIMG: vaddr range read; vdata written by loads/samples, read by stores, both by atomics
			if (len < 2)
				return false;
			// vdata: one register per dmask component (gathers: always 4), plus one for the TFE/LWE status. GFX8's d16 is
			// unpacked (each half in its own register's low bits), so d16 doesn't change the count (Tahoe, log
			// 1793354624: 119 shaders refused at a d16 image_sample_b, f0940f00 80450103, counted as a bound).
			uint32_t op = (w >> 18) & 0x7F, dmask = (w >> 8) & 0xF;
			bool status = (w >> 16) & 3;  // TFE, LWE
			bool gather = op >= 0x40 && op <= 0x5F;
			uint32_t components = gather ? 4 : __builtin_popcount(dmask) ? __builtin_popcount(dmask) : 1;
			uint8_t count = static_cast<uint8_t>(components + (status ? 1 : 0));
			bool store = op >= 0x08 && op <= 0x0B, atomic = op >= 0x0F && op <= 0x1C;
			add(1, 0, 8, 4, true, false);
			if (store)
				add(1, 8, 8, static_cast<uint8_t>(components), true, false, true);
			else if (atomic)
				add(1, 8, 8, 4, true, true);
			else
				add(1, 8, 8, count, false, true, true);
			return true;
		}
		case 0x38: {  // MUBUF: exact vdata sizes for the plain loads and stores; d16, lds and atomics are bounds
			if (len < 2)
				return false;
			uint32_t op = (w >> 18) & 0x7F;
			bool lds = (w >> 16) & 1;
			add(1, 0, 8, 2, true, false);
			static const uint8_t formatSize[4] {1, 2, 3, 4};
			if (lds) {
			} else if (op <= 0x03) {
				add(1, 8, 8, formatSize[op], false, true, true);
			} else if (op <= 0x07) {
				add(1, 8, 8, formatSize[op - 4], true, false, true);
			} else if (op >= 0x10 && op <= 0x17) {
				static const uint8_t loadSize[8] {1, 1, 1, 1, 1, 2, 3, 4};
				add(1, 8, 8, loadSize[op - 0x10], false, true, true);
			} else if (op >= 0x18 && op <= 0x1F) {
				static const uint8_t storeSize[8] {1, 1, 1, 1, 1, 2, 3, 4};
				add(1, 8, 8, storeSize[op - 0x18], true, false, true);
			} else {
				add(1, 8, 8, 4, true, true);
			}
			return true;
		}
		case 0x3A: {  // MTBUF: tbuffer_load_format_x-xyzw 0-3, store 4-7, d16 8-15
			if (len < 2)
				return false;
			uint32_t op = (w >> 15) & 0xF;
			add(1, 0, 8, 2, true, false);
			if (op <= 3)
				add(1, 8, 8, static_cast<uint8_t>(op + 1), false, true, true);
			else if (op <= 7)
				add(1, 8, 8, static_cast<uint8_t>(op - 3), true, false, true);
			else
				add(1, 8, 8, 4, true, true);
			return true;
		}
		case 0x36:  // DS
			if (len < 2)
				return false;
			add(1, 0, 8, 1, true, false);
			add(1, 8, 8, 4, true, false);
			add(1, 16, 8, 4, true, false);
			add(1, 24, 8, 4, false, true);
			return true;
		case 0x37:  // FLAT
			if (len < 2)
				return false;
			add(1, 0, 8, 2, true, false);
			add(1, 8, 8, 4, true, false);
			add(1, 24, 8, 4, false, true);
			return true;
		default:
			return false;
	}
}

static uint32_t fieldValue(const uint32_t *code, const VgprField &f) {
	uint32_t v = (code[f.dword] >> f.shift) & ((1U << f.width) - 1);
	if (f.width == 9)
		return v >= 256 ? v - 256 : 0x1000;  // not a VGPR
	return v;
}

static void setField(uint32_t *code, const VgprField &f, uint32_t vgpr) {
	uint32_t mask = ((1U << f.width) - 1) << f.shift;
	uint32_t v = f.width == 9 ? vgpr + 256 : vgpr;
	code[f.dword] = (code[f.dword] & ~mask) | (v << f.shift);
}

enum InterpSkip {
	SkipNone, SkipNoEnd, SkipUnknownEncoding, SkipNoFreeVgpr, SkipNoP2, SkipBranch, SkipRange, SkipWriteBetween
};

// Returns the number of hazards fixed in code[0..len) (0 if none or skipped; skip reason in *skip).
// Control flow of one instruction: falls through to the next, and/or branches to target (-1: none).
static void cfgStep(const uint32_t *code, uint32_t i, bool &fall, int32_t &target) {
	uint32_t x = code[i];
	fall = true;
	target = -1;
	if ((x >> 23) == 0x17F) {
		uint32_t op = (x >> 16) & 0x7F;
		int32_t to = static_cast<int32_t>(i) + 1 + static_cast<int16_t>(x & 0xFFFF);
		if (op == 0x01) {  // s_endpgm
			fall = false;
		} else if (op == 0x02) {  // s_branch
			fall = false;
			target = to;
		} else if ((op >= 0x04 && op <= 0x09) || (op >= 0x17 && op <= 0x1A)) {
			target = to;
		}
	}
}

// The instruction graph of one shader, for following a register's values backwards: which dwords start an
// instruction, the instruction before each, and the branches into each (cfgFirstIn: 1 + index into cfgBranchFrom).
// The largest shader the fixer reads and analyses, in dwords (512 KB). Sonoma's lock screen shader is past 128 KB (no
// s_endpgm in the first 32768 dwords, 1554 branches): it was skipped and always drew unfixed (a black wallpaper).
static constexpr uint32_t MaxShaderDwords = 0x20000;
static uint8_t cfgIsStart[MaxShaderDwords];
static uint32_t cfgPrev[MaxShaderDwords], cfgFirstIn[MaxShaderDwords];
static uint32_t cfgBranchFrom[0x4000], cfgNextIn[0x4000];

static bool cfgBuild(const uint32_t *code, uint32_t end) {
	if (end > arrsize(cfgIsStart))
		return false;
	memset(cfgIsStart, 0, end);
	memset(cfgFirstIn, 0, end * sizeof(cfgFirstIn[0]));
	uint32_t prev = 0xFFFFFFFF, branches = 0;
	for (uint32_t i = 0; i < end; i += gcn3InstLength(code + i, end - i)) {
		cfgIsStart[i] = 1;
		cfgPrev[i] = prev;
		prev = i;
	}
	for (uint32_t i = 0; i < end; i += gcn3InstLength(code + i, end - i)) {
		bool fall;
		int32_t target;
		cfgStep(code, i, fall, target);
		if (target < 0)
			continue;
		if (target >= static_cast<int32_t>(end) || !cfgIsStart[target] || branches == arrsize(cfgBranchFrom))
			return false;  // a branch out of the program or into the middle of an instruction
		cfgBranchFrom[branches] = i;
		cfgNextIn[branches] = cfgFirstIn[target];
		cfgFirstIn[target] = ++branches;
	}
	return true;
}

// When the hazard's result is read as part of a register block (an image coordinate, say), it can't get a register of
// its own: move the whole block [base, base + k) to fresh registers [F, F + k) instead. Each member moves as a web:
// the definitions reaching the block read, every read those reach, and (until nothing changes) every other definition
// reaching one of those reads. So a member may be set on several paths, or written any number of times elsewhere in
// the shader (the first version needed one definition, and gave up past 31 writes of the register). The hazard's p1/p2
// are in the result's web, so moving it fixes the hazard. A member whose starting value reaches the block read is only
// moved if it is above the interpolation inputs and nothing but block reads see it (a slot the instruction doesn't
// use). An operand naming several members (a multi-register one) moves all of them: when it is in some members' webs
// only, it joins the others' too (their definitions reaching it, or its write) and the plan is made again
// (nvram178: three shaders failed that check). Plans the changes; false if the block can't be moved (fail:
// member << 8 | why).
struct RegChange {
	uint32_t at;
	VgprField field;
	uint32_t reg;
	uint32_t voted;  // members (bit = index in the block) moving this operand
};

static uint8_t webKind[MaxShaderDwords], webDef[MaxShaderDwords], webIn[MaxShaderDwords], webSeen[MaxShaderDwords],
	webEntry[MaxShaderDwords];
static uint32_t webStack[MaxShaderDwords + MaxShaderDwords / 4];

// With fail code 5: the registers [blockGrowBase, blockGrowEnd) the block and that operand cover together.
static uint32_t blockGrowBase, blockGrowEnd;

static bool planBlockMove(const uint32_t *code, uint32_t end, uint32_t at, uint32_t p2, uint32_t vI, uint32_t blockAt,
	uint32_t base, uint32_t k, uint32_t F, RegChange *out, uint32_t &outNum, uint32_t outMax, uint32_t &fail) {
	VgprField f[8];
	uint32_t n;
	outNum = 0;
	fail = 1;
	if (!cfgBuild(code, end))
		return false;
	uint32_t maxInterpSource = 0;
	for (uint32_t i = 0; i < end; i += gcn3InstLength(code + i, end - i))
		if ((code[i] >> 26) == 0x35 && ((code[i] >> 16) & 3) != 2 && (code[i] & 0xFF) > maxInterpSource)
			maxInterpSource = code[i] & 0xFF;
	auto isInterpSource = [&](uint32_t i, const VgprField &field) {
		return (code[i] >> 26) == 0x35 && field.shift == 0;
	};
	bool full = false;  // addChange ran out of room (fail code 11) rather than meeting a conflict (7)
	auto addChange = [&](uint32_t i, const VgprField &field, uint32_t reg, uint32_t member) {
		for (uint32_t c = 0; c < outNum; c++) {
			if (out[c].at != i || out[c].field.dword != field.dword || out[c].field.shift != field.shift)
				continue;
			if (out[c].reg != reg)
				return false;
			out[c].voted |= 1U << (member - base);
			return true;
		}
		if (outNum == outMax) {
			full = true;
			return false;
		}
		out[outNum++] = {i, field, reg, 1U << (member - base)};
		return true;
	};
	// Operands a member must move with the others: instruction position, bit 31 set for its write.
	static uint32_t forced[32][32];  // up to 32 members: the voted masks are 32-bit
	uint32_t forcedNum[32] {};
	if (k > arrsize(forced))
		return false;
	for (uint32_t attempt = 0;; attempt++) {
	outNum = 0;
	for (uint32_t r = base; r < base + k; r++) {
		uint32_t member = (r - base) << 8;
		// How each instruction writes r: 0 not, 1 writes it, 2 may write it (a register range whose size is a bound).
		for (uint32_t i = 0; i < end; i += gcn3InstLength(code + i, end - i)) {
			uint8_t kind = 0;
			gcn3VgprFields(code + i, end - i, f, n);
			for (uint32_t q = 0; q < n; q++) {
				uint32_t v = fieldValue(code + i, f[q]);
				if (f[q].write && v <= r && r < v + f[q].count)
					kind = f[q].count > 1 && !f[q].exact && v != r ? (kind ? kind : 2) : 1;
			}
			webKind[i] = kind;
		}
		memset(webDef, 0, end);
		bool entry = false, overflow = false;
		// The definitions whose value can reach instruction x join the web (the starting value: entry). True if any
		// was new.
		auto join = [&](uint32_t x) {
			bool grew = false;
			uint32_t sp = 0;
			memset(webSeen, 0, end);
			auto push = [&](uint32_t y) {
				if (sp == arrsize(webStack))
					overflow = true;
				else
					webStack[sp++] = y;
			};
			auto preds = [&](uint32_t y) {
				if (y == 0 && !entry) {
					entry = true;
					grew = true;
				}
				uint32_t p = cfgPrev[y];
				if (p != 0xFFFFFFFF) {
					bool fall;
					int32_t target;
					cfgStep(code, p, fall, target);
					if (fall)
						push(p);
				}
				for (uint32_t b = cfgFirstIn[y]; b; b = cfgNextIn[b - 1])
					push(cfgBranchFrom[b - 1]);
			};
			preds(x);
			while (sp && !overflow) {
				uint32_t p = webStack[--sp];
				if (webSeen[p])
					continue;
				webSeen[p] = 1;
				if (webKind[p] && !webDef[p]) {
					webDef[p] = 1;
					grew = true;
				}
				if (webKind[p] != 1)
					preds(p);
			}
			return grew;
		};
		// Which values of r reach each instruction: 1 the web's, 2 any other.
		auto flow = [&]() {
			memset(webIn, 0, end);
			webIn[0] = entry ? 1 : 2;
			for (uint32_t pass = 0; pass < 1024; pass++) {
				bool changed = false;
				for (uint32_t i = 0; i < end;) {
					uint32_t len = gcn3InstLength(code + i, end - i);
					uint8_t in = webIn[i];
					if (in) {
						uint8_t mine = webDef[i] ? 1 : 2;
						uint8_t o = webKind[i] == 1 ? mine : webKind[i] == 2 ? static_cast<uint8_t>(in | mine) : in;
						bool fall;
						int32_t target;
						cfgStep(code, i, fall, target);
						if (fall && i + len < end && (webIn[i + len] | o) != webIn[i + len]) {
							webIn[i + len] |= o;
							changed = true;
						}
						if (target >= 0 && (webIn[target] | o) != webIn[target]) {
							webIn[target] |= o;
							changed = true;
						}
					}
					i += len;
				}
				if (!changed)
					return true;
			}
			return false;
		};
		join(blockAt);
		for (uint32_t s = 0; s < forcedNum[r - base]; s++) {
			uint32_t site = forced[r - base][s] & 0x7FFFFFFF;
			if (forced[r - base][s] & 0x80000000)
				webDef[site] = 1;
			else
				join(site);
		}
		bool settled = false;
		for (uint32_t round = 0; round < 256 && !overflow; round++) {
			if (!flow()) {
				fail = member | 8;
				return false;
			}
			bool grew = false;
			for (uint32_t i = 0; i < end && !overflow; i += gcn3InstLength(code + i, end - i)) {
				uint8_t in = webIn[i];
				if (!in)
					continue;
				gcn3VgprFields(code + i, end - i, f, n);
				for (uint32_t q = 0; q < n; q++) {
					uint32_t v = fieldValue(code + i, f[q]);
					if (!f[q].read || isInterpSource(i, f[q]) || !(v <= r && r < v + f[q].count))
						continue;
					// One operand read and written (v_mac, a p2's accumulator): both move or neither.
					if ((in & 1) && f[q].write && !webDef[i]) {
						webDef[i] = 1;
						grew = true;
					}
					if ((in & 2) && ((in & 1) || (f[q].write && webDef[i])))
						grew |= join(i);
				}
			}
			if (!grew) {
				settled = !overflow;
				break;
			}
		}
		if (overflow) {
			fail = member | 2;
			return false;
		}
		if (!settled) {
			fail = member | 8;
			return false;
		}
		if (r == vI && !(webDef[at] && webDef[p2])) {
			fail = member | 4;
			return false;
		}
		// An image address slot holding nothing but an interpolation input on entry (Tahoe: 13 shaders, 0x303,
		// image_sample_b v0..v2 = bias, u, v counted as v0..v3, v3 being the J that the shader's own v_interp_p2 reads),
		// past the registers a 2D sample of that kind needs (two coordinates, plus one each for an offset, bias, compare
		// value, LOD and clamp, four for derivatives): a raw barycentric is never a slice or third coordinate, so it is a
		// slot the instruction doesn't use. Leave the input where it is and let the moved address read a fresh register
		// there (inside the raised allocation, never written). Samples and gathers only (MIMG 0x20-0x5F).
		bool webDefined = false;
		for (uint32_t i = 0; i < end && !webDefined; i++)
			webDefined = webDef[i];
		uint32_t imageOp = (code[blockAt] >> 18) & 0x7F, used2d = 0;
		if (imageOp >= 0x20 && imageOp <= 0x5F) {
			static const uint8_t variant[8] {0, 1, 4, 5, 1, 1, 2, 0};  // -, cl, d, d_cl, l, b, b_cl, lz
			used2d = 2 + variant[imageOp & 7] + ((imageOp >> 3) & 1) + ((imageOp >> 4) & 1);
		}
		if (entry && !webDefined && r != vI && r <= maxInterpSource && !forcedNum[r - base] &&
			(code[blockAt] >> 26) == 0x3C && blockAt + 1 < end && used2d &&
			r >= (code[blockAt + 1] & 0xFF) + used2d) {
			uint32_t vaddr = code[blockAt + 1] & 0xFF;
			gcn3VgprFields(code + blockAt, end - blockAt, f, n);
			bool voted = false;
			for (uint32_t q = 0; q < n && !voted; q++)
				if (f[q].dword == 1 && f[q].shift == 0 && f[q].read && vaddr >= base && vaddr <= r &&
					r < vaddr + f[q].count) {
					if (!addChange(blockAt, f[q], F + vaddr - base, r)) {
						fail = member | (full ? 11 : 7);
						return false;
					}
					voted = true;
				}
			if (voted)
				continue;
		}
		if (entry && (r == vI || r <= maxInterpSource)) {
			fail = member | 3;  // an input (or the result's register on entry) would move
			return false;
		}
		if (entry) {
			// Where the starting value itself can be read (webEntry bit 0): the definitions in the web don't count.
			memcpy(webSeen, webDef, end);
			memset(webDef, 0, end);
			bool flowed = flow();
			memcpy(webEntry, webIn, end);
			memcpy(webDef, webSeen, end);
			if (!flowed || !flow()) {
				fail = member | 8;
				return false;
			}
		}
		for (uint32_t i = 0; i < end; i += gcn3InstLength(code + i, end - i)) {
			uint8_t in = webIn[i];
			if (!(in & 1) && !webDef[i])
				continue;
			gcn3VgprFields(code + i, end - i, f, n);
			for (uint32_t q = 0; q < n; q++) {
				uint32_t v = fieldValue(code + i, f[q]);
				if (!(v <= r && r < v + f[q].count))
					continue;
				if (isInterpSource(i, f[q])) {
					// A v_interp's I/J stays where it is: it must not read a value that moves (the hazard's own p1
					// only could, round a loop, and there it wants I, which the move leaves in vI).
					if ((in & 1) && !(r == vI && i == at)) {
						fail = member | 10;
						return false;
					}
					continue;
				}
				bool moveRead = f[q].read && (in & 1), moveWrite = f[q].write && webDef[i];
				if (!moveRead && !moveWrite)
					continue;
				if ((moveRead && (in & 2)) || (f[q].read && f[q].write && moveRead != moveWrite)) {
					fail = member | 6;
					return false;
				}
				bool inBlock = v >= base && v + f[q].count <= base + k;
				// Starting values may reach multi-register reads inside the block only: the slots of an image address the
				// decoder counts as 4 registers (GFX8 doesn't encode how many the texture uses) and the instruction
				// doesn't use. Not only the whole-block read: once a block has grown (Sonoma: three blur shaders, 0x203,
				// image_sample addresses v0..v1 and v2..v3 counted as v0..v3 and v2..v5), its original reads cover part
				// of it.
				if (entry && moveRead && (webEntry[i] & 1) && !(f[q].count > 1 && inBlock)) {
					fail = member | 3;  // the starting value is read outside the block reads: not an unused slot
					return false;
				}
				if (f[q].count > 1 && !inBlock) {
					// The caller can grow the block to cover this operand and plan again.
					blockGrowBase = v < base ? v : base;
					blockGrowEnd = v + f[q].count > base + k ? v + f[q].count : base + k;
					fail = member | 5;  // a register range reaching outside the block
					return false;
				}
				if (!addChange(i, f[q], F + (f[q].count > 1 ? v : r) - base, r)) {
					fail = member | (full ? 11 : 7);
					return false;
				}
			}
		}
	}
	bool more = false;
	for (uint32_t c = 0; c < outNum; c++) {
		if (out[c].field.count < 2)
			continue;
		uint32_t first = fieldValue(code + out[c].at, out[c].field) - base;
		uint32_t missing = (((1U << out[c].field.count) - 1) << first) & ~out[c].voted;
		for (uint32_t m = 0; m < k; m++) {
			if (!(missing & (1U << m)))
				continue;
			// An operand moving some of its registers only: its other members take it into their webs.
			uint32_t sites[2], siteNum = 0;
			if (out[c].field.read)
				sites[siteNum++] = out[c].at;
			if (out[c].field.write)
				sites[siteNum++] = out[c].at | 0x80000000;
			for (uint32_t s = 0; s < siteNum; s++) {
				bool known = false;
				for (uint32_t q = 0; q < forcedNum[m] && !known; q++)
					known = forced[m][q] == sites[s];
				if (known)
					continue;
				if (forcedNum[m] == arrsize(forced[m]) || attempt == 16) {
					fail = (m << 8) | 9;
					return false;
				}
				forced[m][forcedNum[m]++] = sites[s];
				more = true;
			}
		}
	}
	if (!more)
		return outNum != 0;
	}
}

static uint32_t fixInterpInShader(uint32_t *code, uint32_t got, uint32_t rsrc1, uint32_t &hazards, InterpSkip &skip,
	bool allowBump, uint32_t &newVgprs) {
	newVgprs = 0;
	interpFix.lastSkipWhy = 0;
	hazards = 0;
	skip = SkipNone;
	// Walk once: end, highest VGPR touched, and the hazards.
	uint32_t end = gcn3ProgramEnd(code, got), maxVgpr = 0, badAt[8], badNum = 0;
	VgprField f[8];
	uint32_t n;
	for (uint32_t i = 0; i < (end ? end : got);) {
		uint32_t w = code[i];
		uint32_t len = gcn3InstLength(code + i, got - i);
		if (!gcn3VgprFields(code + i, got - i, f, n)) {
			interpFix.lastUnknownWord = w;
			interpFix.lastUnknownAt = i;
			skip = SkipUnknownEncoding;
			return 0;
		}
		for (uint32_t k = 0; k < n; k++) {
			uint32_t v = fieldValue(code + i, f[k]);
			if (v < 256 && v + f[k].count - 1 > maxVgpr)
				maxVgpr = v + f[k].count - 1;
		}
		if ((w >> 26) == 0x35 && ((w >> 16) & 3) == 0 && ((w >> 18) & 0xFF) == (w & 0xFF)) {
			hazards++;
			if (badNum < arrsize(badAt))
				badAt[badNum++] = i;
		}
		i += len;
	}
	if (!hazards)
		return 0;
	if (!end) {
		skip = SkipNoEnd;
		return 0;
	}
	uint32_t allocated = ((rsrc1 & 0x3F) + 1) * 4;
	uint32_t fixedHere = 0;
	for (uint32_t b = 0; b < badNum; b++) {
		uint32_t at = badAt[b], w = code[at];
		uint32_t vI = w & 0xFF, attrChan = (w >> 8) & 0xFF;
		// The matching p2 (same attribute and channel, writing vI); nothing may touch vI in between.
		uint32_t p2 = 0;
		for (uint32_t i = at + gcn3InstLength(code + at, end - at); i < end;) {
			uint32_t x = code[i];
			if ((x >> 26) == 0x35 && ((x >> 16) & 3) == 1 && ((x >> 8) & 0xFF) == attrChan &&
				((x >> 18) & 0xFF) == vI) {
				p2 = i;
				break;
			}
			gcn3VgprFields(code + i, end - i, f, n);
			for (uint32_t k = 0; k < n; k++) {
				uint32_t v = fieldValue(code + i, f[k]);
				if (v <= vI && vI < v + f[k].count) {
					skip = SkipWriteBetween;
					return fixedHere;
				}
			}
			i += gcn3InstLength(code + i, end - i);
		}
		if (!p2) {
			skip = SkipNoP2;
			return fixedHere;
		}
		// Which reads of vI see this hazard's result? Straight-line code: those after p2 until vI is written again. With
		// branches anywhere, follow the control flow: for each instruction, the set of definitions of vI that can reach
		// it (bit 0: any other, or vI's value on entry; bit 1: this result). A read reached only by the result is
		// renamed, one reached by both can't be (the shader is left alone). nvram170: the 7 hazards left were in shaders
		// with loops that write vI again.
		interpFix.lastShaderLen = end;
		interpFix.lastHazardAt = at;
		bool branchy = false;
		for (uint32_t i = 0; i < end;) {
			uint32_t x = code[i];
			if ((x >> 23) == 0x17F) {
				uint32_t op = (x >> 16) & 0x7F;
				branchy |= op == 0x02 || (op >= 0x04 && op <= 0x09) || (op >= 0x17 && op <= 0x1A);
			} else if ((x >> 23) == 0x17D && (((x >> 8) & 0xFF) == 0x1D || ((x >> 8) & 0xFF) == 0x1E)) {
				interpFix.lastSkipWhy = 4;  // s_setpc / s_swappc: control flow we can't follow
				skip = SkipBranch;
				return fixedHere;
			}
			i += gcn3InstLength(code + i, end - i);
		}
		static uint32_t renameAt[64];
		static VgprField renameField[64];
		uint32_t renames = 0;
		uint32_t blockBase = 0, blockCount = 0, blockAt = 0;
		if (branchy) {
			static uint8_t state[MaxShaderDwords], start[MaxShaderDwords];
			memset(state, 0, end);
			memset(start, 0, end);
			for (uint32_t i = 0; i < end; i += gcn3InstLength(code + i, end - i))
				start[i] = 1;
			state[0] = 1;
			bool changed = true;
			for (uint32_t pass = 0; changed && pass < 256; pass++) {
				changed = false;
				for (uint32_t i = 0; i < end;) {
					uint32_t len = gcn3InstLength(code + i, end - i);
					uint8_t in = state[i], out = in;
					if (in) {
						if (i == at || i == p2) {
							out = 2;
						} else {
							gcn3VgprFields(code + i, end - i, f, n);
							for (uint32_t k = 0; k < n; k++) {
								uint32_t v = fieldValue(code + i, f[k]);
								if (!f[k].write || !(v <= vI && vI < v + f[k].count))
									continue;
								if (f[k].count > 1 && !f[k].exact && v != vI) {
									interpFix.lastSkipWhy = 5;  // a write that may or may not cover vI
									interpFix.lastUnknownWord = code[i];  // which instruction (Tahoe: 119 shaders)
									skip = SkipRange;
									return fixedHere;
								}
								out = 1;
							}
						}
						uint32_t x = code[i];
						bool fall = true;
						int32_t target = -1;
						if ((x >> 23) == 0x17F) {
							uint32_t op = (x >> 16) & 0x7F;
							int32_t to = static_cast<int32_t>(i) + 1 + static_cast<int16_t>(x & 0xFFFF);
							if (op == 0x01) {  // s_endpgm
								fall = false;
							} else if (op == 0x02) {  // s_branch
								fall = false;
								target = to;
							} else if ((op >= 0x04 && op <= 0x09) || (op >= 0x17 && op <= 0x1A)) {
								target = to;
							}
						}
						auto flow = [&](int32_t to) {
							if (to >= 0 && to < static_cast<int32_t>(end) && start[to] && (state[to] | out) != state[to]) {
								state[to] |= out;
								changed = true;
							}
						};
						if (fall)
							flow(static_cast<int32_t>(i + len));
						if (target >= 0)
							flow(target);
					}
					i += len;
				}
			}
			// The result reaching p1 (or another v_interp) through a loop is fine: after the fix it lives in vF, and vI
			// keeps I, which is what a v_interp reads there. Those I reads are never renamed.
			for (uint32_t i = 0; i < end;) {
				uint32_t len = gcn3InstLength(code + i, end - i);
				if (i != at && i != p2 && (state[i] & 2)) {
					gcn3VgprFields(code + i, end - i, f, n);
					for (uint32_t k = 0; k < n; k++) {
						uint32_t v = fieldValue(code + i, f[k]);
						if (!f[k].read || !(v <= vI && vI < v + f[k].count))
							continue;
						if ((code[i] >> 26) == 0x35 && f[k].shift == 0)
							continue;  // a v_interp's I/J source
						// A register block read, or a read that also sees other values of vI: move the result's whole web
						// (planBlockMove; a "block" of one register for a mixed read).
						if (f[k].count > 1 && (blockCount <= 1 || (v == blockBase && f[k].count == blockCount))) {
							if (blockCount <= 1) {
								blockBase = v;
								blockCount = f[k].count;
								blockAt = i;
							}
							continue;  // the same block read again: the block move renames it too
						}
						if (f[k].count == 1 && ((state[i] & 1) || renames == arrsize(renameAt))) {
							if (!blockCount) {
								blockBase = vI;
								blockCount = 1;
								blockAt = i;
							}
							continue;
						}
						if ((state[i] & 1) || f[k].count > 1 || renames == arrsize(renameAt)) {
							interpFix.lastSkipWhy = 3;  // a read that sees the result on some paths only
							skip = (state[i] & 1) ? SkipBranch : SkipRange;
							return fixedHere;
						}
						renameAt[renames] = i;
						renameField[renames++] = f[k];
					}
				}
				i += len;
			}
		} else {
			for (uint32_t i = p2 + 1; i < end;) {
				uint32_t len = gcn3InstLength(code + i, end - i);
				gcn3VgprFields(code + i, end - i, f, n);
				bool written = false;
				for (uint32_t k = 0; k < n; k++) {
					uint32_t v = fieldValue(code + i, f[k]);
					if (!(v <= vI && vI < v + f[k].count))
						continue;
					if (f[k].count > 1 && f[k].read &&
						(!blockCount || (v == blockBase && f[k].count == blockCount))) {
						if (!blockCount) {
							blockBase = v;
							blockCount = f[k].count;
							blockAt = i;
						}
						continue;
					}
					if (f[k].count > 1 && (f[k].read || (v != vI && !f[k].exact))) {
						skip = SkipRange;
						return fixedHere;
					}
					if (f[k].read) {
						if (renames == arrsize(renameAt)) {
							skip = SkipRange;
							return fixedHere;
						}
						renameAt[renames] = i;
						renameField[renames++] = f[k];
					}
					if (f[k].write)
						written = true;
				}
				if (written)
					break;
				i += len;
			}
		}
		if (blockCount) {
			// Move the whole block to fresh registers above everything the shader uses (raising the allocation if
			// needed). A multi-register operand that covers a member and registers outside the block (fail code 5;
			// Monterey: three shaders, 0x305) can't move in part: grow the block over it and plan again, the extra
			// registers then being members like the others (refused as usual if they can't move), up to 32 registers: the
			// decoder counts every image address as 4 registers, so overlapping addresses chain (Sonoma: v0..v16).
			uint32_t F = maxVgpr + 1, need = 0, newAllocated = allocated;
			// Room for every operand a block move touches: Sonoma's 144 KB lock screen shader failed at 256 (code 7).
			static RegChange changes[4096];
			uint32_t changeNum = 0, blockFail = 0;
			for (uint32_t grow = 0;; grow++) {
				need = F + blockCount;
				newAllocated = allocated;
				if (need > allocated) {
					if (!allowBump || need > 256) {
						interpFix.lastSkipWhy = 6;
						skip = SkipRange;
						return fixedHere;
					}
					newAllocated = (need + 3) & ~3U;
				}
				if (planBlockMove(code, end, at, p2, vI, blockAt, blockBase, blockCount, F, changes, changeNum,
						arrsize(changes), blockFail))
					break;
				if ((blockFail & 0xFF) == 5 && grow < 32 && blockGrowEnd - blockGrowBase <= 32 &&
					(blockGrowBase != blockBase || blockGrowEnd != blockBase + blockCount)) {
					blockBase = blockGrowBase;
					blockCount = blockGrowEnd - blockGrowBase;
					interpFix.blockGrows++;
					continue;
				}
				interpFix.lastBlockFail = blockFail;
				interpFix.lastSkipWhy = 7;  // see planBlockMove's fail codes
				skip = SkipRange;
				return fixedHere;
			}
			for (uint32_t c = 0; c < changeNum; c++)
				setField(code + changes[c].at, changes[c].field, changes[c].reg);
			if (newAllocated != allocated) {
				allocated = newAllocated;
				newVgprs = allocated / 4 - 1;
			}
			maxVgpr = F + blockCount - 1;
			interpFix.blockMoves++;
			fixedHere++;
			continue;
		}
		// A register for the result: above the highest one used if the allocation has room, else one that is dead here
		// (nothing touches it until the last renamed read, and its next use after that is a write, or there is none).
		// That needs straight-line code: no backward branch anywhere in the shader.
		uint32_t regionEnd = renames ? renameAt[renames - 1] : p2;
		uint32_t vF = maxVgpr + 1;
		if (vF >= allocated || vF > 255) {
			vF = 0x100;
			for (uint32_t r = 0; r < allocated && r < 256 && vF == 0x100 && !branchy; r++) {
				if (r == vI)
					continue;
				bool decided = false, usable = true;
				for (uint32_t i = at; i < end && !decided;) {
					gcn3VgprFields(code + i, end - i, f, n);
					bool reads = false, writes = false;
					for (uint32_t k = 0; k < n; k++) {
						uint32_t v = fieldValue(code + i, f[k]);
						if (v <= r && r < v + f[k].count) {
							reads |= f[k].read;
							writes |= f[k].write;
						}
					}
					if (reads || writes) {
						decided = true;
						usable = i > regionEnd && !reads;
					}
					i += gcn3InstLength(code + i, end - i);
				}
				if (usable)
					vF = r;
			}
			if (vF == 0x100 && allowBump && allocated + 4 <= 256) {
				// Give the shader four more VGPRs (the caller raises SPI_SHADER_PGM_RSRC1_PS.VGPRS in the command buffer).
				vF = allocated;
				allocated += 4;
				newVgprs = allocated / 4 - 1;
			} else if (vF == 0x100) {
				skip = SkipNoFreeVgpr;
				return fixedHere;
			} else {
				interpFix.deadRegs++;
			}
		}
		// Apply: the p1 and p2 write vF, the reads use vF.
		code[at] = (code[at] & ~(0xFFU << 18)) | (vF << 18);
		code[p2] = (code[p2] & ~(0xFFU << 18)) | (vF << 18);
		for (uint32_t r = 0; r < renames; r++)
			setField(code + renameAt[r], renameField[r], vF);
		if (vF > maxVgpr)
			maxVgpr = vF;
		fixedHere++;
	}
	return fixedHere;
}

// Seen pixel shaders: VMID, address, hash of the code as we last saw (or left) it, the VGPRS field it needs when we
// gave it more registers (0: its own), and whether a fix is still pending (no register without raising the allocation,
// which only the submit hook can do).
// GPU copies seen in command buffers (DMA_DATA, WRITE_DATA, COPY_DATA to memory): the shader fixer patches a shader at
// submission, from the CPU, but the GPU runs the command buffers queued before it later. A copy that lands on the
// shader after the patch (in an earlier command buffer still queued, or in the same one before the draw) puts the
// unfixed code back for that submission, and the fixer finds it reverted at the next bind: a glitch in some frames
// only (Sequoia: 24 reverts in a boot). Each GPU cache is invalidated at the start of every submission (Apple's frame
// template: SURFACE_SYNC with CP_COHER_CNTL 0xFEEFFFCF, I$ K$ TCL1 TC WB), so stale caches aren't the problem; the
// order is. These record the copies so a bind or a revert can be matched to one.
struct GpuCopy {
	uint64_t dst, src, ms;
	uint32_t bytes, op, vmid, ctrl;
};
static GpuCopy gpuCopies[256];
static uint32_t sdmaHookCalls, sdmaIbs, sdmaUnknown;  // sDMA submissions read (wrapCommitSdma)
static uint32_t sdmaUnknownHdr[8], sdmaUnknownHdrs;    // the first distinct headers it stopped at
static uint32_t gpuCopyNext, gpuCopyPackets, gpuCopyBindHits, gpuCopyRevertHits, gpuCopyLogged;

static void applySourcePatches(uint32_t vmid, uint64_t src, uint64_t dst, uint32_t bytes);

static void noteGpuCopy(uint32_t vmid, uint32_t op, uint32_t ctrl, uint64_t src, uint64_t dst, uint32_t bytes) {
	__atomic_fetch_add(&gpuCopyPackets, 1, __ATOMIC_RELAXED);
	gpuCopies[__atomic_fetch_add(&gpuCopyNext, 1, __ATOMIC_RELAXED) % arrsize(gpuCopies)] = {dst & 0xFFFFFFFFFFFFULL, src & 0xFFFFFFFFFFFFULL, uptimeMs(), bytes,
		op, vmid, ctrl};
}

// The most recent copy (within maxAge ms) whose destination overlaps the shader's first 4 KB.
static const GpuCopy *recentCopyTo(uint32_t vmid, uint64_t va, uint64_t maxAge = 200) {
	uint64_t now = uptimeMs();
	const GpuCopy *best = nullptr;
	for (auto &c : gpuCopies)
		if (c.bytes && c.vmid == vmid && now - c.ms <= maxAge && c.dst < va + 0x1000 && va < c.dst + c.bytes &&
			(!best || c.ms >= best->ms))
			best = &c;
	return best;
}

struct SeenShader {
	uint64_t va;
	uint32_t vmid, hash, vgprs;
	bool pending;
	uint32_t probeAt, probeVal;  // a fixed instruction and its fixed value (probeAt 0xFFFFFFFF: none)
	uint64_t fixedMs;            // when it was (last) fixed
	uint64_t fixedPhys;          // its first page's physical address then (a revert on another page: evicted, restored)
	uint32_t fixedPte, fixedSdmaWptr;  // that page's PTE flags, and the sDMA0 ring's write pointer then
	// The fix as dwords (index, original, fixed), replayed onto an sDMA copy's source (applySourcePatches);
	// patchCount 0xFFFF: more than fit, not replayed (srcFixOverflow counts the copies that met one).
	uint16_t patchCount;         // 0xFFFE: the list is in bigPatches[bigSlot]
	uint8_t bigSlot;
	uint32_t patchAt[96], patchOld[96], patchNew[96];
};
static SeenShader seenShaders[512];
// Fixes too large for a SeenShader's list (Sonoma's 144 KB lock screen shader: 329 dwords): a few slots, reused in turn.
struct BigPatch {
	uint64_t va;
	uint32_t vmid, count;
	uint32_t at[1024], oldValue[1024], newValue[1024];
};
static BigPatch bigPatches[8];
static uint32_t bigPatchNext;
static uint32_t seenNum, seenNext;
static uint32_t ibHookCalls, ibHookIbs, ibHookRsrcBumps, ibHookLastVmid, ibHookLastFlags, ibHookBusy, ibHookNoMap,
	ibHookRaiseOnly, ibHookBinds, ibHookMemoHits;
// Pixel shader binds answered during the current lock hold (vmCacheGen): address, VMID, RSRC1 in and out.
struct BindMemo {
	uint64_t va;
	uint32_t vmid, rsrcIn, rsrcOut, gen;
	bool allowBump;
};
static BindMemo bindMemo[16];
static uint32_t bindMemoNext;

static uint64_t absToMs(uint64_t abs) {
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(abs, &ns);
	return ns / 1000000;
}

static uint32_t hookCallCount() {
	return __atomic_load_n(&ibHookCalls, __ATOMIC_RELAXED);
}

static uint32_t interpFixedCount() {
	return interpFix.fixed;
}

// Metal is drawing (the submit hook has seen work): the watcher's start-up diagnostics can quieten down.
static bool desktopRunning() {
	return __atomic_load_n(&ibHookCalls, __ATOMIC_RELAXED) > 100;
}
static uint64_t ibHookNs;

// Shaders given more VGPRs: every command buffer that binds one must raise RSRC1_PS, even after the seen table has
// forgotten it or the code has changed (more VGPRs than needed is always safe). Append-only, readable without the lock.
struct RaisedShader {
	uint64_t va;
	uint32_t vmid, vgprs;
};
static RaisedShader raisedShaders[1024];
// Shaders the fix had to leave alone: address, length, hazard position, reason, detail (for the report).
struct SkippedShader {
	uint64_t va;
	uint32_t len, at, reason, why, word;
};
static SkippedShader skippedShaders[12];
static uint32_t skippedNum;
static uint32_t raisedNum;

static uint32_t raisedVgprs(uint32_t vmid, uint64_t va) {
	uint32_t n = __atomic_load_n(&raisedNum, __ATOMIC_ACQUIRE), best = 0;
	for (uint32_t i = 0; i < n; i++)
		if (raisedShaders[i].va == va && raisedShaders[i].vmid == vmid && raisedShaders[i].vgprs > best)
			best = raisedShaders[i].vgprs;
	return best;
}

static void addRaised(uint32_t vmid, uint64_t va, uint32_t vgprs) {
	if (raisedVgprs(vmid, va) >= vgprs)
		return;
	uint32_t n = raisedNum;
	if (n >= arrsize(raisedShaders))
		return;
	raisedShaders[n] = {va, vmid, vgprs};
	__atomic_store_n(&raisedNum, n + 1, __ATOMIC_RELEASE);
}

static uint32_t withRaise(uint32_t vmid, uint64_t va, uint32_t rsrc1) {
	uint32_t need = raisedVgprs(vmid, va);
	return need && (rsrc1 & 0x3F) < need ? (rsrc1 & ~0x3FU) | need : rsrc1;
}

static uint32_t codeHash(const uint32_t *code, uint32_t n) {
	uint32_t h = 2166136261U;
	for (uint32_t i = 0; i < n; i++)
		h = (h ^ code[i]) * 16777619U;
	return h;
}

// Read up to max dwords at a GPU VA (page by page through the VM); stops after s_endpgm if stopAtEnd. Only with
// interpLock held.
static uint32_t readGpuVa(uint32_t vmid, uint64_t va, uint32_t max, uint32_t *out, bool stopAtEnd) {
	uint32_t have = 0;
	while (have < max) {
		uint64_t a = va + have * 4ULL;
		uint32_t chunk = static_cast<uint32_t>((0x1000 - (a & 0xFFF)) / 4);
		if (chunk > max - have)
			chunk = max - have;
		uint32_t pte = 0;
		uint64_t phys = ibVaToPhysLocked(vmid, a, &pte);
		if (!phys || !ibReadPhys(phys, chunk * 4, out + have, (pte & 6) == 6))
			break;
		have += chunk;
		if (stopAtEnd) {
			bool ended = false;
			for (uint32_t q = have - chunk; q < have && !ended; q++)
				ended = out[q] == 0xBF810000;
			if (ended)
				break;
		}
	}
	return have;
}

// A pixel shader bound with RSRC1_PS = rsrc1: fix it if we haven't, and return the RSRC1 value it needs.
static uint32_t handlePixelShader(uint32_t vmid, uint64_t psVa, uint32_t rsrc1, bool allowBump) {
	// Up to MaxShaderDwords (512 KB) of code (nvram173/174: some pixel shaders run past 8 KB, a few past 32 KB; Sonoma's lock
	// screen shader past 128 KB).
	static uint32_t code[MaxShaderDwords], original[MaxShaderDwords];
	// Every bind of a known shader only costs its first 16 dwords (a fingerprint taken after fixing it); the whole
	// shader is read and fixed only when it is new here or its code changed.
	if (readGpuVa(vmid, psVa, 16, code, false) != 16)
		return rsrc1;
	uint32_t hash = codeHash(code, 16);
	SeenShader *entry = nullptr;
	for (uint32_t q = 0; q < seenNum && !entry; q++)
		if (seenShaders[q].va == psVa && seenShaders[q].vmid == vmid)
			entry = &seenShaders[q];
	if (entry && entry->hash == hash && !(entry->pending && allowBump)) {
		// The fix can lie past the fingerprint: an original re-uploaded at the same address (nvram171: a shader
		// bound with its hazard back) would look fixed. Check the fixed instruction itself.
		uint32_t now = 0;
		if (entry->probeAt == 0xFFFFFFFF ||
			(readGpuVa(vmid, psVa + entry->probeAt * 4ULL, 1, &now, false) == 1 && now == entry->probeVal))
			return withRaise(vmid, psVa, rsrc1);
		interpFix.probeReverts++;
		auto *c = recentCopyTo(vmid, psVa, ~0ULL);
		if (c)
			gpuCopyRevertHits++;
		if (interpFix.probeReverts <= 60)
		{
			// Paging test: the same virtual page now at another physical page (or another memory type) means the shader
			// was evicted and copied back, by the sDMA engine (not in the command buffers the copy check reads).
			uint32_t pteNow = 0;
			uint64_t physNow = ibVaToPhysLocked(vmid, psVa & ~0xFFFULL, &pteNow);
			uint32_t sdmaNow = gpuMmio ? gpuMmio[0x3484] : 0;
			if (physNow != entry->fixedPhys)
				interpFix.revertMoved++;
			if (interpFix.probeReverts <= 60)
				diskLogEvent("interp revert: ps %llx vmid %u, fixed %llu ms before; page %llx (pte %x) then, %llx (pte %x) "
					"now%s; sdma0 wptr %x -> %x; copy to it: %s op %x src %llx %u bytes %llu ms ago", psVa, vmid,
					uptimeMs() - entry->fixedMs, entry->fixedPhys, entry->fixedPte, physNow, pteNow,
					physNow != entry->fixedPhys ? " MOVED" : "", entry->fixedSdmaWptr, sdmaNow, c ? "yes" : "no",
					c ? c->op : 0, c ? c->src : 0, c ? c->bytes : 0, c ? uptimeMs() - c->ms : 0);
		}
	}
	uint32_t have = readGpuVa(vmid, psVa, arrsize(code), code, true);
	while (have && have < arrsize(code) && !gcn3ProgramEnd(code, have)) {
		// An early s_endpgm: the program goes on past the page it ended the read in.
		uint32_t more = readGpuVa(vmid, psVa + have * 4ULL, arrsize(code) - have, code + have, true);
		if (!more)
			break;
		have += more;
	}
	if (!have)
		return rsrc1;
	interpFix.shaders++;
	memcpy(original, code, have * 4);
	uint32_t hazards = 0, newVgprs = 0;
	InterpSkip skip = SkipNone;
	uint32_t fixedHere = fixInterpInShader(code, have, rsrc1, hazards, skip, allowBump, newVgprs);
	if (!entry || entry->hash != hash)
		interpFix.hazards += hazards;
	if (skip != SkipNone) {
		interpFix.skipped++;
		interpFix.lastSkip = skip;
		bool listed = false;
		for (uint32_t q = 0; q < skippedNum && !listed; q++)
			listed = skippedShaders[q].va == psVa;
		// No end found in 128 KB (Sonoma: 4 shaders, all at 64 KB boundaries, hazard in their first 64 dwords): dump
		// the start and count the control flow in what was read, to see how they end (an s_setpc / s_swappc into an
		// epilogue elsewhere would leave no s_endpgm at all).
		static uint32_t noEndDumped;
		if (!listed && skip == SkipNoEnd && noEndDumped < 4) {
			noEndDumped++;
			uint32_t endpgm = 0, setpc = 0, swappc = 0, branches = 0, firstSetpc = 0xFFFFFFFF, firstEnd = 0xFFFFFFFF;
			for (uint32_t i = 0; i < have;) {
				uint32_t w = code[i];
				if ((w >> 23) == 0x17F) {  // SOPP
					uint32_t op = (w >> 16) & 0x7F;
					if (op == 1) {
						endpgm++;
						if (firstEnd == 0xFFFFFFFF)
							firstEnd = i;
					} else if (op == 2 || (op >= 4 && op <= 9)) {
						branches++;
					}
				} else if ((w >> 23) == 0x17D) {  // SOP1: s_setpc_b64 0x1D, s_swappc_b64 0x1E
					uint32_t op = (w >> 8) & 0xFF;
					if (op == 0x1D) {
						setpc++;
						if (firstSetpc == 0xFFFFFFFF)
							firstSetpc = i;
					} else if (op == 0x1E) {
						swappc++;
					}
				}
				i += gcn3InstLength(code + i, have - i);
			}
			// Where it does end: scan on for the s_endpgm word, past what the fixer reads (up to 4 MB).
			uint32_t trueEnd = 0;
			static uint32_t more[1024];
			for (uint64_t at = have; !trueEnd && at < 0x100000;) {
				uint32_t n = readGpuVa(vmid, psVa + at * 4, arrsize(more), more, false);
				for (uint32_t q = 0; q < n && !trueEnd; q++)
					if (more[q] == 0xBF810000)
						trueEnd = static_cast<uint32_t>(at + q + 1);
				if (n < arrsize(more))
					break;
				at += n;
			}
			diskLogEvent("no end: ps %llx %u dw read; s_endpgm %u (first %d), s_setpc %u (first %d), s_swappc %u, branches %u, "
				"hazard at %u; first s_endpgm word further on: %u dw", psVa, have, endpgm, static_cast<int>(firstEnd), setpc,
				static_cast<int>(firstSetpc), swappc, branches, interpFix.lastHazardAt, trueEnd);
			for (uint32_t i = 0; i < 160 && i < have; i += 16)
				diskLogEvent("no end %llx +%u: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
					psVa, i, code[i], code[i + 1], code[i + 2], code[i + 3], code[i + 4], code[i + 5], code[i + 6],
					code[i + 7], code[i + 8], code[i + 9], code[i + 10], code[i + 11], code[i + 12], code[i + 13],
					code[i + 14], code[i + 15]);
		}
		// A block move refused (Monterey: three shaders, fail 0x305 before blocks could grow), or a write of unknown size
		// that may cover vI (Tahoe: 119 of 507 hazard shaders): the original code, for the offline fixer harness. Up to 3
		// shaders of each and 1024 dwords each.
		static uint32_t blockDumped, rangeDumped;
		if (!listed && skip == SkipRange && ((interpFix.lastSkipWhy == 7 && blockDumped < 3) ||
			(interpFix.lastSkipWhy == 5 && rangeDumped < 3))) {
			if (interpFix.lastSkipWhy == 5)
				rangeDumped++;
			else
				blockDumped++;
			diskLogEvent("block refused: ps %llx %u dw, hazard at %u, why %u, fail %x, word %08x", psVa, have,
				interpFix.lastHazardAt, interpFix.lastSkipWhy, interpFix.lastBlockFail, interpFix.lastUnknownWord);
			for (uint32_t i = 0; i < have && i < 1024; i += 16) {
				uint32_t w[16] {};
				for (uint32_t q = 0; q < 16 && i + q < have; q++)
					w[q] = original[i + q];
				diskLogEvent("block refused %llx +%u: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x "
					"%08x %08x", psVa, i, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12],
					w[13], w[14], w[15]);
			}
		}
		if (!listed && skippedNum < arrsize(skippedShaders))
			skippedShaders[skippedNum++] = {psVa, have,
				skip == SkipUnknownEncoding ? interpFix.lastUnknownAt : interpFix.lastHazardAt, static_cast<uint32_t>(skip),
				interpFix.lastSkipWhy, skip == SkipUnknownEncoding || interpFix.lastSkipWhy == 5 ? interpFix.lastUnknownWord :
				interpFix.lastSkipWhy == 7 ? interpFix.lastBlockFail : 0};
	}
	bool ok = true;
	if (fixedHere) {
		for (uint32_t q = 0; q < have; q++) {
			if (code[q] == original[q])
				continue;
			uint64_t phys = ibVaToPhysLocked(vmid, psVa + q * 4ULL);
			if (phys && ibWritePhys(phys, 4, &code[q]))
				interpFix.writes++;
			else
				ok = false;
		}
		if (ok)
			interpFix.fixed += fixedHere;
		else
			interpFix.writeFails++;
	}
	if (newVgprs && ok) {
		interpFix.bumps++;
		addRaised(vmid, psVa, newVgprs);
	}
	uint32_t print[16] {};
	readGpuVa(vmid, psVa, 16, print, false);
	uint32_t probeAt = 0xFFFFFFFF, probeVal = 0;
	for (uint32_t q = 0; q < have && fixedHere && ok; q++) {
		if (code[q] != original[q]) {
			probeAt = q;
			probeVal = code[q];
			break;
		}
	}
	uint32_t pteAtFix = 0;
	uint64_t physAtFix = ibVaToPhysLocked(vmid, psVa & ~0xFFFULL, &pteAtFix);
	SeenShader fresh {psVa, vmid, codeHash(print, 16), ok ? newVgprs : 0,
		skip == SkipNoFreeVgpr && !allowBump, probeAt, probeVal, uptimeMs(), physAtFix, pteAtFix,
		gpuMmio ? gpuMmio[0x3484] : 0};
	for (uint32_t q = 0; q < have && fixedHere && ok && fresh.patchCount < 0xFFFE; q++) {
		if (code[q] == original[q])
			continue;
		if (fresh.patchCount == arrsize(fresh.patchAt)) {
			uint32_t total = fresh.patchCount;
			for (uint32_t r = q; r < have; r++)
				total += code[r] != original[r];
			if (total <= arrsize(bigPatches[0].at)) {
				// Into the big pool: this shader's slot if it has one, else the next in turn.
				uint32_t slot = arrsize(bigPatches);
				for (uint32_t b = 0; b < arrsize(bigPatches) && slot == arrsize(bigPatches); b++)
					if (bigPatches[b].count && bigPatches[b].va == psVa && bigPatches[b].vmid == vmid)
						slot = b;
				if (slot == arrsize(bigPatches))
					slot = bigPatchNext++ % arrsize(bigPatches);
				auto &big = bigPatches[slot];
				big.va = psVa;
				big.vmid = vmid;
				big.count = 0;
				for (uint32_t r = 0; r < have && big.count < arrsize(big.at); r++) {
					if (code[r] == original[r])
						continue;
					big.at[big.count] = r;
					big.oldValue[big.count] = original[r];
					big.newValue[big.count] = code[r];
					big.count++;
				}
				fresh.patchCount = 0xFFFE;
				fresh.bigSlot = static_cast<uint8_t>(slot);
				break;
			}
			static uint32_t longLogged;
			if (longLogged++ < 10)
				diskLogEvent("fix too long to replay: ps %llx, %u dwords patched (room for %u)", psVa, total,
					static_cast<uint32_t>(arrsize(bigPatches[0].at)));
			fresh.patchCount = 0xFFFF;
			break;
		}
		fresh.patchAt[fresh.patchCount] = q;
		fresh.patchOld[fresh.patchCount] = original[q];
		fresh.patchNew[fresh.patchCount] = code[q];
		fresh.patchCount++;
	}
	if (entry) {
		*entry = fresh;
	} else if (seenNum < arrsize(seenShaders)) {
		seenShaders[seenNum++] = fresh;
	} else {
		seenShaders[seenNext] = fresh;
		seenNext = (seenNext + 1) % arrsize(seenShaders);
	}
	return withRaise(vmid, psVa, rsrc1);
}

// Draw census (-crnocensus disables): every distinct draw set-up the GPU is given, with when it was first seen, so a
// visual bug can be matched to the draws that appear when it does (the folder icons: flat squares, their shape mask
// not applied). Per draw: the pixel shader, render targets 0/1 (CB_COLORn_INFO, CB_COLOR0_ATTRIB), CB_TARGET_MASK,
// CB_COLOR_CONTROL, CB_BLEND0_CONTROL, DB_DEPTH_CONTROL, DB_STENCIL_CONTROL, DB_STENCILREFMASK, DB_STENCIL_INFO,
// DB_Z_INFO, DB_SHADER_CONTROL, SPI_SHADER_COL_FORMAT and the PS user data 0-3; if user data 0/1 look like a GPU
// address, the first two 8-dword texture descriptors there (dwords 1 and 3: format, swizzle). Context registers are
// tracked across command buffers (state set by context loads from memory isn't seen). Written by the submit hook (under
// interpLock: one writer), published with censusNum, logged by the watcher.
struct CensusEntry {
	uint64_t ps;
	uint32_t regs[13], userData[4], tex[4];
	uint32_t mimgCount, mimg[4];  // the shader's image instructions: op << 16 | dmask << 8 | first resource SGPR
	// The resource descriptors (T#) the first two image instructions use, read from memory at the draw: how they were
	// found (0 not found, 3 by running the scalar code before the instruction) and their 8 dwords.
	uint32_t descHow[2], desc[2][8];
	uint64_t firstMs;
	uint32_t count;
	bool logged;
};
static constexpr uint16_t censusRegs[13] {0x31C, 0x31D, 0x32B, 0x08E, 0x202, 0x1E0, 0x200, 0x10B, 0x10C, 0x011, 0x010,
	0x203, 0x1C5};
static CensusEntry census[1024];
static uint32_t censusNum, censusFull;
static uint32_t censusCtx[0x400], censusUserData[16], censusRsrc2;
// Which context / uconfig registers Metal has set (the dump tells "set to 0" from "never set").
static uint32_t censusCtxSet[0x400 / 32], censusUcfg[0x400], censusUcfgSet[0x400 / 32];
static bool ctxDumped;
  // RSRC2_PS: the user SGPR count
static uint64_t censusPs;
static int censusOff = -1;
static struct {
	uint64_t ps;
	uint32_t mimgAt, start, count, words[96];
	bool logged;
} censusCode[8];
static uint32_t censusCodeNum;

static void censusDraw(uint32_t vmid) {
	CensusEntry e {};
	e.ps = censusPs;
	for (uint32_t r = 0; r < arrsize(censusRegs); r++)
		e.regs[r] = censusCtx[censusRegs[r]];
	memcpy(e.userData, censusUserData, sizeof(e.userData));  // the first four
	uint32_t n = censusNum;
	for (uint32_t i = 0; i < n; i++) {
		if (census[i].ps == e.ps && !memcmp(census[i].regs, e.regs, sizeof(e.regs))) {
			census[i].count++;
			return;
		}
	}
	if (n == arrsize(census)) {
		censusFull++;
		return;
	}
	uint64_t va = static_cast<uint64_t>(e.userData[1]) << 32 | e.userData[0];
	if (e.userData[0] && !(e.userData[0] & 0xFF) && e.userData[1] && e.userData[1] < 0x10000) {
		uint32_t desc[16] {};
		if (readGpuVa(vmid, va, 16, desc, false) == 16) {
			e.tex[0] = desc[1];
			e.tex[1] = desc[3];
			e.tex[2] = desc[9];
			e.tex[3] = desc[11];
		}
	}
	// The pixel shader's image instructions (MIMG: op, the channels it reads, the resource descriptor's first SGPR).
	if (e.ps) {
		static uint32_t code[1024];
		uint32_t got = readGpuVa(vmid, e.ps, arrsize(code), code, true);
		uint32_t end = gcn3ProgramEnd(code, got);
		uint32_t mimgAt[2] {};
		if (!end)
			end = got;
		for (uint32_t i = 0; i < end;) {
			uint32_t w = code[i];
			if ((w >> 26) == 0x3C && i + 1 < got) {
				if (e.mimgCount < arrsize(e.mimg))
					e.mimg[e.mimgCount] = ((w >> 18) & 0x7F) << 16 | ((w >> 8) & 0xF) << 8 | (((code[i + 1] >> 16) & 0x1F) * 4);
				if (e.mimgCount < 2)
					mimgAt[e.mimgCount] = i;
				e.mimgCount++;
			}
			i += gcn3InstLength(code + i, got - i);
		}
		// The resource descriptors: run the scalar code before each image instruction (straight through, branches not
		// taken) on the draw's user SGPRs, doing its scalar loads from memory, so the T# SGPRs end up known however the
		// shader builds their address (8:59 log: the folder mask's T# is 32 bytes at a loaded table + user s8 * 32).
		static uint32_t sv[128];
		static bool sk[128];
		auto run = [&](uint32_t at) {
			memset(sk, 0, sizeof(sk));
			uint32_t users = (censusRsrc2 >> 1) & 0x1F;
			for (uint32_t r = 0; r < users && r < arrsize(censusUserData); r++) {
				sv[r] = censusUserData[r];
				sk[r] = true;
			}
			bool scc = false, sccKnown = false;
			auto drop = [&](uint32_t r, uint32_t n) {
				for (uint32_t q = r; q < r + n && q < 128; q++)
					sk[q] = false;
			};
			auto set = [&](uint32_t r, uint32_t v) {
				if (r < 128) {
					sv[r] = v;
					sk[r] = true;
				}
			};
			for (uint32_t i = 0; i < at && i < end;) {
				uint32_t w = code[i], len = gcn3InstLength(code + i, end - i), lit = i + 1 < end ? code[i + 1] : 0;
				auto src = [&](uint32_t s, uint32_t &v) {
					if (s < 128) {
						v = sv[s];
						return sk[s];
					}
					if (s <= 192)
						v = s - 128;
					else if (s <= 208)
						v = static_cast<uint32_t>(192 - static_cast<int32_t>(s));
					else if (s == 255)
						v = lit;
					else
						return false;
					return true;
				};
				uint32_t top9 = w >> 23;
				if ((w >> 30) == 2 && top9 == 0x17D) {  // SOP1
					uint32_t op = (w >> 8) & 0xFF, d = (w >> 16) & 0x7F, a = 0, b = 0;
					if (op == 0 && src(w & 0xFF, a)) {  // s_mov_b32
						set(d, a);
					} else if (op == 1 && (w & 0xFF) < 127 && src(w & 0xFF, a) && src((w & 0xFF) + 1, b)) {  // s_mov_b64
						set(d, a);
						set(d + 1, b);
					} else if (op == 28) {  // s_getpc_b64: the next instruction's address
						uint64_t pc = e.ps + (i + 1) * 4ULL;
						set(d, static_cast<uint32_t>(pc));
						set(d + 1, static_cast<uint32_t>(pc >> 32));
					} else {
						drop(d, 2);
						drop(126, 2);
						sccKnown = false;
					}
				} else if ((w >> 30) == 2 && top9 == 0x17E) {  // SOPC: compares (eq, lg, gt, ge, lt, le; i32 then u32)
					uint32_t op = (w >> 16) & 0x7F, a = 0, b = 0;
					sccKnown = op <= 11 && src(w & 0xFF, a) && src((w >> 8) & 0xFF, b);
					if (sccKnown) {
						int32_t x = static_cast<int32_t>(a), y = static_cast<int32_t>(b);
						bool u = op >= 6;
						switch (op % 6) {
							case 0: scc = a == b; break;
							case 1: scc = a != b; break;
							case 2: scc = u ? a > b : x > y; break;
							case 3: scc = u ? a >= b : x >= y; break;
							case 4: scc = u ? a < b : x < y; break;
							default: scc = u ? a <= b : x <= y; break;
						}
					}
				} else if ((w >> 30) == 2 && top9 == 0x17F) {  // SOPP: branches not followed
				} else if ((w >> 28) == 0xB) {  // SOPK
					uint32_t op = (w >> 23) & 0x1F, d = (w >> 16) & 0x7F, a = 0;
					uint32_t imm = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(w & 0xFFFF)));
					if (op == 0)  // s_movk_i32
						set(d, imm);
					else if (op == 14 && src(d, a))  // s_addk_i32
						set(d, a + imm);
					else if (op < 2 || (op > 13 && (op < 18 || op > 20)))  // writes d (not s_cmpk_*, s_setreg*)
						drop(d, 1);
					if (op >= 2 && op <= 14)
						sccKnown = false;
				} else if ((w >> 30) == 2) {  // SOP2
					uint32_t op = (w >> 23) & 0x7F, d = (w >> 16) & 0x7F, a = 0, b = 0;
					bool ok = src(w & 0xFF, a) && src((w >> 8) & 0xFF, b);
					uint64_t wide = static_cast<uint64_t>(a) + b;
					switch (ok ? op : 0xFF) {
						case 0:  // s_add_u32
							set(d, a + b);
							scc = wide >> 32;
							sccKnown = true;
							break;
						case 1:  // s_sub_u32
							set(d, a - b);
							scc = b > a;
							sccKnown = true;
							break;
						case 4:  // s_addc_u32
							if (sccKnown) {
								wide += scc;
								set(d, static_cast<uint32_t>(wide));
								scc = wide >> 32;
							} else {
								drop(d, 1);
							}
							break;
						case 10:  // s_cselect_b32
							if (sccKnown)
								set(d, scc ? a : b);
							else
								drop(d, 1);
							break;
						case 12: case 14: case 16: case 28: case 30: {  // s_and/or/xor/lshl/lshr_b32
							uint32_t v = op == 12 ? a & b : op == 14 ? a | b : op == 16 ? a ^ b : op == 28 ? a << (b & 31) : a >> (b & 31);
							set(d, v);
							scc = v != 0;
							sccKnown = true;
							break;
						}
						case 36:  // s_mul_i32
							set(d, a * b);
							break;
						default:
							drop(d, 2);
							drop(126, 2);
							sccKnown = false;
							break;
					}
				} else if ((w >> 26) == 0x30) {  // SMEM: s_load_dword* (0-4), s_buffer_load_dword* (8-12) done from memory
					uint32_t op = (w >> 18) & 0xFF, d = (w >> 6) & 0x7F, base = (w & 0x3F) * 2, off = 0;
					bool imm = (w >> 17) & 1;
					uint32_t n = op <= 4 ? 1U << op : op >= 8 && op <= 12 ? 1U << (op - 8) : 0;
					if (n) {
						uint32_t lo = 0, hi = 0, got = 0;
						bool ok = d + n <= 128 && src(base, lo) && src(base + 1, hi);
						if (ok && imm)
							off = lit & 0xFFFFF;
						else if (ok)
							ok = src(lit & 0xFF, off);
						if (ok) {
							uint64_t va = ((static_cast<uint64_t>(hi & 0xFFFF) << 32 | lo) + off) & ~3ULL;
							got = readGpuVa(vmid, va, n, sv + d, false);
						}
						for (uint32_t q = 0; q < n && d + q < 128; q++)
							sk[d + q] = q < got;
					} else if (!(op >= 16 && op <= 18) && !(op >= 24 && op <= 26)) {  // not a store
						drop(d, 2);
					}
				} else if ((w >> 25) == 0x3F) {  // VOP1: v_readfirstlane_b32 writes an SGPR
					if (((w >> 9) & 0xFF) == 2)
						drop((w >> 17) & 0xFF, 1);
				} else if ((w >> 25) == 0x3E) {  // VOPC: vcc
					drop(106, 2);
				} else if (!(w >> 31)) {  // VOP2: v_add/sub/subrev/addc/subb/subbrev_u32 write vcc
					uint32_t op = (w >> 25) & 0x3F;
					if (op >= 25 && op <= 30)
						drop(106, 2);
				} else if ((w >> 26) == 0x34) {  // VOP3: compares and readlane write [7:0], VOP3b carries to [14:8]
					uint32_t op = (w >> 16) & 0x3FF;
					if (op < 0x100 || op == 0x289)
						drop(w & 0xFF, op < 0x100 ? 2 : 1);
					else if ((op >= 0x119 && op <= 0x11E) || op == 0x1E0 || op == 0x1E1)
						drop((w >> 8) & 0x7F, 2);
				}
				i += len;
			}
		};
		for (uint32_t k = 0; k < 2 && k < e.mimgCount; k++) {
			uint32_t at = mimgAt[k], srsrc = ((code[at + 1] >> 16) & 0x1F) * 4;
			run(at);
			bool all = srsrc + 8 <= 128;
			for (uint32_t q = 0; q < 8 && all; q++)
				all = sk[srsrc + q];
			if (all) {
				memcpy(e.desc[k], sv + srsrc, sizeof(e.desc[k]));
				e.descHow[k] = 3;
			}
		}
		// An alpha-only sample (a mask, like the folder icon's) whose descriptor couldn't be traced: keep the code that
		// sets it up (up to 96 dwords before the sample), for 8 shaders, to see how its resource SGPRs are built.
		for (uint32_t k = 0; k < 2 && k < e.mimgCount; k++) {
			uint32_t at = mimgAt[k];
			if (((code[at] >> 8) & 0xF) != 0x8 || e.descHow[k] || censusCodeNum >= arrsize(censusCode))
				continue;
			bool known = false;
			for (uint32_t c = 0; c < censusCodeNum && !known; c++)
				known = censusCode[c].ps == e.ps;
			if (known)
				break;
			auto &dump = censusCode[censusCodeNum];
			dump.ps = e.ps;
			dump.mimgAt = at;
			dump.start = at + 2 > arrsize(dump.words) ? at + 2 - arrsize(dump.words) : 0;
			dump.count = at + 2 - dump.start;
			memcpy(dump.words, code + dump.start, dump.count * 4);
			__atomic_store_n(&censusCodeNum, censusCodeNum + 1, __ATOMIC_RELEASE);
			break;
		}
	}
	e.firstMs = uptimeMs();
	e.count = 1;
	census[n] = e;
	__atomic_store_n(&censusNum, n + 1, __ATOMIC_RELEASE);
}

// One-time dump of the context state: every context register Metal has set (last values), every uconfig register it set,
// and the clear-state buffer's context registers (the defaults under them), 8 per line, to compare with what Mesa
// programs for Stoney.
static void ctxDump() {
	auto dump = [](const char *what, uint32_t base, const uint32_t *val, const uint32_t *set, uint32_t n) {
		char line[200];
		size_t len = 0;
		uint32_t total = 0;
		for (uint32_t r = 0; r < n; r++) {
			if (!(set[r / 32] & (1U << (r % 32))))
				continue;
			total++;
			len += snprintf(line + len, sizeof(line) - len, " %x=%x", base + r * 4, val[r]);
			if (len > 150) {
				diskLogEvent("%s:%s", what, line);
				len = 0;
			}
		}
		if (len)
			diskLogEvent("%s:%s", what, line);
		diskLogEvent("%s: %u registers", what, total);
	};
	dump("ctx metal", 0x28000, censusCtx, censusCtxSet, 0x400);
	dump("ctx clearstate", 0x28000, csbCtx, csbCtxSet, 0x400);
	dump("uconfig metal", 0x30000, censusUcfg, censusUcfgSet, 0x400);
	diskLogEvent("gpu copies: %u packets, %u binds and %u reverts right after a copy to the shader; sDMA submissions %u, "
		"command buffers %u, unknown packets %u (headers %x %x %x %x %x %x %x %x)", gpuCopyPackets, gpuCopyBindHits,
		gpuCopyRevertHits, sdmaHookCalls, sdmaIbs, sdmaUnknown, sdmaUnknownHdr[0], sdmaUnknownHdr[1], sdmaUnknownHdr[2],
		sdmaUnknownHdr[3], sdmaUnknownHdr[4], sdmaUnknownHdr[5], sdmaUnknownHdr[6], sdmaUnknownHdr[7]);
}

// New draw-census entries to the disk log (watcher thread).
static void censusLog() {
	if (!ctxDumped && __atomic_load_n(&censusNum, __ATOMIC_ACQUIRE) >= 40) {
		ctxDumped = true;
		ctxDump();
	}
	uint32_t codes = __atomic_load_n(&censusCodeNum, __ATOMIC_ACQUIRE);
	for (uint32_t c = 0; c < codes; c++) {
		auto &dump = censusCode[c];
		if (dump.logged)
			continue;
		dump.logged = true;
		static char hex[96 * 9 + 1];
		size_t len = 0;
		for (uint32_t w = 0; w < dump.count; w++)
			len += static_cast<size_t>(snprintf(hex + len, sizeof(hex) - len, "%08x ", dump.words[w]));
		diskLogEvent("shader %llx code from dword %u (sample at %u): %s", dump.ps, dump.start, dump.mimgAt, hex);
	}
	uint32_t n = __atomic_load_n(&censusNum, __ATOMIC_ACQUIRE), batch = 0;
	for (uint32_t i = 0; i < n && batch < 40; i++) {  // at most 40 per tick, so the event buffer can't overflow
		auto &e = census[i];
		if (e.logged)
			continue;
		batch++;
		e.logged = true;
		diskLogEvent("draw #%u first %llu ms: ps %llx cb0 %x attr %x cb1 %x tmask %x cc %x blend %x depth %x stencil %x "
			"ref %x sinfo %x zinfo %x dbsh %x colfmt %x ud %x %x %x %x tex %x %x %x %x mimg %u: %x %x %x %x "
			"T0(%u) %08x %08x %08x %08x %08x %08x %08x %08x T1(%u) %08x %08x %08x %08x %08x %08x %08x %08x", i, e.firstMs, e.ps,
			e.regs[0], e.regs[1], e.regs[2], e.regs[3], e.regs[4], e.regs[5], e.regs[6], e.regs[7], e.regs[8],
			e.regs[9], e.regs[10], e.regs[11], e.regs[12], e.userData[0], e.userData[1], e.userData[2], e.userData[3],
			e.tex[0], e.tex[1], e.tex[2], e.tex[3], e.mimgCount, e.mimg[0], e.mimg[1], e.mimg[2], e.mimg[3],
			e.descHow[0], e.desc[0][0], e.desc[0][1], e.desc[0][2], e.desc[0][3], e.desc[0][4], e.desc[0][5], e.desc[0][6],
			e.desc[0][7], e.descHow[1], e.desc[1][0], e.desc[1][1], e.desc[1][2], e.desc[1][3], e.desc[1][4], e.desc[1][5],
			e.desc[1][6], e.desc[1][7]);
	}
	static uint32_t loggedFull;
	if (censusFull && !loggedFull) {
		loggedFull = 1;
		diskLogEvent("draw census full (%u entries)", n);
	}
}

// Walk one user command buffer: every pixel shader it binds (SET_SH_REG SPI_SHADER_PGM_LO/HI_PS, RSRC1_PS) gets
// fixed; with allowBump (before submission) a raised RSRC1_PS is written back into the command buffer.
// Where the submit hook's time goes (nvram178: ~146 us per command buffer): reading command buffers vs checking the
// shaders they bind, how much is read, and what kind of memory the command buffers are in (system snooped, system
// not snooped, carve-out): reads of non-snooped memory must flush the lines first.
static uint64_t ibReadTime, psCheckTime, ibDwordsRead;
static uint32_t ibPagesSnooped, ibPagesPlain, ibPagesVram;

static void processUserIb(uint32_t vmid, uint64_t ibVa, uint32_t dwords, bool allowBump) {
	static uint32_t buf[0x4000];
	uint64_t t0 = mach_absolute_time();
	uint32_t got = readGpuVa(vmid, ibVa, dwords < arrsize(buf) ? dwords : arrsize(buf), buf, false);
	ibReadTime += mach_absolute_time() - t0;
	ibDwordsRead += got;
	for (uint64_t page = ibVa & ~0xFFFULL; page < ibVa + got * 4ULL; page += 0x1000) {
		uint32_t pte = 0;
		if (!ibVaToPhysLocked(vmid, page, &pte))
			continue;
		if (!(pte & 2))
			ibPagesVram++;
		else if (pte & 4)
			ibPagesSnooped++;
		else
			ibPagesPlain++;
	}
	uint32_t psLo = 0, psHi = 0, rsrc1 = 0, rsrc1At = 0xFFFFFFFF;
	if (censusOff < 0)
		censusOff = checkKernelArgument("-crnocensus") ? 1 : 0;
	for (uint32_t j = 0; j < got;) {
		uint32_t header = buf[j], type = header >> 30;
		if (type == 2) {
			j++;
			continue;
		}
		uint32_t count = ((header >> 16) & 0x3FFF) + 1;
		uint32_t op = (header >> 8) & 0xFF;
		if (type == 3 && !censusOff) {
			if (op == 0x69 && j + 1 < got) {  // SET_CONTEXT_REG
				uint32_t start = buf[j + 1] & 0xFFFF;
				for (uint32_t k = 0; k + 1 < count && j + 2 + k < got && start + k < arrsize(censusCtx); k++) {
					censusCtx[start + k] = buf[j + 2 + k];
					censusCtxSet[(start + k) / 32] |= 1U << ((start + k) % 32);
				}
			} else if (op == 0x79 && j + 1 < got) {  // SET_UCONFIG_REG
				uint32_t start = buf[j + 1] & 0xFFFF;
				for (uint32_t k = 0; k + 1 < count && j + 2 + k < got && start + k < arrsize(censusUcfg); k++) {
					censusUcfg[start + k] = buf[j + 2 + k];
					censusUcfgSet[(start + k) / 32] |= 1U << ((start + k) % 32);
				}
			} else if (op == 0x27 || op == 0x2D || op == 0x35 || op == 0x25 || op == 0x38 || op == 0x24) {
				censusDraw(vmid);
			}
		}
		// GPU copies to memory (see GpuCopy): DMA_DATA (DST_SEL [21:20] 0/3 = address, SRC_SEL [30:29] 0/3 = address, 2 =
		// data), WRITE_DATA (DST_SEL [11:8] 1/2/5 = memory), COPY_DATA (DST_SEL [11:8] 1/2/5, COUNT_SEL [16]: 64-bit).
		if (type == 3 && op == 0x50 && j + 6 < got) {
			uint32_t ctrl = buf[j + 1], dstSel = (ctrl >> 20) & 3, srcSel = (ctrl >> 29) & 3;
			if (dstSel == 0 || dstSel == 3) {
				uint64_t src = (static_cast<uint64_t>(buf[j + 3]) << 32 | buf[j + 2]) & 0xFFFFFFFFFFFFULL;
				uint64_t dst = (static_cast<uint64_t>(buf[j + 5]) << 32 | buf[j + 4]) & 0xFFFFFFFFFFFFULL;
				noteGpuCopy(vmid, op, ctrl, src, dst, buf[j + 6] & 0x1FFFFF);
				if (srcSel == 0 || srcSel == 3)  // from an address (not immediate data or GDS)
					applySourcePatches(vmid, src, dst, buf[j + 6] & 0x1FFFFF);
			}
		} else if (type == 3 && op == 0x37 && j + 3 < got && count > 3) {
			uint32_t ctrl = buf[j + 1], dstSel = (ctrl >> 8) & 0xF;
			if (dstSel == 1 || dstSel == 2 || dstSel == 5)
				noteGpuCopy(vmid, op, ctrl, 0, static_cast<uint64_t>(buf[j + 3]) << 32 | buf[j + 2], (count - 3) * 4);
		} else if (type == 3 && op == 0x40 && j + 5 < got) {
			uint32_t ctrl = buf[j + 1], dstSel = (ctrl >> 8) & 0xF;
			if (dstSel == 1 || dstSel == 2 || dstSel == 5)
				noteGpuCopy(vmid, op, ctrl, static_cast<uint64_t>(buf[j + 3]) << 32 | buf[j + 2],
					static_cast<uint64_t>(buf[j + 5]) << 32 | buf[j + 4], (ctrl >> 16) & 1 ? 8 : 4);
		}
		if (type == 3 && op == 0x76 && j + 1 < got) {
			uint32_t start = buf[j + 1] & 0xFFFF;
			bool touched = false;
			for (uint32_t k = 0; k + 1 < count && j + 2 + k < got; k++) {
				uint32_t reg = start + k, v = buf[j + 2 + k];
				if (reg >= 0x0C && reg <= 0x1B)  // SPI_SHADER_USER_DATA_PS_0-15: s0-s15 in the pixel shader
					censusUserData[reg - 0x0C] = v;
				if (reg == 0x0B)  // SPI_SHADER_PGM_RSRC2_PS
					censusRsrc2 = v;
				if (reg == 0x08) {
					psLo = v;
					touched = true;
				} else if (reg == 0x09) {
					psHi = v;
					touched = true;
				} else if (reg == 0x0A) {
					rsrc1 = v;
					rsrc1At = j + 2 + k;
					touched = true;
				}
			}
			if (psLo)
				censusPs = (static_cast<uint64_t>(psHi & 0xFF) << 40) | (static_cast<uint64_t>(psLo) << 8);
			if (touched && psLo && rsrc1) {
				uint64_t psVa = (static_cast<uint64_t>(psHi & 0xFF) << 40) | (static_cast<uint64_t>(psLo) << 8);
				if (auto *c = recentCopyTo(vmid, psVa)) {
					gpuCopyBindHits++;
					if (gpuCopyLogged < 40) {
						gpuCopyLogged++;
						diskLogEvent("copy before bind: ps %llx vmid %u; op %x ctrl %x src %llx dst %llx %u bytes, %llu ms "
							"earlier", psVa, vmid, c->op, c->ctrl, c->src, c->dst, c->bytes, uptimeMs() - c->ms);
					}
				}
				// A shader bound again in the same submission was checked (and fixed) moments ago: reuse the answer
				// instead of reading its fingerprint and probe again.
				ibHookBinds++;
				BindMemo *memo = nullptr;
				for (uint32_t m = 0; m < arrsize(bindMemo) && !memo; m++)
					if (bindMemo[m].gen == vmCacheGen && bindMemo[m].va == psVa && bindMemo[m].vmid == vmid &&
						bindMemo[m].rsrcIn == rsrc1 && bindMemo[m].allowBump == allowBump)
						memo = &bindMemo[m];
				uint32_t want;
				if (memo) {
					want = memo->rsrcOut;
					ibHookMemoHits++;
				} else {
					uint64_t t1 = mach_absolute_time();
					want = handlePixelShader(vmid, psVa, rsrc1, allowBump);
					psCheckTime += mach_absolute_time() - t1;
					bindMemo[bindMemoNext] = {psVa, vmid, rsrc1, want, vmCacheGen, allowBump};
					bindMemoNext = (bindMemoNext + 1) % arrsize(bindMemo);
				}
				if (allowBump && want != rsrc1 && rsrc1At < got) {
					uint64_t phys = ibVaToPhysLocked(vmid, ibVa + rsrc1At * 4ULL);
					if (phys && ibWritePhys(phys, 4, &want)) {
						buf[rsrc1At] = want;
						rsrc1 = want;
						ibHookRsrcBumps++;
					}
				}
			}
		}
		j += 1 + count;
	}
}

// Without the lock (another submission holds it): only raise RSRC1_PS for shaders already given more VGPRs.
static void raiseOnlyIb(uint32_t vmid, uint64_t ibVa, uint32_t dwords) {
	if (!__atomic_load_n(&raisedNum, __ATOMIC_ACQUIRE))
		return;
	uint32_t psLo = 0, psHi = 0, rsrc1 = 0;
	uint64_t rsrc1Va = 0;
	for (uint32_t j = 0; j < dwords;) {
		uint32_t header = 0;
		uint64_t phys = ibVaToPhys(vmid, ibVa + j * 4ULL);
		if (!phys || !ibReadPhys(phys, 4, &header))
			return;
		uint32_t type = header >> 30;
		if (type == 2) {
			j++;
			continue;
		}
		uint32_t count = ((header >> 16) & 0x3FFF) + 1;
		if (type == 3 && ((header >> 8) & 0xFF) == 0x76 && count >= 2 && j + 1 + count <= dwords) {
			uint32_t pkt[16];
			uint32_t n = count < arrsize(pkt) ? count : arrsize(pkt);
			for (uint32_t k = 0; k < n; k++) {
				uint64_t a = ibVa + (j + 1 + k) * 4ULL;
				uint64_t ph = ibVaToPhys(vmid, a);
				if (!ph || !ibReadPhys(ph, 4, &pkt[k]))
					return;
			}
			uint32_t start = pkt[0] & 0xFFFF;
			bool touched = false;
			for (uint32_t k = 1; k < n; k++) {
				uint32_t reg = start + k - 1;
				if (reg == 0x08) {
					psLo = pkt[k];
					touched = true;
				} else if (reg == 0x09) {
					psHi = pkt[k];
					touched = true;
				} else if (reg == 0x0A) {
					rsrc1 = pkt[k];
					rsrc1Va = ibVa + (j + 1 + k) * 4ULL;
					touched = true;
				}
			}
			if (touched && psLo && rsrc1 && rsrc1Va) {
				uint64_t psVa = (static_cast<uint64_t>(psHi & 0xFF) << 40) | (static_cast<uint64_t>(psLo) << 8);
				uint32_t want = withRaise(vmid, psVa, rsrc1);
				uint64_t ph = want != rsrc1 ? ibVaToPhys(vmid, rsrc1Va) : 0;
				if (ph && ibWritePhys(ph, 4, &want)) {
					rsrc1 = want;
					ibHookRaiseOnly++;
				}
			}
		}
		j += 1 + count;
	}
}

// Fallback: the last 16 user command buffers on the GFX ring, every second (already submitted, so no RSRC1 changes).
static void interpFixScan() {
	auto *hw = static_cast<uint8_t *>(accelHardware);
	auto *mmio = gpuMmio;
	if (!hw || !mmio || !interpLock)
		return;
	// The submit hook only reads through this mapping, so make it even where the ring scan below can't run (Catalina).
	if (interpSubmit)
		carveInit();
	if (!ringLayoutKnown)
		return;
	auto *pm4 = *reinterpret_cast<uint8_t **>(hw + hwEngines);
	auto *ring = pm4 ? *reinterpret_cast<uint8_t **>(pm4 + 0x50) : nullptr;
	auto *cpu = ring ? *reinterpret_cast<uint32_t **>(ring + 0x40) : nullptr;
	uint32_t dwords = ring ? *reinterpret_cast<uint32_t *>(ring + 0x30) : 0;
	if (!cpu || dwords < 0x100 || (dwords & (dwords - 1)))
		return;
	if (interpHooked && __atomic_load_n(&ibHookCalls, __ATOMIC_RELAXED))
		return;  // the submit hook sees everything; don't compete for the lock
	IOLockLock(interpLock);
	vmCacheBegin();
	interpFix.scans++;
	uint32_t rptr = mmio[0x21C0] & (dwords - 1), found = 0;
	for (uint32_t back = 4; back < dwords && found < 16; back++) {
		uint32_t i = (rptr - back) & (dwords - 1);
		if (cpu[i] != 0xC0023F00)
			continue;
		uint32_t ctl = cpu[(i + 3) & (dwords - 1)], vmid = (ctl >> 24) & 0xF, dw = ctl & 0xFFFFF;
		if (!vmid || !dw)
			continue;
		found++;
		uint64_t ibVa = (static_cast<uint64_t>(cpu[(i + 2) & (dwords - 1)] & 0xFFFF) << 32) |
			(cpu[(i + 1) & (dwords - 1)] & ~3U);
		processUserIb(vmid, ibVa, dw, false);
	}
	IOLockUnlock(interpLock);
}

static void interpFixThread(void *, wait_result_t) {
	for (;;) {
		interpFixScan();
		IOSleep(1000);
	}
}

static void startInterpFix() {
	static bool started;
	if (started || interpFixOff || !interpLock)
		return;
	started = true;
	thread_t thread = nullptr;
	if (kernel_thread_start(interpFixThread, nullptr, &thread) == KERN_SUCCESS)
		thread_deallocate(thread);
}

// AMDVIPM4Channel::commitIndirectCommandBuffer(AMD_SUBMIT_COMMAND_BUFFER_INFO *): writes the INDIRECT_BUFFER packets for
// a submission into the GFX ring. info: +0 flags (bits 1 and 4 select other paths), +4 VMID, +0xC number of command
// buffers, then one entry per command buffer from +0x44, 0x28 bytes each: dwords at +0, GPU address at +0xC. Fix every
// pixel shader the command buffers bind before the GPU sees them.
// Polaris (and so Stoney spoofed as Baffin) submits through AMDFijiPM4Channel's override, which has the same layout
// (nvram161: the AMDVIPM4Channel hook alone saw 0 calls).
static void interpHookBody(uint8_t *info) {
	if (info && interpLock && !interpFixOff && gpuMmio) {
		uint32_t flags = *reinterpret_cast<uint32_t *>(info), vmid = *reinterpret_cast<uint32_t *>(info + 4);
		uint32_t count = *reinterpret_cast<uint32_t *>(info + submitCountOff);
		ibHookCalls++;
		ibHookLastVmid = vmid;
		ibHookLastFlags = flags;
		if (!(flags & 0x12) && vmid >= 1 && vmid <= 15 && !__atomic_load_n(&carveMap, __ATOMIC_ACQUIRE)) {
			ibHookNoMap++;
		} else if (!(flags & 0x12) && vmid >= 1 && vmid <= 15) {
			uint64_t start = mach_absolute_time();
			__atomic_fetch_add(&hookDepth, 1, __ATOMIC_ACQ_REL);
			// Another submission may hold the lock; it never blocks or allocates, so wait a little for it.
			bool locked = false;
			for (uint32_t spin = 0; spin < 20000 && !(locked = IOLockTryLock(interpLock)); spin++)
				__builtin_ia32_pause();
			if (!locked) {
				ibHookBusy++;
				for (uint32_t i = 0; i < count && i < 8; i++) {
					uint32_t dw = *reinterpret_cast<uint32_t *>(info + submitEntriesOff + i * submitStride);
					uint64_t va = *reinterpret_cast<uint64_t *>(info + submitEntriesOff + submitVaOff + i * submitStride);
					if (dw && va)
						raiseOnlyIb(vmid, va, dw);
				}
				__atomic_fetch_sub(&hookDepth, 1, __ATOMIC_ACQ_REL);
				return;
			}
			vmCacheBegin();
			for (uint32_t i = 0; i < count && i < 8; i++) {
				uint32_t dw = *reinterpret_cast<uint32_t *>(info + submitEntriesOff + i * submitStride);
				uint64_t va = *reinterpret_cast<uint64_t *>(info + submitEntriesOff + submitVaOff + i * submitStride);
				if (dw && va) {
					ibHookIbs++;
					processUserIb(vmid, va, dw, interpBump);
				}
			}
			IOLockUnlock(interpLock);
			__atomic_fetch_sub(&hookDepth, 1, __ATOMIC_ACQ_REL);
			uint64_t ns = 0;
			absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
			ibHookNs += ns;
		}
	}
}

static uint64_t wrapCommitIb(void *channel, uint8_t *info) {
	interpHookBody(info);
	return FunctionCast(wrapCommitIb, orgCommitIb)(channel, info);
}

static uint64_t wrapCommitIbFiji(void *channel, uint8_t *info) {
	interpHookBody(info);
	return FunctionCast(wrapCommitIbFiji, orgCommitIbFiji)(channel, info);
}

// sDMA submissions (AMDVIsDMAChannel::commitIndirectCommandBuffer, the same AMD_SUBMIT_COMMAND_BUFFER_INFO): their copies,
// writes and fills go into the copy list (op 0x100 + sDMA opcode), to see whether the sDMA engine rewrites fixed shaders
// (Sonoma: every revert had sDMA0 activity since the fix, and none had a copy in the graphics command buffers).
// Packet lengths from Linux vid.h / sdma_v3_0 (VI sDMA): COPY linear 7, WRITE linear 4 + n, INDIRECT 6, FENCE 4, TRAP 2,
// SEM 3, POLL_REGMEM 6, COND_EXE 5, ATOMIC 8, CONST_FILL 5, GEN_PTEPDE 10, TIMESTAMP 3, SRBM_WRITE 3; others end the walk.

// Sonoma (5157f14 log): the reverts are Metal's driver re-uploading its shader heap with sDMA linear copies (36 KB
// chunks, 1-7 ms before each revert) from a staging copy holding the original code: the copy lands after the fixer
// patched the destination at submission, and the draws in between run unfixed (the random flat-colour images). Fix the
// source too, here, before the copy runs: for every fixed shader inside the copy's destination, write its patch list at
// the same offset in the source, dword by dword, only where the source still holds the original dword it replaced.
static uint32_t srcFixCopies, srcFixDwords, srcFixMismatch, srcFixLogged, srcFixOverflow, srcFixReadFail;

static void applySourcePatches(uint32_t vmid, uint64_t src, uint64_t dst, uint32_t bytes) {
	static int off = -1;  // -crnosrcfix disables it
	if (off < 0)
		off = checkKernelArgument("-crnosrcfix") ? 1 : 0;
	if (!src || !bytes || off)
		return;
	for (uint32_t i = 0; i < seenNum && i < arrsize(seenShaders); i++) {
		auto &sh = seenShaders[i];
		if (sh.vmid != vmid || !sh.patchCount)
			continue;
		if (sh.patchCount == 0xFFFF) {  // a fix too long to replay: count copies onto its start
			if (sh.va >= dst && sh.va < dst + bytes)
				srcFixOverflow++;
			continue;
		}
		const uint32_t *patchAt = sh.patchAt, *patchOld = sh.patchOld, *patchNew = sh.patchNew;
		uint32_t patchCount = sh.patchCount;
		if (patchCount == 0xFFFE) {  // in the big pool, unless its slot went to another shader since
			auto &big = bigPatches[sh.bigSlot % arrsize(bigPatches)];
			if (big.va != sh.va || big.vmid != sh.vmid || !big.count) {
				if (sh.va >= dst && sh.va < dst + bytes)
					srcFixOverflow++;
				continue;
			}
			patchAt = big.at;
			patchOld = big.oldValue;
			patchNew = big.newValue;
			patchCount = big.count;
		}
		if (sh.va >= dst + bytes || sh.va + patchAt[patchCount - 1] * 4ULL + 4 <= dst)
			continue;  // no patched dword can be inside the copy (patchAt is ascending)
		srcFixCopies++;
		uint32_t wrote = 0, mismatch = 0;
		for (uint32_t k = 0; k < patchCount; k++) {
			// Each patched dword on its own: a re-upload chunk may start in the middle of a shader.
			uint64_t addr = sh.va + patchAt[k] * 4ULL;
			if (addr < dst || addr + 4 > dst + bytes)
				continue;
			uint64_t off = addr - dst;
			uint32_t now = 0;
			if (readGpuVa(vmid, src + off, 1, &now, false) != 1) {
				srcFixReadFail++;
				continue;
			}
			if (now == patchNew[k])
				continue;  // already fixed
			if (now != patchOld[k]) {
				mismatch++;
				continue;
			}
			uint64_t phys = ibVaToPhysLocked(vmid, src + off);
			if (phys && ibWritePhys(phys, 4, &patchNew[k]))
				wrote++;
		}
		srcFixDwords += wrote;
		srcFixMismatch += mismatch;
		if (srcFixLogged < 30 && (wrote || mismatch)) {
			srcFixLogged++;
			diskLogEvent("source fix: ps %llx in sDMA copy %llx -> %llx (%u bytes): %u dwords patched, %u mismatched", sh.va,
				src, dst, bytes, wrote, mismatch);
		}
	}
}

static void sdmaWalk(uint32_t vmid, uint64_t va, uint32_t dwords) {
	static uint32_t buf[0x1000];
	uint32_t got = readGpuVa(vmid, va, dwords < arrsize(buf) ? dwords : arrsize(buf), buf, false);
	for (uint32_t j = 0; j < got;) {
		uint32_t h = buf[j], op = h & 0xFF, sub = (h >> 8) & 0xFF, len = 0;
		switch (op) {
			case 0: len = 1 + ((h >> 16) & 0x3FFF); break;  // NOP (count of extra dwords)
			case 1:
				if (sub == 0 && j + 6 < got) {
					uint64_t src = (static_cast<uint64_t>(buf[j + 4]) << 32 | buf[j + 3]) & 0xFFFFFFFFFFFFULL;
					uint64_t dst = (static_cast<uint64_t>(buf[j + 6]) << 32 | buf[j + 5]) & 0xFFFFFFFFFFFFULL;
					uint32_t bytes = (buf[j + 1] & 0x3FFFFF) + 1;
					noteGpuCopy(vmid, 0x101, h, src, dst, bytes);
					applySourcePatches(vmid, src, dst, bytes);
				}
				// Linux tonga_sdma_pkt_open.h: linear 7, tiled 12, struct (SOA) 8, linear sub-window 13, tiled sub-window 14
				// (Apple's texture uploads, header 0x501: 91 per boot stopped the walk before), tile-to-tile 15.
				len = sub == 0 ? 7 : sub == 1 ? 12 : sub == 3 ? 8 : sub == 4 ? 13 : sub == 5 ? 14 : sub == 6 ? 15 : 0;
				break;
			case 2:
				if (sub == 0 && j + 3 < got) {
					noteGpuCopy(vmid, 0x102, h, 0, static_cast<uint64_t>(buf[j + 2]) << 32 | buf[j + 1],
						((buf[j + 3] & 0xFFFFF) + 1) * 4);
					len = 4 + (buf[j + 3] & 0xFFFFF) + 1;
				}
				break;
			case 4: len = 6; break;
			case 5: len = 4; break;
			case 6: len = 2; break;
			case 7: len = 3; break;
			case 8: len = 6; break;
			case 9: len = 5; break;
			case 10: len = 8; break;
			case 11:
				if (j + 4 < got)
					noteGpuCopy(vmid, 0x10B, h, 0, static_cast<uint64_t>(buf[j + 2]) << 32 | buf[j + 1],
						(buf[j + 4] & 0x3FFFFF) + 1);
				len = 5;
				break;
			case 12: len = 10; break;
			case 13: len = 3; break;
			case 14: len = 3; break;
			default: break;
		}
		if (!len) {
			sdmaUnknown++;
			bool known = false;
			for (uint32_t u = 0; u < sdmaUnknownHdrs && !known; u++)
				known = sdmaUnknownHdr[u] == h;
			if (!known && sdmaUnknownHdrs < arrsize(sdmaUnknownHdr))
				sdmaUnknownHdr[sdmaUnknownHdrs++] = h;
			return;
		}
		j += len;
	}
}

static uint64_t wrapCommitSdma(void *channel, uint8_t *info) {
	if (info && interpLock && gpuMmio && __atomic_load_n(&carveMap, __ATOMIC_ACQUIRE)) {
		uint32_t flags = *reinterpret_cast<uint32_t *>(info), vmid = *reinterpret_cast<uint32_t *>(info + 4);
		uint32_t count = *reinterpret_cast<uint32_t *>(info + submitCountOff);
		sdmaHookCalls++;
		if (!(flags & 0x12) && vmid <= 15 && IOLockTryLock(interpLock)) {
			__atomic_fetch_add(&hookDepth, 1, __ATOMIC_ACQ_REL);
			vmCacheBegin();
			for (uint32_t i = 0; i < count && i < 8; i++) {
				uint32_t dw = *reinterpret_cast<uint32_t *>(info + submitEntriesOff + i * submitStride);
				uint64_t va = *reinterpret_cast<uint64_t *>(info + submitEntriesOff + submitVaOff + i * submitStride);
				if (dw && va && vmid) {
					sdmaIbs++;
					sdmaWalk(vmid, va, dw);
				}
			}
			__atomic_fetch_sub(&hookDepth, 1, __ATOMIC_ACQ_REL);
			IOLockUnlock(interpLock);
		}
	}
	return FunctionCast(wrapCommitSdma, orgCommitSdma)(channel, info);
}

struct PutState {
	char *out;
	size_t size;
	size_t len;
};

static void psPut(void *c, const char *fmt, ...) {
	auto *st = static_cast<PutState *>(c);
	if (st->len + 1 >= st->size)
		return;
	va_list va;
	va_start(va, fmt);
	int n = vsnprintf(st->out + st->len, st->size - st->len, fmt, va);
	va_end(va);
	if (n > 0)
		st->len += static_cast<size_t>(n) < st->size - st->len ? static_cast<size_t>(n) : st->size - st->len - 1;
}

static void captureUserIbs(char *out, size_t size) {
	size_t len = 0;
	auto put = [&](const char *fmt, ...) {
		if (len + 1 >= size)
			return;
		va_list va;
		va_start(va, fmt);
		int n = vsnprintf(out + len, size - len, fmt, va);
		va_end(va);
		if (n > 0)
			len += static_cast<size_t>(n) < size - len ? static_cast<size_t>(n) : size - len - 1;
	};
	out[0] = 0;
	auto *hw = static_cast<uint8_t *>(accelHardware);
	auto *mmio = gpuMmio;
	if (!hw || !mmio || !ringLayoutKnown)
		return;
	auto *pm4 = *reinterpret_cast<uint8_t **>(hw + hwEngines);
	auto *ring = pm4 ? *reinterpret_cast<uint8_t **>(pm4 + 0x50) : nullptr;
	auto *cpu = ring ? *reinterpret_cast<uint32_t **>(ring + 0x40) : nullptr;
	uint32_t dwords = ring ? *reinterpret_cast<uint32_t *>(ring + 0x30) : 0;
	if (!cpu || dwords < 0x100 || (dwords & (dwords - 1)))
		return;
	uint32_t rptr = mmio[0x21C0] & (dwords - 1);
	// The last 16 user IBs before the read pointer (newest first), parsed oldest first.
	struct Ib {
		uint64_t va;
		uint32_t dw, vmid;
	};
	Ib ibs[16] {};
	uint32_t found = 0;
	for (uint32_t back = 4; back < dwords && found < arrsize(ibs); back++) {
		uint32_t i = (rptr - back) & (dwords - 1);
		if (cpu[i] != 0xC0023F00)
			continue;
		uint32_t ctl = cpu[(i + 3) & (dwords - 1)], vmid = (ctl >> 24) & 0xF, dw = ctl & 0xFFFFF;
		if (!vmid || !dw)
			continue;
		ibs[found++] = {(static_cast<uint64_t>(cpu[(i + 2) & (dwords - 1)] & 0xFFFF) << 32) |
			(cpu[(i + 1) & (dwords - 1)] & ~3U), dw, vmid};
	}
	static uint32_t ctx[0x400];
	memset(ctx, 0, sizeof(ctx));
	static const uint16_t keys[] {0x202, 0x1E0, 0x31C, 0x1C5, 0x1B3, 0x1B6, 0x1B1, 0x191, 0x192, 0x193, 0x205, 0x203,
		0x1B8, 0x206};
	static uint32_t keyVals[arrsize(keys)][4];
	static uint32_t keyNum[arrsize(keys)];
	memset(keyNum, 0, sizeof(keyNum));
	auto addDistinct = [](uint32_t *vals, uint32_t &num, uint32_t v, uint32_t max) {
		for (uint32_t k = 0; k < num; k++)
			if (vals[k] == v)
				return;
		if (num < max)
			vals[num++] = v;
	};
	// Per draw: pixel shader, NUM_INTERP, VS export count, SPI_PS_INPUT_CNTL_0-2, PS input enables, CB0 info, blend 0.
	struct Draw {
		uint64_t ps;
		uint32_t regs[8];
	};
	static const uint16_t drawRegs[8] {0x1B6, 0x1B1, 0x191, 0x192, 0x193, 0x1B3, 0x31C, 0x1E0};
	static Draw drawSet[12];
	uint32_t drawNum = 0, drawTotal = 0, parsed = 0, parsedDw = 0, packets = 0, loads[4] {}, vmidMask = 0;
	uint64_t psCur = 0, psAddrs[8] {};
	uint32_t psLo = 0, psHi = 0, psNum = 0, psVmid = 0;
	static uint32_t buf[0x4000];
	for (uint32_t q = found; q-- > 0;) {
		vmidMask |= 1U << ibs[q].vmid;
		uint32_t want = ibs[q].dw < arrsize(buf) ? ibs[q].dw : arrsize(buf), got = 0;
		while (got < want) {
			uint64_t va = ibs[q].va + got * 4ULL;
			uint32_t chunk = static_cast<uint32_t>((0x1000 - (va & 0xFFF)) / 4);
			if (chunk > want - got)
				chunk = want - got;
			uint64_t phys = ibVaToPhys(ibs[q].vmid, va);
			if (!phys || !ibReadPhys(phys, chunk * 4, buf + got))
				break;
			got += chunk;
		}
		if (!got)
			continue;
		parsed++;
		parsedDw += got;
		for (uint32_t j = 0; j < got;) {
			uint32_t header = buf[j], type = header >> 30;
			if (type == 2) {
				j++;
				continue;
			}
			uint32_t count = ((header >> 16) & 0x3FFF) + 1;
			if (type != 3) {
				j += 1 + count;
				continue;
			}
			packets++;
			uint32_t op = (header >> 8) & 0xFF;
			if (op == 0x69 && j + 1 < got) {
				uint32_t start = buf[j + 1] & 0xFFFF;
				for (uint32_t k = 0; k + 1 < count && j + 2 + k < got; k++) {
					uint32_t reg = start + k;
					if (reg >= 0x400)
						break;
					ctx[reg] = buf[j + 2 + k];
					for (uint32_t r = 0; r < arrsize(keys); r++)
						if (keys[r] == reg)
							addDistinct(keyVals[r], keyNum[r], ctx[reg], 4);
				}
			} else if (op == 0x76 && j + 1 < got) {
				uint32_t start = buf[j + 1] & 0xFFFF;
				for (uint32_t k = 0; k + 1 < count && j + 2 + k < got; k++) {
					uint32_t reg = start + k, v = buf[j + 2 + k];
					if (reg == 0x08)
						psLo = v;
					else if (reg == 0x09)
						psHi = v;
				}
				if (psLo) {
					psCur = (static_cast<uint64_t>(psHi & 0xFF) << 40) | (static_cast<uint64_t>(psLo) << 8);
					bool known = false;
					for (uint32_t r = 0; r < psNum; r++)
						known |= psAddrs[r] == psCur;
					if (!known && psNum < arrsize(psAddrs)) {
						psAddrs[psNum++] = psCur;
						psVmid = ibs[q].vmid;
					}
				}
			} else if (op == 0x5F || op == 0x60 || op == 0x61 || op == 0x73) {
				loads[op == 0x5F ? 0 : op == 0x60 ? 1 : op == 0x61 ? 2 : 3]++;
			} else if (op == 0x27 || op == 0x2D || op == 0x35 || op == 0x25 || op == 0x38) {
				drawTotal++;
				Draw d {psCur, {}};
				for (uint32_t r = 0; r < 8; r++)
					d.regs[r] = ctx[drawRegs[r]];
				bool known = false;
				for (uint32_t r = 0; r < drawNum && !known; r++)
					known = !memcmp(&drawSet[r], &d, sizeof(d));
				if (!known && drawNum < arrsize(drawSet))
					drawSet[drawNum++] = d;
			}
			j += 1 + count;
		}
	}
	put("userib: %u found (vmids %x), %u parsed, %u dw, %u packets, %u draws; loads sh %u cfg %u ctx %u ind %u\n", found,
		vmidMask, parsed, parsedDw, packets, drawTotal, loads[0], loads[1], loads[2], loads[3]);
	put("userib keys:");
	for (uint32_t r = 0; r < arrsize(keys); r++) {
		if (!keyNum[r])
			continue;
		put(" %x=", keys[r]);
		for (uint32_t k = 0; k < keyNum[r]; k++)
			put(k ? "/%x" : "%x", keyVals[r][k]);
	}
	put("\nuserib draws (ps ninterp vsout cntl0-2 ena cb0 blend0):");
	for (uint32_t r = 0; r < drawNum; r++) {
		uint32_t psIndex = 0;
		for (uint32_t k = 0; k < psNum; k++)
			if (psAddrs[k] == drawSet[r].ps)
				psIndex = k;
		put(" [ps%u", psIndex);
		for (uint32_t k = 0; k < 8; k++)
			put(" %x", drawSet[r].regs[k]);
		put("]");
	}
	put("\nuserib ps (%u):", psNum);
	PutState state {out, size, len};
	reportPixelShaders(psVmid, psAddrs, psNum, psPut, &state);
	len = state.len;
	put("\n");
}

// crpanicat=N black-box report. Spoof boot 21 went to a black panel with no panic in NVRAM, after IOFramebuffer saved a
// 1366x768 / 76.30 MHz "saved-config", i.e. Apple's controller set a mode. With the screen black, the panic text in
// NVRAM (AAPL,PanicInfo, read with OpenShell dmpstore) is the only way out: counters, the iGPU tree (framebuffers,
// connectors, mode), the current display/clock registers, the trace summary and the last trace events (setPixelClock,
// setCrtcToPixelClock, framebuffer calls). Register values are read fresh (pipes only when powered).
void CRDriver::vmmPanic(void *vmm) {
	// The state that decides this first (the panic log keeps the start): the fixes the sDMA channel depends on.
	char reason[480];
	snprintf(reason, sizeof(reason), "CarrizoRed: the GPU VM manager %p has no sDMA channel (+0x28 null) at a page-table "
		"update; Apple's endVMPTUpdate would fault. hwsig %u found/%u missed, smu load %s (calls %u), sDMA1 dropped %u "
		"aliased %u, sDMA0-only starts %u, accel guards failed %u (last %s), routes ok %u failed %u unverified %u (last %s), "
		"kernel %d.%d.", vmm, hwsigResolved, hwsigMissed, smuLoad.result ? smuLoad.result : "not run", smuLoadCalls,
		accelSdma1Dropped, accelSdma1Aliased, accelStartPatched, accelGuardMisses,
		accelGuardMissLast ? accelGuardMissLast : "-", routesOk, routesFailed, routesUnverified,
		routeLastFailed ? routeLastFailed : "-", getKernelVersion(), getKernelMinorVersion());
	// No disk log here: this runs in WindowServer's submission path with the accelerator's locks held.
	panic("%s\n%s", reason, callback->composeReport(static_cast<uint32_t>(uptimeMs() / 1000), ""));
}

void CRDriver::panicReport(uint32_t t, const char *tree, const char *reason) {
	const char *report = composeReport(t, tree);
	diskLogEvent("panic: %s; black-box report follows", reason);
	diskLogTick(t, 0);
	diskLogAppend(report, strlen(report));
	diskLogAppend("\n", 1);
	panic("%s\n%s", reason, report);
}

const char *CRDriver::composeReport(uint32_t t, const char *tree) {
	// 32 KB: the disk log takes all of it (Catalina wake #8: the display sections of the post-wake report were cut off
	// at 8 KB, inside the registry tree); a panic keeps what fits.
	static char report[32768];
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
	// Early block first: the panic log keeps the start of the report and drops the middle, and a start-up that fails
	// before the display or the accelerator is up needs these (nvram4/5: registers reading 0 before the SMU load).
	if (iGPU) {
		uint16_t pm = iGPU->findPCICapability(kIOPCIPowerManagementCapability);
		add("early: pci %04x:%04x rev %x cmd %x status %x pmcsr %x bar0 %x bar2 %x bar5 %x; mmio phys %llx mapped at %llu ms: "
			"method %u failures %u memsize %x fbloc %x fboff %x grbm %x\n", iGPU->configRead16(kIOPCIConfigVendorID),
			iGPU->configRead16(kIOPCIConfigDeviceID), iGPU->configRead8(kIOPCIConfigRevisionID),
			iGPU->configRead16(kIOPCIConfigCommand), iGPU->configRead16(kIOPCIConfigStatus),
			pm ? iGPU->configRead16(static_cast<uint8_t>(pm + 4)) : 0xFFFF, iGPU->configRead32(kIOPCIConfigBaseAddress0),
			iGPU->configRead32(kIOPCIConfigBaseAddress2), iGPU->configRead32(kIOPCIConfigBaseAddress5), probePhys, probeMs,
			mmioMapMethod, mmioMapFailures, probeValues[0], probeValues[1], probeValues[2], probeValues[3]);
	}
	if (auto *m = gpuMmio) {
		uint32_t smuVersion = smuWindowRead(0x10000000 + 0x1FF80), smuLoadStatus = smuWindowRead(0x10000000 + 0x1FF98);
		add("early now: grbm %x fbloc %x fboff %x memsize %x rlc cntl %x safe %x stat %x cp rptr %x wptr %x me cntl %x; "
			"smu ver %x load %x mbox %x/%x/%x\n", m[Reg::GRBM_STATUS], m[Reg::MC_VM_FB_LOCATION], m[Reg::MC_VM_FB_OFFSET],
			m[Reg::CONFIG_MEMSIZE], m[0xEC00], m[0xEC05], m[0xEC04], m[0x21C0], m[0x3045], m[0x21B6], smuVersion, smuLoadStatus,
			m[0x1C0], m[0x1D0], m[0x1E0]);
	}
	add("early sleep: sleeps %u wakes %u, smu load calls %u, power losses %u; ", systemSleeps, systemWakes, smuLoadCalls,
		smuPowerLosses);
	add("early smuload: %s, resp %x %x %x %x %x %x, status %x -> %x in %u ms; ", smuLoad.result ? smuLoad.result : "not run",
		smuLoad.resp[0], smuLoad.resp[1], smuLoad.resp[2], smuLoad.resp[3], smuLoad.resp[4], smuLoad.resp[5], smuLoad.before,
		smuLoad.after, smuLoad.ms);
	add("cail waits %u, %u failed:", cailWaits, cailWaitFailures);
	for (uint32_t i = 0; i < cailWaitFailures && i < 2; i++)
		add(" [%x]&%x!=%x(%x @%llu)", cailWaitFails[i].reg, cailWaitFails[i].mask, cailWaitFails[i].value, cailWaitFails[i].read,
			cailWaitFails[i].ms);
	add("; RLC at halt: cgcg %x pg %x\n", rlcCgcgBefore, rlcPgBefore);
	add("early events:");
	for (uint32_t i = 0; i < arrsize(diskLogRecent); i++) {
		auto &line = diskLogRecent[(diskLogRecentNext + i) % arrsize(diskLogRecent)];
		if (line[0])
			add(" | %s", line);
	}
	add("\n");
	add("boot DP lanes %u (lane cap %s), LB size fixes %u%s, boot DP depth %u, bpc caps %u%s\n", bootDpLanes,
		checkKernelArgument("-crnolanecap") ? "off" : "on", lbFixes, lbFixOff ? " (off)" : "", bootDpDepth, bpcCaps,
		bpcCapOff ? " (off)" : "");
	if (restoreOn)
		add("restore: %u steps done, %u registers written, %u re-written after Apple changed them\n", restoreDone,
			restoreWrites, restoreHolds);
	add("assr: DPCD 0x00D cap 0x%02x, 0x10A before Apple 0x%02x, %u writes%s, readback 0x%02x\n", assrCap, assrBoot,
		assrWrites, assrOff ? " (off)" : "", assrReadback);
	// Metal and the flip chain first: with real GPU traffic the report fills the 8 KB buffer and drops its tail.
	add("hwinfo: calls %u ret %x fixed %u spoofed %u; ref %llx sys %llx mem %llx d8 %llx; device %llx raster %llx shader %llx\n",
		hwInfoCalls, hwInfoRet, hwInfoFixed, hwInfoSpoofed, hwInfoValues[0], hwInfoValues[1], hwInfoValues[2], hwInfoValues[3],
		hwInfoValues[4], hwInfoValues[5], hwInfoValues[6]);
	if (hwInfoCalls) {
		// What Metal's AddrLib is created with (amdMtlBronzeInitAddrLibInterface): globalGBReg1 (+0xA0) as GB_ADDR_CONFIG,
		// backEndDisableField, numBanksField, numRanksField (+0xA8-0xB8); and the GPU's own GB_ADDR_CONFIG (0x263E),
		// MC_ARB_RAMCFG (0x9D8).
		auto *w = hwInfoCopy;
		add("hwinfo addrlib: gb %x bedis %x banks %x ranks %x (reg GB_ADDR_CONFIG %x RAMCFG %x); rbmask %x se %u/%x sa %u "
			"cus %u; chiprev %x\n", w[0xA0 / 4], w[0xA8 / 4], w[0xB0 / 4], w[0xB8 / 4], gpuMmio ? gpuMmio[0x263E] : 0,
			gpuMmio ? gpuMmio[0x9D8] : 0, w[0x98 / 4], w[0x88 / 4], w[0x8C / 4], w[0x90 / 4], w[0x94 / 4], w[0x8 / 4]);
	}
	add("page patches: hooked %d, %u pages validated; gva %s %u hevc %x;", scPatchHooked, scPatchCalls,
		gvaPatchOn ? "on" : "off", gvaPatchHits, gvaHevcState);
	for (auto &patch : pagePatches)
		add(" %s %s%u", patch.name, patch.enabled ? "" : "off ", patch.hits);
	add("\n");
	add("interp fix: scans %u, shaders %u, hazards %u, fixed %u, skipped %u (last %u), writes %u, failed %u, dead-register fixes %u, raised %u; hook %d calls %u ibs %u rsrc %u vmid %u flags %x ms %llu busy %u nomap %u raise-only %u raised-shaders %u reverts %u (moved %u) block-moves %u (grown %u); source fixes %u copies %u dwords %u mismatched %u too long %u unreadable; binds %u memo %u walks %u; ib-read ms %llu ps-check ms %llu ib-dw %llu pages snooped %u plain %u vram %u\n",
		interpFix.scans, interpFix.shaders, interpFix.hazards, interpFix.fixed, interpFix.skipped, interpFix.lastSkip,
		interpFix.writes, interpFix.writeFails, interpFix.deadRegs, interpFix.bumps, interpHooked, ibHookCalls, ibHookIbs,
		ibHookRsrcBumps, ibHookLastVmid, ibHookLastFlags, ibHookNs / 1000000, ibHookBusy, ibHookNoMap, ibHookRaiseOnly,
		raisedNum, interpFix.probeReverts, interpFix.revertMoved, interpFix.blockMoves, interpFix.blockGrows, srcFixCopies, srcFixDwords, srcFixMismatch, srcFixOverflow, srcFixReadFail, ibHookBinds, ibHookMemoHits, vmWalks,
		absToMs(ibReadTime), absToMs(psCheckTime), ibDwordsRead, ibPagesSnooped, ibPagesPlain, ibPagesVram);
	// The clock samples (sampleGpuClock); "window": the SMU firmware version read through window 13 and window 0,
	// which must match for the samples to mean anything.
	uint32_t windowVersion = gpuMmio ? smuWindowRead(0x10000000 + 0x1FF80) : 0, version = 0;
	if (gpuMmio) {
		gpuMmio[0x180] = 0x10000000 + 0x1FF80;
		version = gpuMmio[0x181];
	}
	add("gpu clock: samples %u, last %x, window %x/%x; current level:", sclkSamples, sclkLast, windowVersion, version);
	for (uint32_t level = 0; level < arrsize(sclkCurrent); level++)
		if (sclkCurrent[level])
			add(" L%u %u", level, sclkCurrent[level]);
	add("; target:");
	for (uint32_t level = 0; level < arrsize(sclkTarget); level++)
		if (sclkTarget[level])
			add(" L%u %u", level, sclkTarget[level]);
	if (sclkSamples && gfxVidMax <= 248)
		add("; gfx %u-%u mV", (6200 - 25 * gfxVidMax) / 4, (6200 - 25 * gfxVidMin) / 4);
	add("\n");
	if (smuClk.done) {
		// sclk levels: MHz/DfsDid/GfxVid/GnbVid; "off" is the level used while DPM is off.
		add("smu clock table: resp %x %x %x %x %x, max level %u; sclk", smuClk.resp[0], smuClk.resp[1], smuClk.resp[2],
			smuClk.resp[3], smuClk.resp[4], smuClk.maxLevel);
		for (uint32_t level = 0; level < 9; level++) {
			const uint8_t *l = smuClk.table + level * 12;
			uint32_t freq = 0;
			memcpy(&freq, l + 8, 4);
			if (level == 8)
				add("; off %u/%x/%x/%x", freq / 100, l[2], l[1], l[0]);
			else
				add(" %u/%x/%x/%x", freq / 100, l[2], l[1], l[0]);
		}
		uint32_t valid = 0, maxIndex = 0;
		memcpy(&valid, smuClk.table + 108, 4);
		memcpy(&maxIndex, smuClk.table + 112, 4);
		add("; valid %x max %u\n", valid, maxIndex);
	}
	if (smuVideo.done)
		add("video: power on at %llu ms, UVDPowerON %x VCEPowerON %x DPM %x; uvd status %x vcpu %x reset %x, vce status %x vcpu %x reset %x\n",
			smuVideo.ms, smuVideo.resp[0], smuVideo.resp[1], smuVideo.resp[2], videoPowered() ? gpuMmio[0x3DAF] : 0,
			videoPowered() ? gpuMmio[0x3D98] : 0, videoPowered() ? gpuMmio[0x3DA0] : 0,
			videoPowered() ? gpuMmio[0x8001] : 0, videoPowered() ? gpuMmio[0x8005] : 0, videoPowered() ? gpuMmio[0x8048] : 0);
	if (uvdStartInfo.calls)
		add("uvd start: calls %u skipped %u, last at %llu ms = %u: status %x vcpu %x reset %x lmi %x tile1 %x; state %u, "
			"pg off %u; now ring rptr %x wptr %x, srbm status %x\n", uvdStartInfo.calls, uvdStartInfo.skipped,
			uvdStartInfo.ms, uvdStartInfo.ret, uvdStartInfo.status, uvdStartInfo.vcpu, uvdStartInfo.reset, uvdStartInfo.lmi,
			uvdStartInfo.tile, uvdState, uvdPgOff, gpuMmio ? gpuMmio[0x3DA4] : 0, gpuMmio ? gpuMmio[0x3DA5] : 0,
			gpuMmio ? gpuMmio[0x394] : 0);
	if (uvdLive.samples) {
		auto &s = uvdLive;
		add("uvd live (%u samples, last at %llu ms): status %x vcpu %x rptr %x wptr %x rb cntl %x bar %x:%08x vmid %x; ib "
			"%x:%08x vmid %x size %x; lmi %x cmd %x data %x %x ctx %x sema %x; ring%s:", s.samples, s.ms, s.status, s.vcpu,
			s.rptr, s.wptr, s.rbCntl, s.rbHi, s.rbLo, s.rbVmid, s.ibHi, s.ibLo, s.ibVmid, s.ibSize, s.lmi, s.cmd, s.data0,
			s.data1, s.ctxId, s.semaTimeout, s.ringRead ? "" : " (not read)");
		for (size_t i = 0; s.ringRead && i < arrsize(s.ring); i++)
			add(" %x", s.ring[i]);
		add("\n");
	}
	if (smuDpm.done)
		add("sclk dpm: resp %x %x %x %x %x, L%u-L%u, features %x -> %x (SclkDpmOn %u), level changes %u, max seen L%u (before DPM), now %x; deep sleep %x watermark %x; mailbox msg %x resp %x arg %x, foreign %u:",
			smuDpm.resp[0], smuDpm.resp[1], smuDpm.resp[2], smuDpm.resp[3], smuDpm.resp[4], smuDpm.hardMin,
			smuDpm.softMax, smuDpm.featuresBefore, smuDpm.featuresAfter, (smuDpm.featuresAfter & Smu8::SclkDpmOn) != 0,
			sclkChanges, sclkMaxSeen, gpuMmio ? smuWindowRead(0xD0200014) : 0, smuDpm.resp[5], smuDpm.resp[6],
			gpuMmio ? gpuMmio[0x1C0] : 0, gpuMmio ? gpuMmio[0x1D0] : 0, gpuMmio ? gpuMmio[0x1E0] : 0, smuForeignMsgs);
	if (smuDpm.done) {
		for (uint32_t i = 0; i < smuForeignNum; i++)
			add(" %x", smuForeignIds[i]);
		add("\n");
	}
	if (skippedNum) {
		add("interp skipped:");
		for (uint32_t q = 0; q < skippedNum; q++) {
			add(" %llx(%u dw, at %u, %u/%u", skippedShaders[q].va, skippedShaders[q].len, skippedShaders[q].at,
				skippedShaders[q].reason, skippedShaders[q].why);
			if (skippedShaders[q].word && skippedShaders[q].reason == SkipUnknownEncoding)
				add(" word %x", skippedShaders[q].word);
			else if (skippedShaders[q].word)
				add(" detail %x", skippedShaders[q].word);
			add(")");
		}
		add("\n");
	}
	if (!checkKernelArgument("-crnoibdump")) {
		static char ibText[3600];
		captureUserIbs(ibText, sizeof(ibText));
		add("%s", ibText);
	}
	if (hangSnap.taken) {
		add("hang @%llu arg %x:", hangSnap.ms / 100, hangSnap.arg);
		for (size_t i = 0; i < arrsize(hangRegs); i++)
			add(" %x=%x", hangRegs[i], hangSnap.regs[i]);
		add("; ring @rptr-40:");
		for (uint32_t i = 0; i < 48; i++)
			add(i == 40 ? " | %x" : " %x", hangSnap.ring[i]);
		add("\n");
	}
	if (gpuDeadMs)
		add("GPU NOT RESPONDING (registers read ffffffff) %s, first seen @%llu: power off fully before the next boot\n",
			gpuDeadAtStart ? "from start-up" : "since", gpuDeadMs / 100);
	add("routes: %u ok, %u failed, %u not landed (last %s); pool %lu/%lu\n", routesOk, routesFailed, routesUnverified,
		routeLastFailed ? routeLastFailed : "-", routePoolUsed, RoutePoolSize);
	add("kexts: X4000 %u(+%u)@%llx HWServices %u(+%u)@%llx HWLibs %u(+%u)@%llx\n", accelKextLoads[0], accelKextRepeats[0],
		accelKextSlide[0], accelKextLoads[1], accelKextRepeats[1], accelKextSlide[1], accelKextLoads[2],
		accelKextRepeats[2], accelKextSlide[2]);
	add("hwlibs: patched @%llx earlyInit %llx, live earlyInit %llx (checks %u) bytes %02x %02x %02x %02x %02x%s, "
		"repatched %u -> %02x %02x %02x %02x %02x; wired %d (%x, text %llx+%llx); phys at patch %llx at check %llx; "
		"probe write cc -> %x\n",
		static_cast<uint64_t>(hwlibsSlide), static_cast<uint64_t>(hwlibsEarlyInit),
		static_cast<uint64_t>(hwlibsLiveEarlyInit), hwlibsLiveChecks, hwlibsEntryBytes[0], hwlibsEntryBytes[1],
		hwlibsEntryBytes[2], hwlibsEntryBytes[3], hwlibsEntryBytes[4], hwlibsUnpatched ? " (UNPATCHED)" : "",
		hwlibsRepatches, hwlibsAfterBytes[0], hwlibsAfterBytes[1], hwlibsAfterBytes[2], hwlibsAfterBytes[3],
		hwlibsAfterBytes[4], hwlibsWired != nullptr, hwlibsWireResult, static_cast<uint64_t>(hwlibsTextStart),
		hwlibsTextSize, hwlibsPhysAtPatch, hwlibsPhysAtCheck, hwlibsProbeWrite);
	add("start order: controller ready %d, accel probes deferred %u, rematches %u\n", controllerReady,
		accelDeferrals, accelRematches);
	add("flips: performed %u, completed %u, kicked %u, last @%llu\n", flipPerforms, flipIrqs, flipKicks,
		flipPerformMs / 100);
	add("pg: blocked %u, RLC_PG_CNTL %x\n", pgBlocked, gpuMmio ? gpuMmio[0xEC43] : 0);
	add("smu reload: %u, last @%llu status %x -> %x resp %x in %u ms\n", smuReload.count, smuReload.at / 100,
		smuReload.before, smuReload.after, smuReload.resp, smuReload.ms);
	if (hwInfoCalls) {
		// AMDVIHardware::initHWInfo: +0x28 GB_ADDR_CONFIG, +0x30/+0x38 PA_SC_RASTER_CONFIG(_1), +0xB0/+0xB8 from
		// MC_SHARED_CHMAP, +0x128 tile-mode count then the table, +0x1AC macro-tile count then the table.
		auto *w = hwInfoCopy;
		add("hwinfo layout: cu masks %x %x %x chmap %x %x; tile modes %u:", w[0x28 / 4], w[0x30 / 4], w[0x38 / 4],
			w[0xB0 / 4], w[0xB8 / 4], w[0x128 / 4]);
		for (uint32_t i = 0; i < 32 && i < w[0x128 / 4]; i++)
			add(" %x", w[0x12C / 4 + i]);
		add("; macro %u:", w[0x1AC / 4]);
		for (uint32_t i = 0; i < 16 && i < w[0x1AC / 4]; i++)
			add(" %x", w[0x1B0 / 4 + i]);
		add("\n");
	}
	add("rbplus: csib %llx (%u dw), seen %x, SX_PS_DOWNCONVERT %x EPSILON %x CONTROL %x (patched %u) MRT0-7",
		rbplusCsib, rbplusCsibDwords, rbplusSeen, rbplusValues[0], rbplusValues[1], rbplusValues[2], rbplusPatched);
	for (uint32_t i = 4; i < 12; i++)
		add(" %x", rbplusValues[i]);
	add("\n");
	add("uc (now @%llu):", uptimeMs() / 100);
	for (size_t i = 0; i < MaxTraceStages; i++)
		if (stageNames[i] && !strncmp(stageNames[i], "uc.", 3)) {
			add(" %s=%u%s", stageNames[i] + 3, stageCalls[i], stageOpen[i] ? "(open)" : "");
			if (stageCalls[i] && stageLastArg[i])
				add("(last %x)", stageLastArg[i]);
			if (stageCalls[i])
				add("@%llu", stageEnterMs[i] / 100);  // last entry, in 0.1 s of uptime
		}
	add("\n");
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
		// The storm breaker's snapshot (nvram73-90: it fired every accelerated boot, at 501 handler calls, leaving the
		// accelerator's 48 submissions waiting for completion interrupts), the read pointers the driver wrote, and the
		// ring's first entries (VI IH entry: src_id, src_data, ring/vmid/pasid, timestamp).
		if (ihStormBroken) {
			auto &snap = ihSnapshot;
			add("ihstorm: at %llu ms after %u calls; RB_CNTL %x BASE %x RPTR %x WPTR %x WPTR_ADDR %x:%x CNTL %x STATUS %x; "
				"wb %x at %llx; ring cpu %llx gpu %llx size %x %s; rptr writes %u last %x; entries:", snap.ms, snap.calls,
				snap.rbCntl, snap.rbBase, snap.rptr, snap.wptr, snap.wptrAddrHi, snap.wptrAddrLo, snap.cntl, snap.status,
				snap.wbValue, snap.wbAddress, snap.ringCpu, snap.ringGpu, snap.size, snap.vram ? "vram" : "sysmem",
				ihReadPointerWrites, ihLastReadPointer);
			if (snap.ringCpu)
				for (uint32_t i = 0; i < 8; i++) {
					auto *e = reinterpret_cast<volatile uint32_t *>(snap.ringCpu) + i * 4;
					add(" %x/%x/%x", e[0], e[1], e[2]);
				}
			add("\n");
			// nvram91: the IH's WPTR register was 0x10 (one entry written) while the CPU saw a zero write-back and an
			// empty ring. Read the same ring entries and write-back from the carve-out's physical memory (where the GPU's
			// FB-relative address lands) and through BAR0 (the CPU's VRAM window), to see which side is off.
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
			uint64_t carveout = static_cast<uint64_t>(m[Reg::MC_VM_FB_OFFSET] & 0x3FFFF) << 22;
			uint64_t fbBase = static_cast<uint64_t>(m[Reg::MC_VM_FB_LOCATION] & 0xFFFF) << 24;
			uint64_t wbMc = static_cast<uint64_t>(snap.wptrAddrHi & 0xFF) << 32 | (snap.wptrAddrLo & ~3U);
			uint64_t ringFb = snap.ringGpu - fbBase, wbFb = wbMc - fbBase;
			uint64_t bar0 = 0, bar0Size = 0;
			if (auto *gpu = iGPU)
				if (auto *range = gpu->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0)) {
					bar0 = range->getPhysicalAddress();
					bar0Size = range->getLength();
				}
			add("ihstorm views: ring fb+%llx wb fb+%llx; HDP base %x;", ringFb, wbFb, m[Reg::HDP_NONSURFACE_BASE]);
			static const struct {
				const char *name;
				bool viaBar;
			} views[] {{"carve-out", false}, {"bar0", true}};
			for (auto &view : views) {
				uint64_t base = view.viaBar ? bar0 : carveout;
				if (!base || (view.viaBar && (ringFb + 64 > bar0Size || wbFb + 4 > bar0Size))) {
					add(" %s n/a;", view.name);
					continue;
				}
				uint32_t entries[16] {}, wb = 0xFFFFFFFF;
				bool ok = readPhys(base + ringFb, sizeof(entries), entries) && readPhys(base + wbFb, sizeof(wb), &wb);
				add(" %s%s wb %x:", view.name, ok ? "" : " (map failed)", wb);
				for (uint32_t i = 0; i < 4; i++)
					add(" %x/%x/%x", entries[i * 4], entries[i * 4 + 1], entries[i * 4 + 2]);
				add(";");
			}
			add("\n");
		} else {
			add("ih: rptr writes %u last %x\n", ihReadPointerWrites, ihLastReadPointer);
		}
		add("ih ring move: %s, gpu %llx, applied %u", ihMoveResult ? ihMoveResult : "not tried", ihMovedGpu, ihRingMoves);
		if (ihRingMap) {
			auto *ring = reinterpret_cast<volatile uint32_t *>(ihRingMap->getVirtualAddress());
			uint32_t size = static_cast<uint32_t>(ihRingMap->getLength()) - 0x1000;
			add("; wb %x; entries:", ring[size / 4]);
			for (uint32_t i = 0; i < 4; i++)
				add(" %x/%x/%x", ring[i * 4], ring[i * 4 + 1], ring[i * 4 + 2]);
		}
		add("\n");
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
	bool displayDiag = checkKernelArgument("-crnoaccel") || checkKernelArgument("-crdisplaydiag") ||
		__atomic_load_n(&wakeDisplayDiag, __ATOMIC_RELAXED);
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
		// SDMA0 (nvram98: interrupts work, the accelerator's 25 submissions reached the HW channel, but none landed on
		// the graphics ring; blits go to SDMA). SDMA0_CNTL, STATUS, STATUS1, F32_CNTL, GFX_RB_CNTL, BASE, BASE_HI,
		// RPTR, WPTR, RPTR_ADDR_HI/LO, IB_CNTL, CONTEXT_STATUS; then the ring's first dwords and those before the
		// read pointer, from the carve-out when the ring is in VRAM.
		{
			static const uint16_t sdmaRegs[] {0x3404, 0x340D, 0x340E, 0x3412, 0x3480, 0x3481, 0x3482, 0x3483, 0x3484,
				0x3488, 0x3489, 0x348A, 0x3491};
			add("sdma:");
			for (auto reg : sdmaRegs)
				add(" %x=%x", reg, gpuMmio[reg]);
			uint64_t ringMc = static_cast<uint64_t>(gpuMmio[0x3481]) << 8 | static_cast<uint64_t>(gpuMmio[0x3482]) << 40;
			uint64_t fbBase = static_cast<uint64_t>(gpuMmio[Reg::MC_VM_FB_LOCATION] & 0xFFFF) << 24;
			uint64_t fbTop = (static_cast<uint64_t>(gpuMmio[Reg::MC_VM_FB_LOCATION] >> 16) + 1) << 24;
			uint32_t rptr = (gpuMmio[0x3483] >> 2) & 0xFFFF;  // bytes -> dwords
			if (ringMc >= fbBase && ringMc + 0x10000 <= fbTop) {
				uint64_t phys = (static_cast<uint64_t>(gpuMmio[Reg::MC_VM_FB_OFFSET] & 0x3FFFF) << 22) + ringMc - fbBase;
				uint32_t start = rptr > 16 ? rptr - 16 : 0;
				auto *desc = IOMemoryDescriptor::withPhysicalAddress(phys, 0x10000, kIODirectionIn);
				auto *map = desc ? desc->map(kIOMapInhibitCache) : nullptr;
				if (map) {
					auto *ring = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
					add(" | ring vram:");
					for (uint32_t i = 0; i < 8; i++)
						add(" %x", ring[i]);
					add(" | @%x:", start);
					for (uint32_t i = start; i < start + 24 && i < 0x4000; i++)
						add(" %x", ring[i]);
				}
				OSSafeReleaseNULL(map);
				OSSafeReleaseNULL(desc);
			} else {
				add(" | ring at %llx (not VRAM)", ringMc);
			}
			add("\n");
		}
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
		if (auto *hw = ringLayoutKnown ? static_cast<uint8_t *>(accelHardware) : nullptr) {
			auto *pm4 = *reinterpret_cast<uint8_t **>(hw + hwEngines);
			auto *ring = pm4 ? *reinterpret_cast<uint8_t **>(pm4 + 0x50) : nullptr;
			auto *cpu = ring ? *reinterpret_cast<uint32_t **>(ring + 0x40) : nullptr;
			uint32_t dwords = ring ? *reinterpret_cast<uint32_t *>(ring + 0x30) : 0;
			uint32_t rptr = gpuMmio[0x21C0];
			// From dword 0 (nvram55: the fetcher was at 0x88, past the start sequence, which ends with SET_BASE at 0x80),
			// with runs of the GFX8 NOP filler (PACKET3(NOP, 0x3FFF) = 0xFFFF1000) collapsed to "*n".
			if (cpu && dwords >= 0x100) {
				add("ring %llx (%u dw):", ring ? *reinterpret_cast<uint64_t *>(ring + 0x48) : 0, dwords);
				// Past the start-up packets (nvram104: RPTR 0x14980), show the 64 dwords before the read pointer instead.
				uint32_t begin = rptr > 0x180 ? rptr - 64 : 0;
				uint32_t end = rptr > 0x180 ? rptr + 16 : (rptr + 8 < 0x200 ? rptr + 8 : 0x200);
				if (begin)
					add(" @%x:", begin);
				for (uint32_t i = begin; i < end && i < dwords;) {
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
		add("smuload: %s (step limit %u, stopped at %u); format calls %u cail size %x region +%x block fb+%llx mc %llx phys %llx tasks %u; resp %x %x %x "
			"%x %x %x; status %x -> %x in %u ms; mec ic %x %x cntl %x\n", smuLoad.result ? smuLoad.result : "not run",
			smuStepLimit, smuStoppedAt,
			smuFormatCalls, smuCailSize, smuRegionStart, smuLoad.blockOffset, smuLoad.mc, smuLoad.phys, smuLoad.tasks,
			smuLoad.resp[0], smuLoad.resp[1], smuLoad.resp[2], smuLoad.resp[3], smuLoad.resp[4], smuLoad.resp[5],
			smuLoad.before, smuLoad.after, smuLoad.ms, smuLoad.mecBase[0], smuLoad.mecBase[1], smuLoad.mecBase[2]);
		add("hooks: hwlibs routed %x failed %x; peci reads %u; bin_image at setupCAIL %u missing %u, at earlyInit %u restores %u, earlyInitialize %x\n",
			hwlibsHooks, hwlibsHookFails, peciReads, binImageSetupChecks, binImageSetupMissing, binImageChecks, binImageRestores,
			earlyInitResult);
	}
	add("soft pptable: %u bytes rev %x\n", softPPTableSize, softPPTableRev);
	add("breadcrumb: %u writes, %u refused%s\n", breadcrumbWrites, breadcrumbRefused,
		breadcrumbNoOptions ? ", no /options" : "");
	{
		// Only the tail once it's long (nvram104-108: the start-up notes pushed the end of the report out of the buffer).
		size_t traceLen = strlen(traceSummary);
		add("trace: %s%s\nlast events:", traceLen > 700 ? "..." : "", traceSummary + (traceLen > 700 ? traceLen - 700 : 0));
	}
	// Newest 16 events, oldest first, without the repetitive attribute and vblank-wait calls.
	uint32_t head = __atomic_load_n(&traceHead, __ATOMIC_RELAXED);
	uint32_t picked[16], count = 0;
	for (uint32_t i = head; i != 0 && head - i < TraceRingSize && count < 16;) {
		i--;
		auto &event = traceRing[i % TraceRingSize];
		if (!event.name || !strncmp(event.name, "fb.setAttribute", 15) || !strncmp(event.name, "tg.wait", 7) ||
			!strncmp(event.name, "uc.", 3))
			continue;
		picked[count++] = i;
	}
	while (count) {
		auto &event = traceRing[picked[--count] % TraceRingSize];
		add(" %s%s=%x", event.exit ? "" : ">", event.name, event.value);
	}
	add("\ndisk log: %s, %u writes, %u errors (last %d), %u events dropped\n", diskLogPath[0] ? diskLogPath : "not started",
		diskLogWrites, diskLogErrors, diskLogLastError, diskLogEventsDropped);
	return report;
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
