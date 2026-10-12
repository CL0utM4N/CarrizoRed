//
//  XHCIProbe.cpp
//  CarrizoRed tools
//
//  Diagnostic kext for xHCI controllers that macOS fails to start ("reset failed").
//
//  It matches every xHCI controller with a probe score above AppleUSBXHCIPCI's, so its probe() runs first. probe()
//  records the controller's state, (on AMD controllers, unless -xhciprobe_noact) walks through the xHCI start-up
//  steps from the public xHCI specification, records the state again, and returns nullptr. It never attaches, so
//  IOKit then probes AppleUSBXHCIPCI as usual.
//
//  On AMD controllers it also reads the firmware's power-management state through ACPI ("XHCIProbe-acpi": OSVR,
//  XHCD, U3TD and _S0W of the D509BA DSDT), and starts a monitor thread that samples the controller every 1 ms for
//  60 s after probe() returns, i.e. while AppleUSBXHCIPCI starts it. Every change of PMCSR, PCI command, USBCMD or
//  USBSTS is appended to "XHCIProbe-trace" as "+<ms> D<n> c<pcicmd> u<usbcmd> s<usbsts>" ("dead" when the
//  registers read all-ones).
//
//  Results are printed with IOLog (verbose boot / dmesg) and published on the IOPCIDevice as properties
//  "XHCIProbe-before", "XHCIProbe-steps", "XHCIProbe-after", "XHCIProbe-acpi" and "XHCIProbe-trace", so
//  `ioreg -lw0 -r -n XHC0@10 | grep XHCIProbe` shows them after boot.
//
//  For 10 minutes it also prints an interrupt summary every 2 s ("XHCIProbe: irq ..." on the verbose screen,
//  "XHCIProbe-irq" property): MSI/MSI-X enable, how often IMAN.IP and USBSTS.EINT were seen set at 1 ms samples,
//  and how often the event ring dequeue pointer moved. IP/EINT seen set most of the time = interrupts not serviced.
//  0.4 adds the interrupt target (MSI address/data or MSI-X entry 0), IMOD and host-system-error counts, and a
//  "XHCIProbe: ports ..." line per window: for each connected root port, the share of 1 ms samples spent in U0, U1,
//  U2, U3, Recovery or another link state, how often the device dropped off (CCS 1 -> 0), the speed and PORTPMSC
//  (the U1/U2 timeouts the driver programmed). Read-only, like the rest of the monitor.
//
//  0.5 switches the AMD FCH xHCI (1022:7914) to MSI-X before AppleUSBXHCIPCI starts (plain MSI never reaches the CPU
//  there; see the comment in probe()).
//
//  Boot-args: -xhciprobe_noact   only record state, don't run the start-up steps
//             -xhciprobe_nomsix  keep IOPCIFamily's default (MSI) on 1022:7914
//             -xhciprobe_nomon   no monitor thread
//             -xhciprobe_off     do nothing at all
//
//  Register layout and bit meanings: eXtensible Host Controller Interface for USB, rev 1.2 (Intel), sections 5.3
//  (capability registers), 5.4 (operational registers), 7.1 (USB Legacy Support capability); PCI PM 1.2 (PMCSR).
//

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/c++/OSString.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <string.h>

// IOPCIDevice::configureInterrupts(type, numRequired, numRequested, options) (IOPCIFamily, reserved slot 4; not in
// MacKernelSDK's header). Called non-virtually by its exported symbol.
extern "C" IOReturn XHCIProbeConfigureInterrupts(IOPCIDevice *device, UInt32 type, UInt32 numRequired,
	UInt32 numRequested, IOOptionBits options) __asm("__ZN11IOPCIDevice19configureInterruptsEjjjj");
static constexpr UInt32 InterruptTypePCIMessagedX = 0x00020000;  // IOPCIFamilyDefinitions.h

class XHCIProbe : public IOService {
	OSDeclareDefaultStructors(XHCIProbe)

public:
	IOService *probe(IOService *provider, SInt32 *score) override;
};

OSDefineMetaClassAndStructors(XHCIProbe, IOService)

