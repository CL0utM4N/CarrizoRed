# Test machines

Hardware facts gathered with [`scripts/collect_linux_info.sh`](../scripts/collect_linux_info.sh) from a Linux live
session. VBIOS table revisions come from [`scripts/atom_tables.py`](../scripts/atom_tables.py).

## ASUS VivoBook 15 D509BA (X509BA), A9-9425: Stoney Ridge

Collected 2026-10-09, Ubuntu live, kernel per `uname.txt` in the dump (`carrizored-dump-ubuntu-20261009-0215.tar.gz`,
kept outside the repo in `Desktop/CarrizoRed-RE/`).

### Platform
| | |
|---|---|
| BIOS | `X509BA.301` |
| CPU | AMD A9-9425 (Family 15h model 70h, Stoney, Excavator), 2 cores / 2 threads |
| RAM | 8 GB DDR4 (7.2 GB visible after the 512 MB carve-out) |
| Storage | WD PC SN520 512 GB NVMe (`15b7:5003`), currently Linux (EFI + ext4) |
| Wi-Fi | Qualcomm Atheros QCA9377 (`168c:0042`): **no macOS driver** |
| USB | FCH XHCI `1022:7914`, EHCI `1022:7908` |
| SATA | FCH AHCI `1022:7901` (empty 2.5" bay) |
| Audio | GPU HDMI `1002:15b3`, platform `1022:157a` |
| ACPI | `VFCT` present (64,644 bytes), `CRAT`, `IVRS`, 4 SSDTs. Full set in the dump `acpi/` |

### GPU
| | |
|---|---|
| PCI | `1002:98E4` rev **`0xDA`**, subsystem `1043:151F`, at `00:01.0` |
| Linux IP blocks | vi_common, gmc_v8_0, cz_ih, **gfx_v8_1**, sdma_v3_0 (1 instance), smu8, DCE 11.0, uvd_v6_2, vce_v3_4, acp |
| Carve-out ("VRAM") | **512 MB**, GPU address `0xF4_0000_0000`–`0xF4_1FFF_FFFF` (`MC_VM_FB_LOCATION` is high) |
| BARs | BAR0 **256 MB** at `0xE0000000` (smaller than the 512 MB carve-out), BAR2 8 MB doorbell, BAR4 I/O, BAR5 256 KB MMIO at `0xFEB00000` |
| GTT | 3,692 MB |
| VBIOS | `SWBRT46974.001`, 64,512 bytes, VFCT copy identical to debugfs `amdgpu_vbios`. VFCT image header: `00:01.0 1002:98E4`, revision field 1 |
| Firmware | SMU 26.17.0, ME 0x3A, PFP 0x58, CE 0x32, MEC 0xA1, RLC 0x4F, SDMA0 0x26, UVD 1.43, VCE 52.0 |

### VBIOS command tables that matter for Apple's handlers
| Table | Revision | Apple's handler |
|---|---|---|
| `SetPixelClock` | **1.6** | `AtiSetPixelClock_V1_V6` exists (dividers from `hwCalculateBasicClockParameters`) |
| `AdjustDisplayPll` | **1.3** | `AtiAdjustDisplayPll_V3` exists |
| `EnableSpreadSpectrumOnPPLL` | **1.3** | `AtiEnableSpreadSpectrumOnPll_V3` exists |
| `DIG1TransmitterControl` | **1.5** | `AtiTransmitterControl_V1_V5` exists |
| `SetDCEClock` | **absent** | DCE 11.0 has no `SetDCEClock` (it was added in DCE 11.1+). Check what `BaffinDisplayEngineClock::SetDisplayEngineClock` calls: if it needs `SetDCEClock`, the display engine clock must go through `SetPixelClock` V6 (`AtiSetPixelClock_V1_V6::setDisplayClock(uchar, uint)` exists) |
| `DIGxEncoderControl` / `DIG2EncoderControl` | 1.4 / 1.1 | to check |
| `SelectCRTC_Source` | 1.3 | to check |
| `EnableGraphSurfaces` / `SetCRTC_UsingDTDTiming` | 1.4 / 2.3 | to check |
| Data: `FirmwareInfo` 2.2, `Object_Header` 1.3, `IntegratedSystemInfo` 1.9, `PowerPlayInfo` 6.1, `ASIC_InternalSS_Info` 3.1 | | |

### macOS install log
- **2026-10-09, attempts 1–2 (MacPro7,1 SMBIOS):** recovery (24E263) boots to userspace, keyboard works, then
  panics ~3.5 min in: page fault `CR2 0x8` in `AppleIntelMCEReporter::start → AppleIntelMCE::Controller::controller`.
  That kext matches the board-ids of MacPro6,1 / iMacPro1,1 / MacPro7,1 and crashes on AMD CPUs. It's in the system
  KC, so the fix is a different SMBIOS: **iMac19,1**. The panic text was read back from NVRAM (`AAPL,PanicInfo*`) via
  `dmpstore -all -s FS0:\nvram.bin` in OpenShell, then unpacked from Apple's 7-bit format.
- **Attempt 3 (iMac19,1):** reaches the macOS Recovery UI, but the PS/2 keyboard and all USB ports are dead. The
  DSDT's `OSFL()` sets `OSVR` from `_OSI("Windows …")`; macOS answers only `Darwin`, so `OSVR = 3` and the HPET's
  `_STA` reports it disabled (needs `OSVR >= 0x0C`) and clears `HPEN`. Fix: `SSDT-XOSI` + `_OSI → XOSI` rename
  (DSDT, 24 hits) so the firmware sees Windows 10 under macOS. HPET `_CRS` already holds IRQ 0/8 and RTC drops its IRQ
  when `HPEN` = 1, so no IRQ patching is needed.
- **Attempt 4 (XOSI claiming Windows 10):** keyboard works, but XHC0 (`1022:7914`) is dead: AppleUSBXHCIPCI fails
  its first power-on ("reset failed" `0xe00002c9`), the controller stays powered off, and the installer waits for its
  root device forever. EHCI is fine. XHCIProbe showed the controller healthy when macOS first finds it (D0, no BIOS
  owner, HCRST done in 12 ms), so the reset itself is not the problem. The DSDT is: with `OSVR >= 0x0F` the firmware
  keeps `XHCD` set, which arms D3cold. `XHC0._PS3` then power-gates the xHCI through SMI 0x67, and `_PS0` must reload
  its firmware (`U3D0` → `LXFW`) through SMI 0x66. Fix: XOSI now claims only up to "Windows 2009" (`OSVR = 0x0E`):
  the HPET stays on (needs `>= 0x0C`) and the FCH D3cold flags are cleared. The AMD I²C/GPIO/UART devices then report
  absent (they need `>= 0x0F`); revisit this if the I²C trackpad is ever wanted.
- **Attempt 5 (XOSI = Windows 7, XHCIProbe 0.2.0):** `_S0W = 0`, so D3cold is off, but USB is still dead. The 1 ms trace
  shows the real failure: `+0 D0 halted; +150 USBCMD=HCRST (Apple's reset); +255 HCRST clear, ready; +263 D3, dead`.
  Apple's reset *succeeds*, but takes ~105 ms (ours took 12 ms at probe time). `AppleUSBXHCI::reset()` gives up just
  before that ("reset failed" `0xe00002c9` = `kIOReturnIOError`), and the driver powers the controller off for good.
  Linux allows the xHCI reset up to 10 s. `AppleUSBXHCI::reset()` waits `clock_interval_to_deadline(100, 1000000)`.
- **Attempt 6 (OpenCore patch: that deadline 100 → 1000 ms, `Base = __ZN12AppleUSBXHCI5resetEv`):** **USB works**. A
  SanDisk USB 3 stick enumerates on XHC0 at 5 Gbps and the installer boots. XHCIProbe dropped from the EFI.
- **First CarrizoRed run (installer, 2026-10-10):** loads after dropping a bad `OSBundleCompatibleVersion` (1.0 >
  version 0.0.1 made XNU reject it silently). Reports Stoney Ridge, rev `0xDA`, 512 MB, carve-out at `0x1FF000000`,
  `GB_ADDR_CONFIG` `0x22010001` (Linux's Stoney golden value). VBIOS injection said No: the platform expert was looked
  up by a registry path that doesn't resolve; now uses `IOService::getPlatform()`.
- The `shared_region … vm_shared_region_start_address() failed` and launchd `error = 37` lines in verbose recovery
  are noise; processes keep running.
- **Spoof boots 5–19 (2026-10-10/11), the "freeze ~20 s after start-up": it was a kernel panic caused by CarrizoRed's
  own tracing, invisible on screen.** `AAPL,PanicInfo` from boot 19 (NVRAM dump via OpenShell `dmpstore`): general
  protection fault in `AMD9500Controller::hwInitializeHardwareSettings + 0x12`, in WindowServer, under
  `IOFramebuffer::open → AMDFramebuffer::enableController → initializeHardware`. Lilu's `createTrampoline` copies a
  wrapped function's prologue without relocating it; with a 14-byte jump the copy includes the stack-protector load
  `mov rax, [rip + guard@GOT]` at +0xb, which then reads the wrong address (`RAX` = "aryP12OS"). `ATIController::start`
  and `initializeProjectInfo` have the same prologue; whether a boot crashed depended on kext load addresses. CarrizoRed
  now refuses to wrap any function with a RIP-relative operand or relative branch in its first 14 bytes
  (`routeChecked`). Lessons: read `AAPL,PanicInfo` before theorising about a "freeze"; the verbose console isn't
  redrawn after a panic once WindowServer has taken the screen.
- **Spoof boot 20:** Apple's own assertion `SiDisplay.cpp:1119 NULL != timingGenerator` in
  `AMD9500Controller::shutDownDisplayHardware(2)` via `ATIController::cleanUpConnections`: 6 framebuffers but timing
  generators only for the 2 real pipes. Fixed by forcing `aty_config` `CFG_FB_LIMIT` to the pipe count.
- **Spoof boot 21:** panel went **black**, no panic in NVRAM. IOFramebuffer wrote `AAPL,PathProperties0000`
  `saved-config` for `PciRoot(0x0)/Pci(0x1,0x0)`: **1366×768, 76.30 MHz** (the panel's EDID mode). So the controller
  found the panel and set a mode; the picture is what's wrong. Next: `crpanicat=60` black-box report (registers,
  trace, framebuffer tree in the NVRAM panic text).
- **Spoof boots 22–23 (`crpanicat=60` black box):** kernel alive at t=60 s. fb0 = internal panel (`conn 0x2`),
  1366×768 @ 76.3 MHz on CRTC 0, which is running (frame counter 906→907 in 20 ms; totals 1592×798). Pixel clock
  from the DP DTO: phase/modulo imply DPREFCLK 399.25 MHz vs 400 MHz from `DENTIST_DISPCLK_CNTL` (DID 0x24 → ÷9 of
  3600 MHz), fine. Backlight PWM on and full. **eDP link training fails at clock recovery**
  (`DisplayPortUtilities::doTrainingPattern1` = `0xE00002BC`, twice), after which Apple disables the transmitter
  and encoder: all DIG front ends 0, DP video streams off, so the panel is lit but gets no picture. The firmware
  trained the same panel at boot, so the encoder/transmitter parameters Apple passes Stoney's VBIOS
  (DIGxEncoderControl 1.4, DIG1TransmitterControl 1.5) are the suspect. Next report: every VBIOS command with its
  parameters, register values now vs at boot, and the training lane status.
- **Spoof boot 23:** firmware vs Apple: at boot the firmware ran the panel on **DIG0 → DP0 with one lane**
  (`DP_CONFIG.DP_UDI_LANES = 0`), link trained, stream on, panel power sequencer on. Apple's training status
  `DPCD 0x202 = 0x07`: lane 0 fully locked, lane 1 dead, so Apple trained more than one lane
  (`DPTrainingSettingsCalculator::maxLinkLanes` = min(LinkCaps, sink MAX_LANE_COUNT), blind to board wiring), failed
  clock recovery and powered the panel off (sequencer target off, state 9). Fix: cap the lanes at the firmware's.
  The VBIOS hook saw nothing: legacy atombios runs through `AtiAtomBiosParserService::executeBiosCommandTable`.
- **Spoof boot 24 (lane cap on, VBIOS calls recorded):** the cap worked (encoder and transmitter both get 1 lane,
  2.7 Gbps, DIG0, PHY A, HPD1, eDP connector 0x14), training still failed. Correction to boot 23: the buffer
  `doTrainingPattern1` fills is the lane *settings* Apple writes to DPCD 0x103 (swing stepped 0, 1, 2, then 7 = max),
  not the sink's status, so clock recovery failed even at maximum swing: a physical-layer problem. The VBIOS
  `Object_Header` confirms eDP is on UNIPHY enum 1 = PHY A (HDMI on PHY B), so the PHY is right. The PHY clock is not:
  `DIG1TransmitterControl` v1.5 gets `ucPhyClkSrcId = 0` (P1PLL), but FirmwareInfo `usUniphyDPModeExtClkFreq` =
  100 MHz ("non-zero: DP-mode Uniphy input clock from the external spread clock"), and Linux DC on DCE 11.0 only
  drives DP from `CLOCK_SOURCE_ID_EXTERNAL` (`ucPhyClkSrcId` 3). CarrizoRed now patches the transmitter parameters in
  DP modes to the external reference.
- **Spoof boot 25 (DP PHY on the external clock, 0x81 = patched in the black box):** training still fails the same
  way. Apple's encoder/transmitter parameters now match what legacy amdgpu passes for Carrizo (rate, symbol clock,
  PHY, encoder, connector, lanes, HPD, clock source). Next: the DPCD transcript (what Apple writes to the sink:
  link rate, lane count, pattern, lane set; what the sink reports at 0x202..).
- One unrelated-looking panic at 1.47 s uptime (Probabilistic GZAlloc: read 49 bytes past a 128-byte data.kalloc
  element); too early to reach NVRAM, top of the log not captured, did not recur on the next boot.
- **Spoof boot 26 (DPCD transcript):** sink caps `12 0a 01 41` (DPCD 1.2, 2.7 Gbps, **1 lane**, downspread). Apple
  writes 0x600=1, 0x100=0x0A, 0x101=1, 0x107=0x10, 0x108=1, then TPS1 (0x102=0x21); the sink reports lane 0 with no
  clock recovery (0x202 = 00) and asks for more swing up to max, never locking. Everything Apple tells the sink is
  consistent, so the PHY output isn't reaching the panel. Legacy amdgpu's eDP enable runs
  `UNIPHYTransmitterControl` with `ATOM_TRANSMITTER_ACTION_POWER_ON` (12) before `ENABLE`; Apple never does, and its
  clean-up had switched the LVTMA power sequencer off (SYNCEN gates the PHY output). CarrizoRed now injects that
  call (`-crnoedppoweron` disables) and snapshots the eDP registers when training pattern 1 starts.
- **Spoof boot 27 (eDP POWER_ON injected):** the **backlight comes on** and the sequencer is in the firmware's state
  at TPS1 (`LVTMA_PWRSEQ_STATE` 0x41F), but the sink still sees no clock recovery. Comparing against amdgpu's
  `case 5` transmitter setup showed the earlier clock-source patch wrote the wrong bits: the v1.5 config byte is
  `[1]` coherent, `[3:2]` PHY clock source, `[6:4]` HPD, so boots 25–27 sent 0x13 (coherent + a reserved bit,
  clock still P1PLL). amdgpu sends 0x1E for eDP on HPD1 (external clock, coherent). Patch corrected.
- **Spoof boot 28 (config 0x1E): the eDP link trains and the panel shows Apple's output for the first time** —
  full-screen vertical stripes (every row identical). Fixes that got here: `CFG_FB_LIMIT`, lane cap, eDP
  `POWER_ON` before `ENABLE`, transmitter external clock + coherent. Next: surface geometry and a sample of the
  scanout memory in the black box, to tell a display-fetch underflow (DISPOUT_FIFO_ERROR has been set every boot)
  from bad framebuffer content.
- **Spoof boot 29:** training succeeds (DPCD 0x202 = 07, aligned), video on, no DISPOUT FIFO error, and the scanout
  memory is a uniform grey (0xFFBFBFBF on the top, middle and bottom rows), yet the panel shows identical coloured
  rows. Apple writes `LB_MEMORY_CTRL.LB_MEMORY_SIZE` = 0x404 (sized for Polaris's 13-bit DCE 11.2 field); the firmware
  and Linux (`LB_TOTAL_NUMBER_OF_ENTRIES`) use 0x6B0. CarrizoRed now forces 0x6B0 (`-crnolbfix` disables).
- **Spoof boot 30 (LB size fixed, `LB_MEMORY_CTRL` 0x706B0):** no change. Every watched surface register equals the
  firmware's apart from the surface address (+3 MB) and pitch (1536 px). Next: an exhaustive boot-vs-now diff of
  DCCG, DMIF/DCI, all of pipe 0 (DCP incl. DVMM, LB, SCL, BLND, CRTC, FMT), LVTMA/HPD and DIG0/DP0.
- **Spoof boot 31 (exhaustive diff, 89 registers changed):** the firmware drives the panel at **6 bpc**
  (`DP_PIXEL_FORMAT.DP_COMPONENT_DEPTH` 0, `FMT_BIT_DEPTH_CONTROL` truncate to 6 bits + spatial dither); Apple sets
  8 bpc (`DP_COMPONENT_DEPTH` 1, no FMT reduction, encoder `ucBitPerColor` = `PANEL_8BIT_PER_COLOR`). An 18 bpp
  panel decoding 24 bpp data gives identical coloured rows. The rest of the diff is Apple's own gamma/regamma LUT,
  gamut remap, cursor, scaler init, CRTC double-buffer settings and status/counter registers. CarrizoRed now caps the
  DP colour depth at the firmware's (`-crnobpccap` disables).
- **Spoof boot 32 (6 bpc cap applied):** DP pixel format and M/N now equal the firmware's; the whole DIG0/DP0 block
  matches. The panel now flashes, still with identical random-colour rows plus a white line. Remaining differences
  are in pipe 0's processing chain (FMT, LB, DCP colour pipeline/cursor, scaler/CRTC update). Next: `-crrestore`
  writes the firmware's values back into one block at a time, 15 s apart, in a single boot.
- **Spoof boot 33 (`-crrestore`, staged):** all four steps ran (28 registers), but the panel kept switching at random
  between the coloured rows, a white line and flashing, so no step could be tied to a change. Apple rewrote some
  restored registers afterwards (`CUR_CONTROL` back on), and regamma, `LB_SYNC_RESET_SEL` and `CRTC_CONTROL` had not
  been restored. Apple's framebuffer now held the verbose console text (black/white), and the line-buffer data level
  was 0x10 against the firmware's 0x37, which suggests underflow. Next: restore everything at t=30 and hold it, and
  count underflows (XDMA counter, DMIF / blender flags, LB level range).
- **Spoof boot 34 (`-crrestore`, all at once and held):** no visible change. Pipe 0's chain now equals the firmware's
  except the surface address and pitch (Apple's framebuffer, uniform grey), and `CRTC_PIXEL_DATA_READBACK` shows
  0xBF8 per channel: the CRTC outputs the right pixels. No XDMA or DMIF underflow (blender flag twice, at mode set).
  So the fault is after the CRTC, with DIG0/DP0 registers equal to the firmware's except four unnamed ones
  (0x4A49..0x4A4D). Apple's training writes DPCD 0x100/0x101/0x107/0x108 but never 0x10A (eDP ASSR), and a scrambler
  seed mismatch turns a uniform picture into identical rows of wrong colour. Next: record DPCD 0x00D / 0x10A before
  Apple's link setup and re-enable ASSR in the panel when Apple writes 0x108; restore the unnamed DIG0 registers at t=45.
- **Spoof boot 35 (ASSR re-enable + `-crrestore`):** **first correct picture**: the light-grey recovery background with
  the beach-ball cursor, which then stopped. The "freeze" at 75 s was the deliberate panic (`debug=0x100` halts, and
  the panic text is not drawn once the framebuffer owns the screen). The panel reports ASSR capable (DPCD 0x00D =
  0x0B) and 0x10A = 0x01 before Apple's first AUX transaction, so it loses ASSR during Apple's link setup; setting it
  again just before training fixes the picture. Interrupt handler calls: 0, so WindowServer's vblank waits never
  complete. Next: the black box records the whole interrupt path (CRTC source, DISP_INTERRUPT_STATUS, IH ring and
  pointers, BIF INTERRUPT_CNTL, MSI).
- **Spoof boot 36 (ASSR only, `crpanicat=60`):** white (light-grey) recovery background again, so the ASSR re-enable alone
  fixes the picture; `-crrestore` is not needed.
- **Spoof boot 37 (irq report):** white screen with the normal cursor, frozen. The IH works: ring enabled, 2316 entries
  written (WPTR 0x90C0, write-back agreeing, RPTR 0), MSI enabled (address 0xFEE10000, data 0x4072, INTx disabled).
  But `IH_CNTL` = 0x01080000: `ENABLE_INTR` off, so the IH never signals. Apple's `setHardwareEnabled` sets
  `ENABLE_INTR` in `IH_RB_CNTL` bit 17 (Tonga layout); CZ keeps it in `IH_CNTL` bit 0 and needs `RPTR_REARM` (bit 4)
  for MSI (Linux `cz_ih` vs `tonga_ih`). CarrizoRed now mirrors the enable into `IH_CNTL` (`-crnoihfix` disables).
- **Spoof boot 38 (IH_CNTL fix, `crpanicat=120`): reached the macOS Recovery setup screen** on the internal panel.
  115 interrupts handled, ring drained (RPTR = WPTR = 0x730), IH idle, `IH_CNTL` = 0x01080011. Apple masks CRTC0
  V_UPDATE again when idle (`CRTC_INTERRUPT_CONTROL` 0x8000020). The scanout memory holds the setup screen's dark
  gradient. The stop at 120 s was the deliberate panic.
- **XOSI back to Windows 10, with the xHCI kept out of D3cold (2026-10-12, untested):** the only USB-relevant
  difference between `OSVR` 0x0E and 0x0F is `FINI` (from `\_SB.PCI0._INI`) clearing `XHCD`/`EHCD`/`ST_D`/`SD_D`/`S03D`
  below 0x0F. `SSDT-XOSI` now claims Windows 2012/2013/2015 and adds `\_SB.PCI0.XHCF`, whose `_INI` (run after
  `PCI0._INI`) clears only `XHCD` under Darwin, so XHC0's `_S0W`/`_PS0`/`_PS3` act as under Windows 7. Windows 8+ also
  makes the AMD I2C (touchpad bus), GPIO and UART devices present and routes the brightness Fn keys as ACPI
  `Notify (VGA.LCD, 0x86/0x87)`. Previous (Windows 7) EFI kept as `efi-d509ba/usb-spoof-win7`.
- **Windows 10 mode tested:** boots from the USB stick and the USB mouse works, so clearing `XHCD` alone keeps the
  xHCI alive. The mouse still disconnects and reconnects now and then (seen in the verbose log). Touchpad: the EFI now
  injects VoodooI2C 2.9.1 (AMD `AMD0010` I2C and `AMD0030` GPIO support) with VoodooI2CHID for the ELAN
  HID-over-I2C touchpad (`\_SB.I2CD.ETPD`, address 0x15, GPIO interrupt).
- **`usb-spoof` (no deliberate panic) boots fully into Recovery** on Apple's Polaris display driver.
- **First `usb-spoof-accel` boot (nvram27): the accelerator never attached.** Only the display drivers were on the iGPU
  and no accelerator patch ran. The graphics policy now sets `LoadController` by itself, so CarrizoRed's request
  (which also set `LoadHWServices` / `LoadAccelerator`) was skipped. The acceleration request is now separate and made
  once `AMD9500Controller` is attached.
- **Second `usb-spoof-accel` boot (nvram28): still no accelerator,** and no trace of the request either. The black box
  now records which acceleration kexts load (`kext:X4000/HWServices/HWLibs`), the request (or that `LoadAccelerator`
  was already set), both `probe()` calls, and the iGPU's `Load*` properties.

### Consequences
- The CarrizoRed VFCT injector will match this image (vendor/device/slot/function `1002:98E4` `00:01.0`).
- The pixel-clock path Apple uses for DCE 11.0 VBIOSes is the one this VBIOS needs.
- **New item:** no `SetDCEClock` table, so check Apple's display-engine-clock path.
- **Phase 3 note:** BAR0 (256 MB) is smaller than the carve-out (512 MB). Apple's accelerator must not assume the
  whole "VRAM" is CPU-visible through BAR0 (Linux maps the carve-out directly at `MC_VM_FB_OFFSET << 22`).
- For macOS: no Wi-Fi (QCA9377), so use a USB Ethernet/Wi-Fi adapter or iPhone USB tethering if needed. Use a USB mouse
  (the I²C trackpad is unlikely to work on AMD without extra work).

### USB slowness: what's known (2026-10-12)
- Symptoms: the 2026-10-10 install copied ~130 KB/s from the stick; `dd` from the stick hung; the mouse sometimes
  disconnects and re-enumerates on XHC0; the full-installer stick sat in `validate_root_image` for minutes. The UEFI
  firmware itself took 112 s to load `BootKernelExtensions.kc` from that stick (boot.efi log), i.e. it is slow before
  macOS runs.
- Linux on this laptop (Ubuntu dump): XHC0 `1022:7914` rev 0x20, xHCI 1.0, **MSI-X** enabled (MSI cap 1/8 unused), no
  device-specific quirk (`quirks 0x10` = the generic spurious-success one); the AMD PLL fix only covers Hudson-2/Bolton.
  Linux enables USB 3 LPM (U1/U2) only on hosts it trusts (mostly Intel), so it never uses it here; macOS does. A
  long-running Red Hat bug (1196943) has the older AMD APU FCHs (Hudson/Bolton) dying or failing enumeration with USB
  autosuspend on, fine with it off. EHCI (`1022:7908`, the USB 2.0 ports) shares IRQ 18 but has its own MSI-less path.
- Hypotheses, each with a test: (1) the stick/port itself (firmware is slow too): read speed of the same stick on
  another machine; (2) xHCI link power management / suspend: compare a USB 2.0 port (EHCI, no U1/U2); (3) lost
  interrupts (driver falls back to timeouts): `usb-spoof-xhciprobe` (XHCIProbe 0.3 with `-xhciprobe_noact`) prints
  `XHCIProbe: irq … msi msix … IP x/y EINT x/y events n` every 2 s.
