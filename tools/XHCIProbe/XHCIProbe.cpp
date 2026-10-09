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
//  Results are printed with IOLog (verbose boot / dmesg) and published on the IOPCIDevice as properties
//  "XHCIProbe-before", "XHCIProbe-steps" and "XHCIProbe-after", so `ioreg -lw0 -r -n XHC0@10 | grep XHCIProbe`
//  shows them after boot.
//
//  Boot-args: -xhciprobe_noact   only record state, change nothing
//             -xhciprobe_off     do nothing at all
//
//  Register layout and bit meanings: eXtensible Host Controller Interface for USB, rev 1.2 (Intel), sections 5.3
//  (capability registers), 5.4 (operational registers), 7.1 (USB Legacy Support capability); PCI PM 1.2 (PMCSR).
//

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSString.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <string.h>

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
	constexpr UInt32 RegCapLength = 0x00, RegHciVersion = 0x02, RegHcsParams1 = 0x04, RegHccParams1 = 0x10;
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

	map->release();
	return nullptr;  // never attach: let AppleUSBXHCIPCI take the controller
}