namespace {
	// PCI
	constexpr UInt8 CapPowerManagement = 0x01, CapMSI = 0x05, CapMSIX = 0x11;
	constexpr UInt16 CommandMemory = 1 << 1, CommandBusMaster = 1 << 2;

	// xHCI capability registers (offsets from BAR0)
	constexpr UInt32 RegCapLength = 0x00, RegHcsParams1 = 0x04, RegHccParams1 = 0x10;
	// xHCI operational registers (offsets from BAR0 + CAPLENGTH)
	constexpr UInt32 RegUsbCmd = 0x00, RegUsbSts = 0x04;
	constexpr UInt32 CmdRun = 1 << 0, CmdReset = 1 << 1;
	constexpr UInt32 StsHalted = 1 << 0, StsHostError = 1 << 2, StsNotReady = 1 << 11, StsControllerError = 1 << 12;
	// USB Legacy Support extended capability (ID 1)
	constexpr UInt32 LegBiosOwned = 1 << 16, LegOsOwned = 1 << 24;
	constexpr UInt32 LegCtlSmiEnables = (1 << 0) | (1 << 4) | (1 << 13) | (1 << 14) | (1 << 15);
	constexpr UInt32 LegCtlSmiStatus = 7u << 29;  // write 1 to clear

	struct Controller {
		IOPCIDevice *pci;
		volatile UInt8 *base;
		UInt32 length;
		UInt8 pm, msi, msix;
		UInt32 leg;  // offset of the USB Legacy Support capability, 0 if none

		UInt32 r32(UInt32 off) const { return off + 4 <= length ? *reinterpret_cast<volatile UInt32 *>(base + off) : ~0u; }
		void w32(UInt32 off, UInt32 v) const {
			if (off + 4 <= length)
				*reinterpret_cast<volatile UInt32 *>(base + off) = v;
		}
		UInt8 capLength() const { return static_cast<UInt8>(r32(RegCapLength) & 0xFF); }
		UInt32 op(UInt32 reg) const { return capLength() + reg; }
	};

	void findLegacyCapability(Controller &c) {
		c.leg = 0;
		UInt32 hcc = c.r32(RegHccParams1);
		if (hcc == ~0u)
			return;
		UInt32 off = (hcc >> 16) << 2;
		for (int i = 0; off && i < 64; i++) {
			UInt32 v = c.r32(off);
			if (v == ~0u)
				return;
			if ((v & 0xFF) == 1) {
				c.leg = off;
				return;
			}
			UInt32 next = (v >> 8) & 0xFF;
			if (!next)
				return;
			off += next << 2;
		}
	}

	void snapshot(const Controller &c, const char *when, char *buf, size_t size) {
		auto *pci = c.pci;
		UInt16 cmd = pci->configRead16(kIOPCIConfigCommand);
		UInt16 sts = pci->configRead16(kIOPCIConfigStatus);
		UInt16 pmcsr = c.pm ? pci->configRead16(c.pm + 4) : 0xFFFF;
		UInt16 msi = c.msi ? pci->configRead16(c.msi + 2) : 0;
		UInt16 msix = c.msix ? pci->configRead16(c.msix + 2) : 0;
		UInt32 hcs = c.r32(RegHcsParams1), hcc = c.r32(RegHccParams1);
		UInt32 cap = c.r32(RegCapLength);
		UInt32 usbcmd = c.r32(c.op(RegUsbCmd)), usbsts = c.r32(c.op(RegUsbSts));
		UInt32 leg = c.leg ? c.r32(c.leg) : 0, legctl = c.leg ? c.r32(c.leg + 4) : 0;
		snprintf(buf, size,
			"%s: pcicmd=%04x pcists=%04x pmcsr=%04x(D%u) msi=%04x msix=%04x | cap=%08x(len %02x ver %04x) "
			"hcs1=%08x(ports %u) hcc1=%08x | usbcmd=%08x usbsts=%08x [%s%s%s%s] | legsup@%x=%08x [%s%s] legctl=%08x",
			when, cmd, sts, pmcsr, pmcsr & 3, msi, msix, cap, cap & 0xFF, cap >> 16, hcs, hcs >> 24, hcc, usbcmd,
			usbsts, (usbsts & StsHalted) ? "HCH " : "run ", (usbsts & StsNotReady) ? "CNR " : "",
			(usbsts & StsHostError) ? "HSE " : "", (usbsts & StsControllerError) ? "HCE" : "", c.leg, leg,
			(leg & LegBiosOwned) ? "BIOS " : "", (leg & LegOsOwned) ? "OS" : "", legctl);
		IOLog("XHCIProbe: %s\n", buf);
	}

