# USB on AMD Carrizo / Bristol Ridge / Stoney Ridge: lost xHCI interrupts and FCHXHCIFix

**Summary.** On these AMD APUs the USB 3 controller built into the chip, the FCH xHCI (PCI `1022:7914`), works under
macOS but **none of its interrupts reach macOS**. Apple's USB driver then services it only from a 2-second fallback
timer, so every USB transfer can wait up to 2 seconds. USB storage crawls (about 130 KB/s), installing macOS from a
USB stick takes most of a day, and mice and keyboards lag or drop out. [FCHXHCIFix](../FCHXHCIFix/README.md) works
around it by waking Apple's USB interrupt handler itself every ~2.5 ms. With it, USB runs at normal speed.

This was found and fixed on an ASUS VivoBook 15 D509BA (A9-9425, Stoney Ridge) running macOS Sequoia, while bringing
up CarrizoRed. The kext is independent of CarrizoRed and of Lilu.

## The controller

| | |
|---|---|
| PCI ID | `1022:7914` "FCH USB XHCI Controller", revision `0x20`, class `0c0330` (xHCI) |
| Location | `00:10.0`, inside the APU (AMD's FCH, the on-die southbridge) |
| Spec | xHCI 1.0; USB 3.x Gen 1 (5 Gbps); 8 root ports. On the D509BA: a USB-A 3 and a USB-C socket (ports 1-2 USB 3, 5-6 their USB 2 halves) and two USB 2 Type-A sockets (ports 7-8) |
| Interrupts | MSI (1 of up to 8 vectors, 64-bit, not maskable) and MSI-X (8 vectors, table in BAR0 at `0x1000`) |
| Companion | the FCH EHCI `1022:7908` (on the D509BA only an internal hub with the webcam and Bluetooth) |
| macOS driver | Apple's generic `AppleUSBXHCIPCI` (no AMD-specific class or quirk for this device) |
| Linux driver | `xhci_hcd` with no device-specific quirk (only the generic spurious-success one); uses MSI-X |

Don't confuse it with Ryzen machines, where `1022:7914` shows up as the **subsystem** ID of a different USB
controller (e.g. Raven `1022:15e0`, Matisse `1022:149c`). Those controllers are not affected, and FCHXHCIFix doesn't
match them: it uses `IOPCIPrimaryMatch`, which compares the device's own vendor and device ID.

### Check whether a machine has it

- **macOS:** `ioreg -lw0 -r -c IOPCIDevice | grep -E 'device-id|vendor-id' -n` and look for the device with
  `vendor-id <22100000>` and `device-id <14790000>`. Or, in System Information → USB, look for a controller on PCI
  device `0x7914`.
- **Linux:** `lspci -nn | grep 1022:7914`. It should show `00:10.0 USB controller [0c03]: … FCH USB XHCI Controller
  [1022:7914] (rev 20)`.
- **Windows:** Device Manager → Universal Serial Bus controllers → AMD USB 3.0 eXtensible Host Controller →
  Properties → Details → Hardware Ids: `PCI\VEN_1022&DEV_7914`.

## Which computers have it

Every machine with one of these APUs has this controller, because it is part of the chip:

| Platform | Codename / socket | Examples |
|---|---|---|
| 6th-gen A-series (2015) | **Carrizo**, FP4 (laptops) | A8-8600P, A10-8700P, FX-8800P, Pro A10-8730B, A12-8800B |
| 7th-gen A-series (2016–17) | **Bristol Ridge**, FP4 (laptops) | A9-9410, A10-9600P/9620P, A12-9700P/9720P, FX-9800P/9830P |
| 7th-gen A-series desktop | **Bristol Ridge**, AM4 | A6-9500, A8-9600, A10-9700, A12-9800(E), Athlon X4 940/950/970 |
| 7th-gen E/A-series (2016–19) | **Stoney Ridge**, FT4 (budget laptops, Chromebooks) | E2-9000(e), A4-9120(C), A6-9220/9225, A9-9420/9425 |

Models confirmed from public Linux hardware probes (`lspci` output with the controller at `00:10.0`, from the
[linuxhw/LsPCI](https://github.com/linuxhw/LsPCI) dataset):

| Machine | APU graphics (so platform) |
|---|---|
| ASUS VivoBook 15 **D509BA** | Stoney (R5), A9-9425; the machine this was found on |
| Lenovo IdeaPad **330-14AST** | Stoney |
| Multilaser **PC150** notebook | Stoney |
| Fusion5 **S14** notebook | Stoney |
| ATOPNUC **MA90** mini PC | Stoney |
| Lenovo **3100** desktop | Wani (Carrizo/Bristol Ridge graphics) |
| Biostar **FX9830M** board | Wani, FX-9830P |
| OEM **AM4** desktop | Wani, Bristol Ridge AM4 APU |
| MSI **MS-7A38**, **MS-7B38**, **MS-7B84** (AM4 boards) | Bristol Ridge AM4 (APU or Athlon X4) |

Many other machines of that era use these APUs (examples, not individually checked): HP 14/15 and 240/245/255 G6/G7, Lenovo IdeaPad 110/310/320/330
(…AST models), Acer Aspire 3/5 with A9/A6, Dell Inspiron 3000 AMD models, HP EliteBook 725/745/755 G3/G4, Lenovo
ThinkPad A275/A475, and Stoney Chromebooks (HP Chromebook 14 db0, Acer Chromebook 315, Lenovo 14e). Whether *each*
of them shows the lost-interrupt problem under macOS is only proven for the D509BA; the controller, its revision and
the APU's interrupt path are shared, so the others very likely do.

## Symptoms under macOS

- **USB storage is extremely slow.** On the D509BA a USB 3 stick at 5 Gbps read at ~130 KB/s. The installer's
  `validate_root_image` step sat for tens of minutes, and copying the installer took most of a day. `dd` from the
  stick appeared to hang.
- **Input devices lag,** and USB mice disconnect and reconnect.
- **Nothing in the logs says why.** The link is up at full speed and there are no errors.
- **The same machine is fine under Linux.**

## How it was diagnosed

Measured with `tools/XHCIProbe`, a read-only diagnostic kext that samples the controller every millisecond (see the
D509BA log in [TEST_MACHINES.md](TEST_MACHINES.md)):

1. **The link is healthy.** The port is in U0 100 % of the time: no U1/U2 power saving, no link recovery, no
   drop-offs, and the controller stays in D0 with no host system errors. So it isn't link power management or
   runtime D3.
2. **The interrupts never arrive.** `USBSTS.EINT` ("an interrupt was raised") was set in every sample and never
   cleared, which means Apple's interrupt handler never ran. The event ring was serviced once every 2 seconds, which
   is AppleUSBXHCI's fallback timer. `IMAN.IP` read 0. With MSI, the controller clears that bit itself when it
   sends the message, so the controller *does* send its interrupts.
3. **It isn't the interrupt type.** Switching the controller to MSI-X, the mode Linux uses, with
   `IOPCIDevice::configureInterrupts(kIOInterruptTypePCIMessagedX)` before Apple's driver started changed nothing.
4. **It isn't the CPU side.** The GPU's MSIs, sent to the same address (`0xFEE10000`), arrive and are handled.
5. **Apple's handler isn't rejecting them.** In Apple's code (15.8.1), for MSI the primary filter just calls a no-op
   `clearInterruptPending` and accepts every interrupt, and `primaryInterrupt` drains the event ring without further
   checks. Any interrupt that arrived would be processed.
6. **Waking the handler only when `EINT` is set isn't enough.** The controller then flags new events only every 1–2 s
   (the IO interrupter completed 1–4 batches per 2 s). Waking it continuously makes USB fast.

**Still open:** why this controller's MSI/MSI-X writes never become CPU interrupts under macOS when they do under Linux.
Point 6 also suggests the controller doesn't re-raise an interrupt until something it waits for happens (an event
handler busy / interrupt pending hand-shake), which only the timer path provided. Anyone with a hardware MSI trace or
AMD documentation for the FCH (BKDG for Family 15h Models 60h–6Fh / 70h–7Fh) is welcome to dig further.

## The fix: FCHXHCIFix

A plain IOKit kext, no Lilu. It:
1. matches the controller (`IOPCIPrimaryMatch` `0x79141022`) in its own `IOMatchCategory`, so it attaches next to
   AppleUSBXHCIPCI instead of competing with it;
2. waits for AppleUSBXHCIPCI to start, then finds that driver's `IOFilterInterruptEventSource` for the controller on
   its work loop (walking the work loop's event chain inside the gate);
3. every ~2.5 ms calls the source's `signalInterrupt()`. This runs AppleUSBXHCI's normal interrupt handler on its work
   loop, exactly as a delivered interrupt would. When there's nothing to do, the handler returns at once. It also
   clears `USBSTS.EINT` when set.

Its state is in the controller's `FCHXHCIFix-state` property:
```
ioreg -lw0 -r -c IOPCIDevice | grep FCHXHCIFix-state
"FCHXHCIFix-state" = "1 sources, 482113 forwards, 482113 polls; running, mode 2 (every poll)"
```

| Boot-arg | Effect |
|---|---|
| `-fchxhcioff` | Disable it |
| `fchxhcimode=1` | Wake the handler only when `USBSTS.EINT` is set (for diagnosis; not enough on its own) |

**Install:** add `FCHXHCIFix.kext` to `EFI/OC/Kexts` and to `config.plist` → Kernel → Add (no dependencies, any
order). It needs to be in the EFI from the start if you install macOS from a USB stick: the installer reads
everything from USB.

**Cost:** one work-loop wake-up every ~2.5 ms. That's negligible for CPU use, but it does keep the CPU from its
deepest idle states, which may cost some battery life until the root cause is found.

**Limitations:**
- Interrupt latency is up to ~2.5 ms instead of microseconds. Fine for storage and input. Audio interfaces and
  webcams on this controller are untested.
- It doesn't fix sleep or wake, if those have their own problems on these machines.
- Tested on Sequoia 15.4 (recovery and installer) on the D509BA only.

## Related fixes on the same machine

These aren't caused by the interrupt problem, but they're needed for this controller under macOS on the D509BA:
- **The firmware's xHCI D3cold path.** With the firmware in Windows 8+ mode (`_OSI`), `XHC0._PS3` cuts the
  controller's power through an SMI, and `_PS0` has to reload its firmware; macOS never gets it back. `SSDT-XOSI`
  clears the `XHCD` flag (see [opencore/SSDT-XOSI.dsl](opencore/SSDT-XOSI.dsl)).
- **Reset timeout.** `AppleUSBXHCI::reset()` waits only 100 ms for `USBCMD.HCRST`; this controller needs ~105 ms. An
  OpenCore kernel patch raises it to 1000 ms (see `scripts/build_efi_d509ba.py`).
- **USB map:** [opencore/USBMap.md](opencore/USBMap.md).
