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

static bool routeChecked(KernelPatcher &patcher, size_t index, KernelPatcher::RouteRequest &request,
	mach_vm_address_t slide, size_t size) {
	if (request.org) {
		auto func = patcher.solveSymbol(index, request.symbol, slide, size);
		if (!func) {
			SYSLOG("cr", "route: %s not found (error %d)", request.symbol, patcher.getError());
			return false;
		}
		size_t copied = 0;
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
			if (ripRelative || (hs.flags & F_RELATIVE)) {
				SYSLOG("cr", "route: REFUSED %s: %s instruction at +0x%lx would be copied unrelocated", request.symbol,
					ripRelative ? "RIP-relative" : "relative-branch", copied);
				return false;
			}
			copied += len;
		}
		// Trampoline: the copied instructions (safe to move, checked above), then jmp [rip+0] back to func + copied.
		if (routePoolUsed + copied + 14 > RoutePoolSize) {
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
		memcpy(tramp, reinterpret_cast<const void *>(func), copied);
		static const uint8_t jumpBack[6] {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
		memcpy(tramp + copied, jumpBack, sizeof(jumpBack));
		mach_vm_address_t back = func + copied;
		memcpy(tramp + copied + sizeof(jumpBack), &back, sizeof(back));
		MachInfo::setKernelWriting(false, nullptr);
		routePoolUsed += (copied + 14 + 15) & ~static_cast<size_t>(15);
		*request.org = reinterpret_cast<mach_vm_address_t>(tramp);
		KernelPatcher::RouteRequest jump {request.symbol, request.to};
		if (!patcher.routeMultipleLong(index, &jump, 1, slide, size)) {
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

static constexpr size_t TraceRingSize = 512, MaxTraceStages = 160;
static constexpr uint64_t WatchdogMs = 60000;
static TraceEvent traceRing[TraceRingSize];
static uint32_t traceHead;
static uint32_t stageOpen[MaxTraceStages];
static uint64_t stageEnterMs[MaxTraceStages];
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
static const uint8_t isaReplace[] {0x1E}, smallPrimReplace[] {0x00}, walkFenceReplace[] {0xA8}, dccReplace[] {0x90, 0xE9};
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
};
static mach_vm_address_t orgCsValidatePage;
static bool scPatchHooked;
static uint32_t scPatchCalls;

static void wrapCsValidatePage(void *vnode, void *pager, uint64_t offset, const void *data, int *validated,
	int *tainted, int *nx) {
	FunctionCast(wrapCsValidatePage, orgCsValidatePage)(vnode, pager, offset, data, validated, tainted, nx);
	__atomic_fetch_add(&scPatchCalls, 1, __ATOMIC_RELAXED);
	if (!data || !validated || !*validated || (offset & 0xFFF))
		return;
	auto *page = const_cast<uint8_t *>(static_cast<const uint8_t *>(data));
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

void CRDriver::processPatcher(KernelPatcher &patcher) {
	physmapBaseVar = reinterpret_cast<uint64_t *>(patcher.solveSymbol(KernelPatcher::KernelID, "_physmap_base"));
	physmapMaxVar = reinterpret_cast<uint64_t *>(patcher.solveSymbol(KernelPatcher::KernelID, "_physmap_max"));
	patcher.clearError();
	bool anyPagePatch = false;
	for (auto &patch : pagePatches) {
		patch.enabled = checkKernelArgument(patch.arg) == patch.optIn && patch.pageOffset + patch.size <= 0x1000 &&
			patch.at + patch.replaceSize <= patch.size;
		anyPagePatch |= patch.enabled;
	}
	if (anyPagePatch) {
		// Long route, chained with any other wrapper of the same function (RestrictEvents has one).
		KernelPatcher::RouteRequest request {"_cs_validate_page", wrapCsValidatePage, orgCsValidatePage};
		scPatchHooked = patcher.routeMultipleLong(KernelPatcher::KernelID, &request, 1) && orgCsValidatePage &&
			orgCsValidatePage != EINVAL;
		if (!scPatchHooked) {
			SYSLOG("cr", "failed to hook cs_validate_page for the Metal patches (error %d)", patcher.getError());
			orgCsValidatePage = 0;
			patcher.clearError();
		}
	}

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

// GPU clock: the SMU's current and target SCLK DPM level (ixTARGET_AND_CURRENT_PROFILE_INDEX, SMC 0xD0200014: current
// [20:16], target [25:21]; Linux smu8_read_sensor) and the GFX rail's voltage ID (ixSMUSVI_GFX_CURRENTVID 0xD8230048
// [31:24], mV = (6200 - 25 * vid) / 4), sampled every 500 ms. Read through MP0PUB_IND_INDEX_13/DATA_13 (0x19A/0x19B):
// Apple's PowerPlay uses window 0 (0x180/0x181), and a read between its index write and data access would send its
// access to the wrong address. No SMU messages (the mailbox is Apple's). Levels map to MHz through the VBIOS clock
// table.
static uint32_t sclkCurrent[32], sclkTarget[32], sclkSamples, sclkLast, gfxVidMin = ~0U, gfxVidMax;
static void smuReadClockTable();  // defined with the SMU loader

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
	sclkCurrent[(profile >> 16) & 0x1F]++;
	sclkTarget[(profile >> 21) & 0x1F]++;
	uint32_t vid = smuWindowRead(0xD8230048) >> 24;
	if (vid < gfxVidMin)
		gfxVidMin = vid;
	if (vid > gfxVidMax)
		gfxVidMax = vid;
	sclkSamples++;
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
		checkKernelArgument("-crnodirectucode") || self->family != ChipFamily::Stoney;
	uint32_t panicAt = 0, stallRptr = 0, stallTicks = 0;
	bool stallPanicOff = checkKernelArgument("-crnostallpanic");
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
		if (self->family == ChipFamily::Stoney)
			sampleGpuClock();
		if (self->family == ChipFamily::Stoney && t >= 60)
			smuReadClockTable();
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
				stuck = __atomic_load_n(&stageOpen[i], __ATOMIC_RELAXED) && ms - stageEnterMs[i] > WatchdogMs;
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
		// After a reset the CP microcode is gone and only the SMU can reload it (see smuReloadCp).
		if (calls > 1 && !strcmp(stages[N].name, "pm4.initializeMicroEngine"))
			smuReloadCp();
		traceEvent(stages[N].name, false, arg);
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
		if (stages[N].summary)
			traceNote("%s=%x ", stages[N].name, static_cast<uint32_t>(ret));
		return ret;
	}

#define CR_WRAP(n) reinterpret_cast<mach_vm_address_t>(wrapStage<n>)
	void routeStages(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size, TraceKext kext) {
		static_assert(arrsize(stages) == 157, "one wrapper per stage");
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
			CR_WRAP(153), CR_WRAP(154), CR_WRAP(155), CR_WRAP(156),
		};
		for (size_t i = 0; i < arrsize(stages); i++) {
			if (stages[i].kext != kext)
				continue;
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
	if (auto *hw = static_cast<uint8_t *>(accelHardware))
		if (auto *pm4 = *reinterpret_cast<uint8_t **>(hw + 0x3B0))
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
	}

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
	static constexpr uint16_t GetMaxSclkLevel = 0x3B, SetClkTableAddrHi = 0x49, SetClkTableAddrLo = 0x4A,
	                          ClkTableXferToDram = 0x267;  // cz_ppsmc.h
	// UCODE_ID_* (smu_ucode_xfer_cz.h); Stoney has no SDMA1 or MEC2, Linux maps them to SDMA0 and MEC_JT1.
	static constexpr uint8_t SDMA0 = 0, CE = 2, PFP = 3, ME = 4, MEC_JT1 = 5, RLC_G = 8, RLC_SCRATCH = 9,
	                         RLC_SRM_ARAM = 10, RLC_SRM_DRAM = 11;
	static constexpr uint32_t StoneyLoadMask = 1U << SDMA0 | 1U << CE | 1U << PFP | 1U << ME | 1U << MEC_JT1 | 1U << RLC_G;
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
static Smu8::CailUcode **stoneyUcodeInfo;  // RLC, SDMA0, SDMA1 (skipped), CE, PFP, ME, MEC1, MEC2 (skipped)
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
	// Never write outside the carve-out (an FB offset past its end would be some other system RAM).
	uint64_t fbSize = ((static_cast<uint64_t>(mmio[Reg::MC_VM_FB_LOCATION] >> 16) + 1) << 24) - fbBase;
	if (regionFb + RegionSize > fbSize) {
		smuLoad.result = "region outside the carve-out";
		return;
	}

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
	smuClkTableJob = used;
	smuClkTableAt = scratchAt[ClkTable];
	scratch(TaskInitialize, ArgInitClkTable, ClkTable, true);
	// Not in Linux's TOC: the CP alone (CE, PFP, ME, MEC jump table), for reloading after Apple's GPU reset.
	smuCpReloadTask = used;
	load(CE, Ce, false);
	load(PFP, Pfp, false);
	load(ME, Me, false);
	load(MEC_JT1, Mec, true);
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
	for (uint32_t i = 0; i < 100000; i++) {  // 1 s
		status = smuReadSram(UcodeLoadStatus);
		if ((status & StoneyLoadMask) == StoneyLoadMask)
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
	smuLoad.result = (status & StoneyLoadMask) == StoneyLoadMask ? "loaded" : "timed out";
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
	constexpr uint32_t CP_ME_CNTL = 0x21B6, mask = 1U << CE | 1U << PFP | 1U << ME | 1U << MEC_JT1;
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
static mach_vm_address_t hwlibsTextStart;
static uint64_t hwlibsTextSize;
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
		if (!hwlibsWired && !checkKernelArgument("-crnohwlibswire")) {
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
							seg->vmaddr >= textBase && seg->vmaddr - textBase + seg->vmsize <= size) {
							text = slide + (seg->vmaddr - textBase);
							textSize = seg->vmsize;
						}
					}
					cmd = reinterpret_cast<const load_command *>(reinterpret_cast<const uint8_t *>(cmd) + cmd->cmdsize);
				}
			}
			hwlibsTextStart = text;
			hwlibsTextSize = textSize;
			if (text && textSize) {
				// Touch every page first, then wire.
				volatile uint8_t sink = 0;
				for (uint64_t off = 0; off < textSize; off += PAGE_SIZE)
					sink = sink + *reinterpret_cast<volatile const uint8_t *>(text + off);
				(void)sink;
				hwlibsWired = IOMemoryDescriptor::withAddressRange(text, textSize, kIODirectionInOut, kernel_task);
				hwlibsWireResult = hwlibsWired ? hwlibsWired->prepare() : kIOReturnNoMemory;
				if (hwlibsWireResult != kIOReturnSuccess)
					OSSafeReleaseNULL(hwlibsWired);
			}
			traceNote("hwlibs=%s ", hwlibsWired ? "wired" : "notwired");
		}
		hwlibsPatcher = &patcher;
		hwlibsIndex = index;
		hwlibsSize = size;
		hwlibsSlide = slide;
		hwlibsEarlyInit = patcher.solveSymbol(index,
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
					PE_parse_boot_argn("crsmustep", &smuStepLimit, sizeof(smuStepLimit));
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
static IOMemoryMap *ihRingMap;
static const char *ihMoveResult;
static uint64_t ihMovedGpu;

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
	memset(reinterpret_cast<void *>(ihRingMap->getVirtualAddress()), 0, size + 0x1000);
	struct {
		uint64_t cpu, gpu, size;
	} info {static_cast<uint64_t>(ihRingMap->getVirtualAddress()), smuLoad.mc + IhOffset, size};
	ihMovedGpu = info.gpu;
	reinterpret_cast<uint32_t (*)(void *, void *)>(ihSetRingBufferLocation)(manager, &info);
	ihMoveResult = "moved";
	traceNote("ihring=%llx ", info.gpu);
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
// needed. Everything else is read through the physmap after flushing the lines, the carve-out too: its uncached
// mapping reads 8 bytes per memory access, a cache line fill 64 (nvram179: command buffers and shader code in the
// carve-out made most of the submit hook's time).
static bool ibReadPhys(uint64_t phys, uint32_t bytes, void *out, bool coherent = false) {
	// Only RAM and the carve-out (below 8.5 GB here), never the PCI hole.
	if (!phys || (phys >= 0xC0000000ULL && phys < 0x100000000ULL) || phys + bytes > 0x220000000ULL)
		return false;
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
	uint32_t probeReverts, blockMoves, lastBlockFail;
};
static InterpFixStats interpFix;

static bool ibWritePhys(uint64_t phys, uint32_t bytes, const void *in) {
	if (!phys || (phys >= 0xC0000000ULL && phys < 0x100000000ULL) || phys + bytes > 0x220000000ULL)
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
			uint32_t op = (w >> 18) & 0x7F, dmask = (w >> 8) & 0xF;
			bool d16 = code[1] >> 31;
			uint8_t count = static_cast<uint8_t>(__builtin_popcount(dmask) ? __builtin_popcount(dmask) : 1);
			bool gather = op >= 0x40 && op <= 0x5F;
			bool store = op >= 0x08 && op <= 0x0B, atomic = op >= 0x0F && op <= 0x1C;
			add(1, 0, 8, 4, true, false);
			if (store)
				add(1, 8, 8, count, true, false, !d16);
			else if (atomic)
				add(1, 8, 8, 4, true, true);
			else
				add(1, 8, 8, gather ? 4 : count, false, true, !d16);
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
static uint8_t cfgIsStart[0x8000];
static uint16_t cfgPrev[0x8000], cfgFirstIn[0x8000];
static uint16_t cfgBranchFrom[0x1000], cfgNextIn[0x1000];

static bool cfgBuild(const uint32_t *code, uint32_t end) {
	if (end > arrsize(cfgIsStart))
		return false;
	memset(cfgIsStart, 0, end);
	memset(cfgFirstIn, 0, end * sizeof(cfgFirstIn[0]));
	uint32_t prev = 0xFFFF, branches = 0;
	for (uint32_t i = 0; i < end; i += gcn3InstLength(code + i, end - i)) {
		cfgIsStart[i] = 1;
		cfgPrev[i] = static_cast<uint16_t>(prev);
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
		cfgBranchFrom[branches] = static_cast<uint16_t>(i);
		cfgNextIn[branches] = cfgFirstIn[target];
		cfgFirstIn[target] = static_cast<uint16_t>(++branches);
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

static uint8_t webKind[0x8000], webDef[0x8000], webIn[0x8000], webSeen[0x8000], webEntry[0x8000];
static uint16_t webStack[0xA000];

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
	auto addChange = [&](uint32_t i, const VgprField &field, uint32_t reg, uint32_t member) {
		for (uint32_t c = 0; c < outNum; c++) {
			if (out[c].at != i || out[c].field.dword != field.dword || out[c].field.shift != field.shift)
				continue;
			if (out[c].reg != reg)
				return false;
			out[c].voted |= 1U << (member - base);
			return true;
		}
		if (outNum == outMax)
			return false;
		out[outNum++] = {i, field, reg, 1U << (member - base)};
		return true;
	};
	// Operands a member must move with the others: instruction position, bit 15 set for its write.
	static uint16_t forced[16][32];
	uint32_t forcedNum[16] {};
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
					webStack[sp++] = static_cast<uint16_t>(y);
			};
			auto preds = [&](uint32_t y) {
				if (y == 0 && !entry) {
					entry = true;
					grew = true;
				}
				uint32_t p = cfgPrev[y];
				if (p != 0xFFFF) {
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
			uint32_t site = forced[r - base][s] & 0x7FFF;
			if (forced[r - base][s] & 0x8000)
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
				if (entry && moveRead && (webEntry[i] & 1) && !(v == base && f[q].count == k)) {
					fail = member | 3;  // the starting value is read outside the block reads: not an unused slot
					return false;
				}
				if (f[q].count > 1 && !inBlock) {
					fail = member | 5;  // a register range reaching outside the block
					return false;
				}
				if (!addChange(i, f[q], F + (f[q].count > 1 ? v : r) - base, r)) {
					fail = member | 7;
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
				sites[siteNum++] = out[c].at | 0x8000;
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
				forced[m][forcedNum[m]++] = static_cast<uint16_t>(sites[s]);
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
			static uint8_t state[0x8000], start[0x8000];
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
			// needed).
			uint32_t F = maxVgpr + 1;
			uint32_t need = F + blockCount;
			uint32_t newAllocated = allocated;
			if (need > allocated) {
				if (!allowBump || need > 256) {
					interpFix.lastSkipWhy = 6;
					skip = SkipRange;
					return fixedHere;
				}
				newAllocated = (need + 3) & ~3U;
			}
			static RegChange changes[256];
			uint32_t changeNum = 0, blockFail = 0;
			if (!planBlockMove(code, end, at, p2, vI, blockAt, blockBase, blockCount, F, changes, changeNum,
					arrsize(changes), blockFail)) {
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
struct SeenShader {
	uint64_t va;
	uint32_t vmid, hash, vgprs;
	bool pending;
	uint32_t probeAt, probeVal;  // a fixed instruction and its fixed value (probeAt 0xFFFFFFFF: none)
};
static SeenShader seenShaders[512];
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
	// Up to 128 KB of code (nvram173/174: some pixel shaders run past 8 KB, a few past 32 KB).
	static uint32_t code[0x8000], original[0x8000];
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
		if (!listed && skippedNum < arrsize(skippedShaders))
			skippedShaders[skippedNum++] = {psVa, have,
				skip == SkipUnknownEncoding ? interpFix.lastUnknownAt : interpFix.lastHazardAt, static_cast<uint32_t>(skip),
				interpFix.lastSkipWhy, skip == SkipUnknownEncoding ? interpFix.lastUnknownWord :
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
	SeenShader fresh {psVa, vmid, codeHash(print, 16), ok ? newVgprs : 0,
		skip == SkipNoFreeVgpr && !allowBump, probeAt, probeVal};
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
	for (uint32_t j = 0; j < got;) {
		uint32_t header = buf[j], type = header >> 30;
		if (type == 2) {
			j++;
			continue;
		}
		uint32_t count = ((header >> 16) & 0x3FFF) + 1;
		if (type == 3 && ((header >> 8) & 0xFF) == 0x76 && j + 1 < got) {
			uint32_t start = buf[j + 1] & 0xFFFF;
			bool touched = false;
			for (uint32_t k = 0; k + 1 < count && j + 2 + k < got; k++) {
				uint32_t reg = start + k, v = buf[j + 2 + k];
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
			if (touched && psLo && rsrc1) {
				uint64_t psVa = (static_cast<uint64_t>(psHi & 0xFF) << 40) | (static_cast<uint64_t>(psLo) << 8);
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
	auto *pm4 = *reinterpret_cast<uint8_t **>(hw + 0x3B0);
	auto *ring = pm4 ? *reinterpret_cast<uint8_t **>(pm4 + 0x50) : nullptr;
	auto *cpu = ring ? *reinterpret_cast<uint32_t **>(ring + 0x40) : nullptr;
	uint32_t dwords = ring ? *reinterpret_cast<uint32_t *>(ring + 0x30) : 0;
	if (!cpu || dwords < 0x100 || (dwords & (dwords - 1)))
		return;
	if (interpSubmit)
		carveInit();
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
		uint32_t count = *reinterpret_cast<uint32_t *>(info + 0xC);
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
					uint32_t dw = *reinterpret_cast<uint32_t *>(info + 0x44 + i * 0x28);
					uint64_t va = *reinterpret_cast<uint64_t *>(info + 0x50 + i * 0x28);
					if (dw && va)
						raiseOnlyIb(vmid, va, dw);
				}
				__atomic_fetch_sub(&hookDepth, 1, __ATOMIC_ACQ_REL);
				return;
			}
			vmCacheBegin();
			for (uint32_t i = 0; i < count && i < 8; i++) {
				uint32_t dw = *reinterpret_cast<uint32_t *>(info + 0x44 + i * 0x28);
				uint64_t va = *reinterpret_cast<uint64_t *>(info + 0x50 + i * 0x28);
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
	if (!hw || !mmio)
		return;
	auto *pm4 = *reinterpret_cast<uint8_t **>(hw + 0x3B0);
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
	add("page patches: hooked %d, %u pages validated;", scPatchHooked, scPatchCalls);
	for (auto &patch : pagePatches)
		add(" %s %s%u", patch.name, patch.enabled ? "" : "off ", patch.hits);
	add("\n");
	add("interp fix: scans %u, shaders %u, hazards %u, fixed %u, skipped %u (last %u), writes %u, failed %u, dead-register fixes %u, raised %u; hook %d calls %u ibs %u rsrc %u vmid %u flags %x ms %llu busy %u nomap %u raise-only %u raised-shaders %u reverts %u block-moves %u; binds %u memo %u walks %u; ib-read ms %llu ps-check ms %llu ib-dw %llu pages snooped %u plain %u vram %u\n",
		interpFix.scans, interpFix.shaders, interpFix.hazards, interpFix.fixed, interpFix.skipped, interpFix.lastSkip,
		interpFix.writes, interpFix.writeFails, interpFix.deadRegs, interpFix.bumps, interpHooked, ibHookCalls, ibHookIbs,
		ibHookRsrcBumps, ibHookLastVmid, ibHookLastFlags, ibHookNs / 1000000, ibHookBusy, ibHookNoMap, ibHookRaiseOnly,
		raisedNum, interpFix.probeReverts, interpFix.blockMoves, ibHookBinds, ibHookMemoHits, vmWalks,
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
		add("ih ring move: %s, gpu %llx", ihMoveResult ? ihMoveResult : "not tried", ihMovedGpu);
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