	// Polls until (r32(off) & mask) == want; returns elapsed ms, or -1 on timeout.
	int waitFor(const Controller &c, UInt32 off, UInt32 mask, UInt32 want, int timeoutMs) {
		for (int ms = 0; ms <= timeoutMs; ms++) {
			UInt32 v = c.r32(off);
			if (v != ~0u && (v & mask) == want)
				return ms;
			IOSleep(1);
		}
		return -1;
	}

	__printflike(3, 4) void appendf(char *buf, size_t size, const char *fmt, ...) {
		size_t len = strlen(buf);
		if (len >= size)
			return;
		va_list va;
		va_start(va, fmt);
		vsnprintf(buf + len, size - len, fmt, va);
		va_end(va);
	}

	// The D509BA firmware's USB power-management state. Names that don't exist on other machines read as "-".
	void readAcpi(IOPCIDevice *pci, char *buf, size_t size) {
		buf[0] = '\0';
		auto *acpi = OSDynamicCast(IOACPIPlatformDevice, pci->getProperty("acpi-device"));
		if (!acpi) {
			appendf(buf, size, "no acpi-device");
			return;
		}
		static const char *const names[] = {"\\OSFL", "\\XHCD", "\\EHCD", "\\U3TD", "\\U_PS", "_S0W"};
		for (auto *name : names) {
			UInt32 v = 0;
			if (acpi->evaluateInteger(name, &v) == kIOReturnSuccess)
				appendf(buf, size, "%s=%x ", name, v);
			else
				appendf(buf, size, "%s=- ", name);
		}
	}

	// Monitor thread: samples the controller while AppleUSBXHCIPCI starts it, records every change.
	struct Monitor {
		Controller c;
		IOMemoryMap *map;
		char trace[4000];
	};

	constexpr int MonitorMs = 600000;  // 10 min: covers the verbose part of an installer boot
	constexpr int TraceMs = 60000;     // state-change trace only for the first minute
	constexpr int WindowMs = 2000;     // interrupt summary period
	// Runtime registers (xHCI 5.5): RTSOFF in the capability registers, interrupter 0 at +0x20.
	constexpr UInt32 RegRtsOff = 0x18, RegIman = 0x20, RegImod = 0x24, RegErdp = 0x38;
	constexpr UInt32 ImanPending = 1 << 0;
	constexpr UInt32 StsEventInterrupt = 1 << 3;
	constexpr UInt32 StsNoisy = (1 << 3) | (1 << 4);  // EINT, PCD: change on normal port activity
	// Port registers (xHCI 5.4.8-5.4.9): PORTSC at operational + 0x400 + 0x10 * (port - 1), PORTPMSC at +4.
	// PORTSC: CCS bit 0, PED bit 1, PLS bits 8:5 (0 U0, 1 U1, 2 U2, 3 U3, 5 RxDetect, 7 Polling, 8 Recovery, ...),
	// speed bits 13:10. PORTPMSC on a USB 3 port: U1 timeout bits 7:0, U2 timeout bits 15:8 (0 = disabled).
	constexpr UInt32 RegPortSc = 0x400, PortStride = 0x10;
	constexpr UInt32 MaxPorts = 16;
	// Link-state buckets per port: U0, U1, U2, U3, Recovery, other.
	enum { LsU0, LsU1, LsU2, LsU3, LsRecovery, LsOther, LsCount };

	struct PortStats {
		UInt32 states[LsCount];
		UInt32 drops;      // CCS seen going 1 -> 0
		UInt32 pmsc;       // last PORTPMSC
		UInt32 speed;      // last PORTSC speed
		bool connected;    // CCS at the last sample
		bool seen;         // connected at any sample in the window
	};

	UInt64 nowMs() {
		UInt64 ns;
		absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
		return ns / 1000000;
	}

	void monitorThread(void *arg, wait_result_t) {
		auto *m = static_cast<Monitor *>(arg);
		auto &c = m->c;
		UInt64 start = nowMs(), lastPublish = 0;
		UInt32 last[4] = {~0u, ~0u, ~0u, ~0u};
		bool changed = false, full = false;
		// Interrupt statistics per window: if MSI/MSI-X is enabled and interrupts are serviced, IMAN.IP and
		// USBSTS.EINT are cleared within microseconds and are almost never seen set at a 1 ms sample. If they stay
		// set, interrupts aren't reaching the driver and it only notices events on its own timeouts.
		UInt32 samples = 0, ipSet = 0, eintSet = 0, erdpMoves = 0, lastErdp = 0, iman = 0, imod = 0, hse = 0;
		UInt64 windowStart = 0;
		// Per-port link states (0.4): a USB 3 link that keeps dropping into U1/U2 or Recovery, or a device that drops
		// off (CCS 1 -> 0), points at link power management or signal problems rather than lost interrupts.
		static PortStats ports[MaxPorts];
		bzero(ports, sizeof(ports));
		UInt32 hcs = c.r32(RegHcsParams1);
		UInt32 numPorts = hcs == ~0u ? 0 : hcs >> 24;
		if (numPorts > MaxPorts)
			numPorts = MaxPorts;
		for (UInt64 t = 0; t < MonitorMs; t = nowMs() - start) {
			if (c.r32(RegCapLength) != ~0u) {
				UInt32 rt = c.r32(RegRtsOff) & ~0x1Fu;
				iman = c.r32(rt + RegIman);
				imod = c.r32(rt + RegImod);
				UInt32 erdp = c.r32(rt + RegErdp) & ~0xFu;
				UInt32 sts = c.r32(c.op(RegUsbSts));
				samples++;
				if (iman != ~0u && (iman & ImanPending))
					ipSet++;
				if (sts & StsEventInterrupt)
					eintSet++;
				if (sts != ~0u && (sts & StsHostError))
					hse++;
				if (erdp != lastErdp)
					erdpMoves++;
				lastErdp = erdp;
				for (UInt32 p = 0; p < numPorts; p++) {
					UInt32 sc = c.r32(c.op(RegPortSc + PortStride * p));
					if (sc == ~0u)
						break;
					auto &ps = ports[p];
					bool connected = sc & 1;
					if (ps.connected && !connected)
						ps.drops++;
					ps.connected = connected;
					if (!connected)
						continue;
					ps.seen = true;
					UInt32 pls = (sc >> 5) & 0xF;
					ps.states[pls <= 3 ? pls : pls == 8 ? LsRecovery : LsOther]++;
					ps.speed = (sc >> 10) & 0xF;
					ps.pmsc = c.r32(c.op(RegPortSc + PortStride * p + 4));
				}
			}
			if (t - windowStart >= WindowMs) {
				bool msi = c.msi && (c.pci->configRead16(c.msi + 2) & 1);
				bool msix = c.msix && (c.pci->configRead16(c.msix + 2) & 0x8000);
				// Where the interrupt goes: MSI address / data, or MSI-X table entry 0 (address, data, vector mask).
				UInt32 target = 0, data = 0, mask = 0;
				if (msi) {
					target = c.pci->configRead32(c.msi + 4);
					data = c.pci->configRead16(c.msi + ((c.pci->configRead16(c.msi + 2) & 0x80) ? 12 : 8));
				} else if (msix) {
					UInt32 table = c.pci->configRead32(c.msix + 4);
					if ((table & 7) == 0) {
						target = c.r32(table & ~7u);
						data = c.r32((table & ~7u) + 8);
						mask = c.r32((table & ~7u) + 12) & 1;
					}
				}
				char w[200];
				snprintf(w, sizeof(w), "t=%llus D%u msi=%d msix=%d to %x/%x%s iman=%x IP %u/%u EINT %u/%u ev %u imod=%x hse %u",
					t / 1000, c.pm ? c.pci->configRead16(c.pm + 4) & 3 : 0xF, msi, msix, target, data,
					mask ? " MASKED" : "", iman, ipSet, samples, eintSet, samples, erdpMoves, imod, hse);
				IOLog("XHCIProbe: irq %s\n", w);
				c.pci->setProperty("XHCIProbe-irq", w);
				// Connected ports: link-state shares in percent, drops, speed and the U1/U2 timeouts.
				char l[220];
				l[0] = '\0';
				for (UInt32 p = 0; p < numPorts; p++) {
					auto &ps = ports[p];
					if (!ps.seen && !ps.drops)
						continue;
					UInt32 n = 0;
					for (auto v : ps.states)
						n += v;
					auto pct = [&](UInt32 v) { return n ? v * 100 / n : 0; };
					appendf(l, sizeof(l), "p%u s%u U0 %u U1 %u U2 %u U3 %u Rec %u oth %u drop %u pmsc %x; ", p + 1, ps.speed,
						pct(ps.states[LsU0]), pct(ps.states[LsU1]), pct(ps.states[LsU2]), pct(ps.states[LsU3]),
						pct(ps.states[LsRecovery]), pct(ps.states[LsOther]), ps.drops, ps.pmsc);
					bool connected = ps.connected;
					bzero(&ps, sizeof(ps));
					ps.connected = connected;
				}
				IOLog("XHCIProbe: ports %s\n", l[0] ? l : "(none connected)");
				c.pci->setProperty("XHCIProbe-ports", l);
				samples = ipSet = eintSet = erdpMoves = hse = 0;
				windowStart = t;
			}
			if (t >= TraceMs) {
				IOSleep(1);
				continue;
			}
			UInt32 now[4];
			now[0] = c.pm ? c.pci->configRead16(c.pm + 4) & 3 : 0xF;
			now[1] = c.pci->configRead16(kIOPCIConfigCommand);
			UInt32 cap = c.r32(RegCapLength);
			bool dead = cap == ~0u;
			now[2] = dead ? ~0u : c.r32(c.op(RegUsbCmd));
			now[3] = dead ? ~0u : c.r32(c.op(RegUsbSts)) & ~StsNoisy;
			if (!full && memcmp(now, last, sizeof(now))) {
				memcpy(last, now, sizeof(now));
				char e[64];
				if (dead)
					snprintf(e, sizeof(e), "+%llu D%u c%04x dead; ", t, now[0], now[1]);
				else
					snprintf(e, sizeof(e), "+%llu D%u c%04x u%x s%x; ", t, now[0], now[1], now[2], now[3]);
				IOLog("XHCIProbe: trace %s\n", e);
				if (strlen(m->trace) + strlen(e) + 8 < sizeof(m->trace)) {
					appendf(m->trace, sizeof(m->trace), "%s", e);
				} else {
					appendf(m->trace, sizeof(m->trace), "FULL");
					full = true;
				}
				changed = true;
			}
			if (changed && t - lastPublish >= 500) {
				c.pci->setProperty("XHCIProbe-trace", m->trace);
				changed = false;
				lastPublish = t;
			}
			IOSleep(1);
		}
		appendf(m->trace, sizeof(m->trace), " end");
		c.pci->setProperty("XHCIProbe-trace", m->trace);
		m->map->release();
		c.pci->release();
		IOFree(m, sizeof(*m));
	}

	void startController(Controller &c, char *steps, size_t size) {
		auto *pci = c.pci;
		steps[0] = '\0';

		// 1. PCI power state D0 (PMCSR bits 1:0 = 0). D3hot -> D0 needs 10 ms (PCI PM 1.2).
		if (c.pm) {
			UInt16 pmcsr = pci->configRead16(c.pm + 4);
			if (pmcsr & 3) {
				pci->configWrite16(c.pm + 4, pmcsr & ~3);
				IOSleep(20);
				appendf(steps, size, "D%d->D0(now D%d) ", pmcsr & 3, pci->configRead16(c.pm + 4) & 3);
			} else {
				appendf(steps, size, "already-D0 ");
			}
		}

		// 2. Memory decoding and bus mastering.
		UInt16 cmd = pci->configRead16(kIOPCIConfigCommand);
		if ((cmd & (CommandMemory | CommandBusMaster)) != (CommandMemory | CommandBusMaster)) {
			pci->configWrite16(kIOPCIConfigCommand, cmd | CommandMemory | CommandBusMaster);
			appendf(steps, size, "cmd %04x->", cmd);
			appendf(steps, size, "%04x ", pci->configRead16(kIOPCIConfigCommand));
		}

		findLegacyCapability(c);

		// 3. Take ownership from the BIOS (xHCI 7.1.1) and switch off its SMIs.
		if (c.leg) {
			UInt32 leg = c.r32(c.leg);
			if (leg & LegBiosOwned) {
				c.w32(c.leg, leg | LegOsOwned);
				int ms = waitFor(c, c.leg, LegBiosOwned, 0, 1000);
				if (ms >= 0) appendf(steps, size, "handoff %dms ", ms); else appendf(steps, size, "handoff TIMEOUT ");
			} else {
				appendf(steps, size, "no-bios-owner ");
			}
			UInt32 ctl = c.r32(c.leg + 4);
			c.w32(c.leg + 4, (ctl & ~LegCtlSmiEnables) | LegCtlSmiStatus);
		} else {
			appendf(steps, size, "no-legacy-cap ");
		}

		if (c.r32(RegCapLength) == ~0u) {
			appendf(steps, size, "REGS-ALL-ONES(abort)");
			return;
		}

		// 4. Controller Not Ready must clear before the operational registers may be written (xHCI 4.2).
		int ms = waitFor(c, c.op(RegUsbSts), StsNotReady, 0, 10000);
		if (ms >= 0) appendf(steps, size, "CNR-clear %dms ", ms); else appendf(steps, size, "CNR TIMEOUT ");

		// 5. Halt (clear R/S, wait for HCH; xHCI allows 16 ms).
		UInt32 usbcmd = c.r32(c.op(RegUsbCmd));
		if (usbcmd & CmdRun) {
			c.w32(c.op(RegUsbCmd), usbcmd & ~CmdRun);
			ms = waitFor(c, c.op(RegUsbSts), StsHalted, StsHalted, 100);
			if (ms >= 0) appendf(steps, size, "halt %dms ", ms); else appendf(steps, size, "halt TIMEOUT ");
		} else {
			appendf(steps, size, "was-halted ");
		}

		// 6. Host Controller Reset; wait for HCRST and CNR to clear (Linux allows up to 10 s).
		c.w32(c.op(RegUsbCmd), c.r32(c.op(RegUsbCmd)) | CmdReset);
		IOSleep(1);
		ms = waitFor(c, c.op(RegUsbCmd), CmdReset, 0, 10000);
		if (ms >= 0) appendf(steps, size, "HCRST %dms ", ms); else appendf(steps, size, "HCRST TIMEOUT ");
		ms = waitFor(c, c.op(RegUsbSts), StsNotReady, 0, 10000);
		if (ms >= 0) appendf(steps, size, "ready %dms", ms); else appendf(steps, size, "ready TIMEOUT");
	}
}

IOService *XHCIProbe::probe(IOService *provider, SInt32 *score) {
	char arg[8];
	if (PE_parse_boot_argn("-xhciprobe_off", arg, sizeof(arg)))
		return nullptr;

	auto *pci = OSDynamicCast(IOPCIDevice, provider);
	if (!pci)
		return nullptr;

	UInt16 vendor = pci->configRead16(kIOPCIConfigVendorID), device = pci->configRead16(kIOPCIConfigDeviceID);
	IOLog("XHCIProbe: %s %04x:%04x at %u:%u.%u\n", pci->getName() ? pci->getName() : "?", vendor, device,
		pci->getBusNumber(), pci->getDeviceNumber(), pci->getFunctionNumber());

	pci->setMemoryEnable(true);
	IOMemoryMap *map = pci->mapDeviceMemoryWithIndex(0);
	if (!map) {
		IOLog("XHCIProbe: cannot map BAR0\n");
		pci->setProperty("XHCIProbe-before", "cannot map BAR0");
		return nullptr;
	}

	Controller c {};
	c.pci = pci;
	c.base = reinterpret_cast<volatile UInt8 *>(map->getVirtualAddress());
	c.length = static_cast<UInt32>(map->getLength());
	pci->findPCICapability(CapPowerManagement, &c.pm);
	pci->findPCICapability(CapMSI, &c.msi);
	pci->findPCICapability(CapMSIX, &c.msix);
	findLegacyCapability(c);

	char buf[640];
	snapshot(c, "before", buf, sizeof(buf));
	pci->setProperty("XHCIProbe-before", buf);

	bool act = vendor == 0x1022 && !PE_parse_boot_argn("-xhciprobe_noact", arg, sizeof(arg));
	if (act) {
		char steps[256];
		startController(c, steps, sizeof(steps));
		IOLog("XHCIProbe: steps: %s\n", steps);
		pci->setProperty("XHCIProbe-steps", steps);
		snapshot(c, "after", buf, sizeof(buf));
		pci->setProperty("XHCIProbe-after", buf);
	}

	// MSI-X (0.5). On the D509BA's FCH xHCI (1022:7914) plain MSI never reaches the CPU: with MSI (vector 0x71,
	// address 0xFEE10000) USBSTS.EINT stayed set in every 1 ms sample and the event ring was serviced once every 2 s,
	// by AppleUSBXHCI's fallback timer, which made USB storage crawl. Linux drives the same controller with MSI-X.
	// IOPCIFamily only picks MSI-X by itself for Ethernet controllers (IOPCIBridge::childPrefersMSIX), but a driver
	// may ask for it before the interrupt vectors are resolved, which happens lazily when AppleUSBXHCIPCI registers
	// its interrupt (after this probe). MSI-X vectors are also marked kIOInterruptTypePCIMessaged, which is what
	// AppleUSBXHCIPCI::start looks for. -xhciprobe_nomsix keeps MSI. Independent of -xhciprobe_noact.
	if (vendor == 0x1022 && device == 0x7914 && c.msix && !PE_parse_boot_argn("-xhciprobe_nomsix", arg, sizeof(arg))) {
		IOReturn ret = XHCIProbeConfigureInterrupts(pci, InterruptTypePCIMessagedX, 1, 1, 0);
		char msix[96];
		snprintf(msix, sizeof(msix), "configureInterrupts(MSI-X, 1, 1) = 0x%x, MSI-X control %04x", ret,
			pci->configRead16(c.msix + 2));
		IOLog("XHCIProbe: %s\n", msix);
		pci->setProperty("XHCIProbe-msix", msix);
	}

	if (vendor == 0x1022) {
		char acpi[160];
		readAcpi(pci, acpi, sizeof(acpi));
		IOLog("XHCIProbe: acpi: %s\n", acpi);
		pci->setProperty("XHCIProbe-acpi", acpi);
	}

	if (vendor == 0x1022 && !PE_parse_boot_argn("-xhciprobe_nomon", arg, sizeof(arg))) {
		auto *m = static_cast<Monitor *>(IOMalloc(sizeof(Monitor)));
		if (m) {
			m->c = c;
			m->map = map;
			m->trace[0] = '\0';
			pci->retain();
			thread_t thread;
			if (kernel_thread_start(monitorThread, m, &thread) == KERN_SUCCESS) {
				thread_deallocate(thread);
				// The thread runs our code for 60 s; keep an instance alive so the kext is never unloaded under it.
				retain();
				return nullptr;
			}
			pci->release();
			IOFree(m, sizeof(*m));
		}
	}

	map->release();
	return nullptr;  // never attach: let AppleUSBXHCIPCI take the controller
}
