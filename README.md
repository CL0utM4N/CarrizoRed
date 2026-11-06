# CarrizoRed

A [Lilu](https://github.com/acidanthera/Lilu) plugin that brings macOS graphics to AMD's GCN 3 (GFX8) APUs by adapting
Apple's own Polaris drivers (`AMD9500Controller` + `AMDSupport` for display, `AMDRadeonX4000` for acceleration). It's
the same idea NootedRed applies to Vega iGPUs on top of `AMDRadeonX5000`. macOS has never supported these APUs (no Mac
used them), so on a hackintosh they've only had the firmware framebuffer.

## Status

> **Experimental, in active development.** The first pre-release, **0.1.0**, is on the
> [Releases](https://github.com/CL0utM4N/CarrizoRed/releases) page. Expect kernel panics and black screens, and
> don't run it on a machine you depend on. Everything below is from a handful of test machines.

**What it does today:** on Stoney Ridge machines, macOS Catalina through Sequoia boots to the desktop and drives
the built-in panel through Apple's own Polaris display driver, instead of the firmware's fixed framebuffer. **Metal
acceleration works and the desktop renders correctly:** WindowServer draws the login screen and desktop through the
GPU, apps use Metal, and vibrancy, text, icons (folder icons included) and the Dock draw properly. The GPU clock
scales with load (400 MHz idle, up to the chip's top level).

| | Status |
|---|---|
| Built-in eDP panel | **Works** through Apple's `AMD9500Controller` / `AMDFramebuffer`: Recovery, the installer and installed Catalina through Sequoia (the D509BA's 1366×768 panel on one eDP lane, the Inspiron 3475's on two) |
| Display interrupts (vblank) | **Works** |
| Acceleration / Metal (`AMDRadeonX4000`) | **Works.** The SMU loads the GPU microcode, the command processor and SDMA run, interrupts work, and WindowServer and apps render with Metal. Apple's shader compiler emits an interpolation pattern Stoney's 16-bank LDS gets wrong; CarrizoRed fixes the compiled pixel shaders before the GPU runs them, including when Metal re-uploads them (see *Metal needed*). Boot with `-crnoaccel` for an unaccelerated desktop |
| GPU clock scaling (SCLK DPM) | **Works, on by default.** Apple's PowerPlay runs its Dummy back end here, so nothing would start the SMU's clock scaling and the GPU would stay at its lowest clock. CarrizoRed starts it 90 s after boot, from the SMU's own clock table: 400 MHz minimum, up to the top level (900 MHz on an A9-9425, 800 MHz on an A9-9420). `-crnosclkdpm` turns it off |
| Sleep | **Works** on the D509BA (Catalina, Big Sur, Monterey) and the Dell Inspiron 3475 (Tahoe): real S3, no wakes of its own, and a USB keyboard on a USB 2 or USB 3 port wakes it (both machines). CarrizoRed flushes the CPU caches before S3 (Apple's ACPI code doesn't, and the wake died without it), lets only the USB controllers, the lid and the power button wake the machine, and works around the AMD FCH's USB wake quirks; FCHXHCIFix's `SSDT-FCHSLEEP` keeps the USB 2 controller powered through S3. It redoes what S3 resets in the display block the way Linux's DC driver does at resume (panel power, display clock and DP DTO, VBIOS scratch and backlight, AUX, ASSR). Sonoma untested since the cache flush. Lid close sleeps on battery but not on AC (macOS sees desktop mode). Details: [docs/SLEEP.md](docs/SLEEP.md) |
| Brightness | **Works:** the Displays slider and the brightness keys (D509BA on Monterey, Dell Inspiron 3475 on Tahoe). Needs `SSDT-PNLF` and [BrightnessKeys](https://github.com/acidanthera/BrightnessKeys) (see *Installing*). With them the panel is macOS's built-in display, which also makes lid-close sleep work on AC |
| External displays (HDMI) | **Works** on the D509BA and the Dell Inspiron 3475 (Tahoe): the monitor is detected and lit at boot and on hot-plug (1920×1080 at 60 Hz tested), and unplugging it hands the desktop back to the panel. See *HDMI* below. DisplayPort outputs untested |
| HDMI audio | Not done |
| Video decode/encode (UVD/VCE) | **On hold**: video plays and records in software. UVD powers up and Apple's firmware boots on it, but decoding hangs the GPU: Apple's driver gives UVD GPU virtual addresses, and Stoney's UVD has no VM support (Linux runs it in physical mode and rewrites every buffer address). Making it work means doing that translation in CarrizoRed. Findings and next steps in [docs/VIDEO.md](docs/VIDEO.md) |
| Carrizo / Bristol Ridge | Same display engine and code paths; the SMU microcode loader, clock scaling and shader-engine fix include Carrizo's differences (second SDMA engine, second MEC), following Linux. **Untested** (no hardware): reports from owners are very welcome |

**Tested on:**

| Machine | APU | macOS | Result |
|---|---|---|---|
| ASUS VivoBook 15 D509BA | A9-9425 (Stoney Ridge, Radeon R5) | Catalina 10.15.7 – Sequoia 15.8.1 (installed); Sequoia 15.4 Recovery and installer | Display and Metal acceleration work, desktop renders correctly |
| Dell Inspiron 3475 (all-in-one) | A6-9225 (Stoney Ridge, Radeon R4) | Ventura 13.7.8; Tahoe 26.7.2 | Ventura: display and Metal acceleration work, desktop renders correctly. Tahoe: works, with glitches (see *macOS versions*) |
| Acer Aspire E5-523 ([Lost-Entrepreneur439](https://www.reddit.com/r/hackintosh/comments/1xh74lv/ive_done_a_lot_of_hackintoshes_but_this_might_be/)) | E2-9010 (Stoney Ridge, Radeon R2) | Ventura 13.7.8 | Display and Metal acceleration work |
| HP 17-ak0xx ([u/macos12345](https://www.reddit.com/r/hackintosh/comments/1xhaqvw/finally_macos_is_possible_on_pre_ryzen_apus/)) | A6-9220 (Stoney Ridge, Radeon R4) | Ventura | Display and Metal acceleration work |
| ASUS X555B ([u/Azusawaga](https://www.reddit.com/r/hackintosh/comments/1xb8mct/what_used_to_be_impossible_today_is_the_reality/)) | A9-9410 (Stoney Ridge, Radeon R5) | Sequoia 15.8.1 | Display and Metal acceleration work |

**macOS versions:**

| macOS | Support |
|---|---|
| Sequoia 15 | **Works.** Development target; every hook found by symbol |
| Sonoma 14 | **Works.** Its AMD init library (HWLibs) ships stripped, so CarrizoRed finds the 46 functions it hooks by byte signatures checked against the 14.8.9 KDK ([`kern_sigs.hpp`](CarrizoRed/kern_sigs.hpp)); Metal patches at Sonoma's offsets |
| Ventura 13 | **Works.** HWLibs is stripped and compiled differently: functions matched by the constants they use (45 of 46), safety checks accept Ventura's register allocation, Metal patches at Ventura's offsets |
| Monterey 12 | **Works** (12.7.6). HWLibs functions matched by their constants (41 of 46) from the installed system's kexts (no Monterey KDK exists), the hardware capability fields at Monterey's offsets, safety checks accept its register allocation, Metal patches at its offsets. Needs the xHCI reset patch's `Limit` at 2048 for USB 3 (see FCHXHCIFix) |
| Big Sur 11 | **Works** (11.7.11). Its HWLibs keeps its symbols, so every hook is found by name; the accelerator and command-buffer layouts differ (capability fields as Monterey, submission info, a vtable slot) and the Metal patches sit at Big Sur's offsets |
| Catalina 10.15 | **Works** (10.15.7). HWLibs keeps its symbols (Polaris loads `AMDRadeonX4000HWLibs` unless the subsystem is Apple's); the accelerator's engine slots, capability fields and command-buffer layout, and CAIL's flags and SMU buffer fields, sit at Catalina's offsets. Its scheduler has one queue per engine and priority, so CarrizoRed creates three extra queues instead of four (the second SDMA engine is folded into the first). Metal: the shader compiler (`libSC.dylib` in AMDShared.bundle) and the Metal driver are separate files there, patched through `cs_validate_range`. The xHCI reset patch matches (`MinKernel` 19.0.0) |
| Tahoe 26 | **Works, with graphical glitches; not recommended yet** (26.7.2 on the Dell Inspiron 3475). The Metal patches match Tahoe's cache and the shader fix handles its shaders, but Tahoe needs more graphics memory than these APUs' usual 512 MB: Apple's driver logs `AMD ERROR! Failed to allocate size:… There is … free memory remaining` (up to 1,749 times in one session) when the carve-out is full or too fragmented, and desktop widgets draw broken while an app is open. Unless the firmware gives the GPU at least 1 GB, stay on Sequoia or earlier for now; if you run Tahoe anyway, expect more glitches. See *Graphics memory on Tahoe* |

Each version's Metal shader-compiler patches are matched against that version's own dyld shared cache.

**Graphics memory on Tahoe.** The "VRAM" of these APUs is a block of system RAM the firmware reserves at boot (UMA
frame buffer, usually 512 MB, sometimes settable in the BIOS). Apple's driver sizes its video-memory heaps from that
block (`AMDVIHWMemory::getHWMemorySize` reads `CONFIG_MEMSIZE`), and the failures come from
`AMDHWMemory::allocateLargeBlocks` when no free piece is large enough. The "fixed-free" figure in the message is a second
heap over the same range, not spare memory. The block can't be grown or moved after boot: the display engine scans out
of it and the CPU reaches it through a fixed aperture. The way forward is to have Apple's driver place more of its
allocations in ordinary system memory, which on an APU is the same RAM at the same speed; that is not done yet.

Reports from other Carrizo, Bristol Ridge or Stoney machines are welcome once it's further along; open an issue with
the model and the black-box report (see *Installing*).

**USB on these machines:** USB 3 is unusably slow under macOS on all of these APUs, independent of graphics. Use
[FCHXHCIFix](https://github.com/CL0utM4N/FCHXHCIFix), released separately.

The display needed fixes for the differences between DCE 11.0 (these APUs) and DCE 11.2 (Polaris):
- 2–3 display pipes instead of 6;
- the APU's PLLs and DIGs;
- the line-buffer size;
- the eDP link (one lane, 6 bpc, panel power-on, external reference clock, and re-enabling ASSR in the panel);
- the interrupt handler's enable bit, which sits in a different register on Carrizo/Stoney than on Polaris.

The accelerator so far needed:
- one sDMA engine instead of two, and one shader engine;
- PowerPlay's Dummy back end, with the VBIOS PowerPlay table;
- **an SMU microcode loader** (a port of Linux's `smu8_smumgr`): on these APUs only the SMU can load the RLC, CP and
  SDMA microcode, and Apple's PowerPlay has no SMU8 code. It's reloaded after Apple's GPU hang recovery too;
- the APU's VRAM window moved to GPU address 0, with APU page-table addresses;
- the interrupt ring moved into VRAM (the display driver addressed it through a range the VRAM move covers);
- the hardware description given to the Metal driver: the spoofed device ID instead of Stoney's real one;
- graphics power gating kept off (nothing drives the SMU's save/restore jobs under the Dummy back end, and the GPU
  hung on its next command after powering down);
- lost display-flip completions delivered by CarrizoRed (otherwise one lost completion freezes the screen);
- the video engines left off unless `-crvideo` powers them through the SMU.

And Metal needed:
- the compiled pixel shaders fixed for Stoney's 16-bank LDS: `v_interp_p1_f32` must not write the VGPR it reads, and
  Apple's shader compiler (even its own Stoney back end, after clearing the shader caches: about 47% of shaders) does
  that for the last component of an attribute. CarrizoRed rewrites those shaders in GPU memory as each command buffer
  is submitted, before the GPU runs them. When the broken result is part of a register block (an image coordinate),
  the whole block moves to fresh registers. Shaders up to 512 KB are handled (Sonoma's lock screen shader is 144 KB);
- the fixes kept from being undone: Metal's driver re-uploads its shader heap with SDMA copies from a staging copy
  holding the original code, which could land after the fix and make some frames draw unfixed (the random flat-colour
  images, like the old blue folder icons). CarrizoRed reads the SDMA command buffers at submission and applies the same
  fix to each copy's source, so the GPU copies in fixed code;
- the small-primitive filter off, the 2-pipe walk fence and no delta colour compression (Polaris settings Mesa doesn't
  use on Stoney), and the Metal compiler switched to its Stoney back end (patched in the dyld shared cache);
- the GPU and CPU named after the APU in About This Mac.

## Development status

| Area | State |
|---|---|
| Display (built-in eDP panel) | Works |
| Metal acceleration | Works on Catalina through Sequoia; the desktop renders correctly |
| Stoney's 16-bank LDS shader fix | Works: every hazardous shader fixed in recent sessions (none skipped), fixes no longer undone by Metal's re-uploads |
| Random glitches (flat-colour images, login avatar, blue folder icons) | Fixed: Metal's SDMA shader re-uploads were undoing the shader fix. Folder icons drawn wrong by older builds stay in macOS's icon cache until it is cleared |
| Lock screen (Sonoma) | Renders; the animated wallpaper is slow (10-bit 4K HEVC on the CPU; Stoney's UVD can't decode 10-bit). A still wallpaper avoids it |
| GPU clock scaling | Works, on by default, up to the chip's top level. It stays at the top level more than it should; being looked at |
| Freezes with clock scaling | Fixed: CarrizoRed's own interrupt-storm breaker was tripping on real interrupt bursts at higher clocks |
| OpenGL | Expected to work, untested: Apple's AMD GL driver compiles through the same `libSC` CarrizoRed patches for Stoney (its `ATIRadeonX4000SCLib` wraps it), and the shader fix works on every submission whatever the API |
| Video decode/encode | On hold: software decode and encode. UVD starts with Apple's firmware, but needs CarrizoRed to translate GPU virtual addresses to physical ones before it can decode ([docs/VIDEO.md](docs/VIDEO.md)) |
| Sleep/wake | Done: works on the D509BA and the Dell (Tahoe), with USB 2 and USB 3 keyboard wake. Sonoma to retest; lid sleep on AC blocked by desktop mode (works on battery); see [docs/SLEEP.md](docs/SLEEP.md) |
| Carrizo / Bristol Ridge | Code paths written (SMU loader, clock scaling, caps), untested: no hardware |
| Tahoe graphics memory | Open: Tahoe can fill the 512 MB carve-out (see *Graphics memory on Tahoe*) |
| Brightness | Done: slider and keys, through Apple's AppleBacklight and AMD framebuffer (see *Brightness* under *Boot arguments*) |
| External displays (HDMI) | Done: the D509BA and the Dell (Tahoe), at boot and on hot-plug |
| HDMI audio | Next |
| GPU occasionally not responding after a warm restart | Open (power off fully) |

**Testing:** every boot writes `/private/var/log/CarrizoRed-<time>.log`: GPU, SMU and clock state every second, all SMU
messages, a census of draw set-ups and the black-box report, written straight to disk so it survives a freeze. Copy
these files off after a few boots and attach them to an issue; no NVRAM dumps or forced panics needed.

HWLibs patches not taking at boot turned out to be Lilu's trampoline buffer running out; CarrizoRed builds its own
trampolines.

Each fix is documented in the code, in [docs/TEST_MACHINES.md](docs/TEST_MACHINES.md) and in
[docs/APPLE_KEXTS.md](docs/APPLE_KEXTS.md).

## Targets

| APU family | Examples | PCI ID | GFX | Display | Status |
|---|---|---|---|---|---|
| Stoney Ridge | A9-9410/9420/9425, A6-9220/9225, A4-9120, E2-9000/9010 | 1002:98E4 | gfx810 | DCE 11.0, 2 pipes | Display and Metal working (A9-9410, A9-9425, A6-9220, A6-9225, E2-9010) |
| Carrizo / Bristol Ridge | A12-9800, A10-9700, FX-9800P, A10-8700P, FX-8800P | 1002:9870, 9874–9877 | gfx801 | DCE 11.0, 3 pipes | Untested |

Older GCN 2 APUs (Kaveri, Kabini, Beema/Mullins) are planned, in this kext and the same way: Apple's Polaris drivers
adapted with Lilu, without OpenCore Legacy Patcher or changes to the system. Every macOS release since Monterey still
has AMD's init code and firmware for these chips and a GFX7 shader compiler. Most of the display and accelerator
registers are at the same offsets as on Polaris. Nothing is implemented yet; the first step is a test machine that
boots macOS. See [docs/OLDER_APUS.md](docs/OLDER_APUS.md).

**macOS:** Catalina through Sequoia work (see *macOS versions* above); Tahoe works with glitches and isn't recommended yet
with 512 MB of graphics memory.
Excavator CPUs have AVX2, so Ventura and later run without CPU hacks (AMD_Vanilla patches are still needed).

**Why this family:** macOS never had drivers for these APUs, and removed its GCN 1–3 dGPU drivers in Ventura, but it
still ships Polaris (GFX8, DCE 11.2). Carrizo-class APUs are the closest to Polaris: the same GFX8 family (GCN 3 vs
Polaris's GCN 4, with nearly the same instruction set) and a close relative of its display engine (DCE 11.0 vs 11.2). Apple's still-shipping kexts are the base, and Apple's copy of AMD's CAIL init
library contains Carrizo and Stoney support (it's shared AMD code).

## Building

Requires macOS with the Xcode command-line tools:

```sh
bash scripts/bootstrap.sh   # fetches Lilu.kext (SDK) and MacKernelSDK
make DEBUG=1                # build/Debug/CarrizoRed.kext
make                        # build/Release/CarrizoRed.kext
```

No Mac? Push to GitHub: `.github/workflows/build.yml` builds CarrizoRed (Debug and Release) and the
XHCIProbe diagnostic on a macOS runner, and uploads them with a debug `Lilu.kext` as one workflow artifact.
Development for this project is done entirely on Windows this way.

## Installing (test machine)

Download `CarrizoRed-<version>-Release.zip` from [Releases](https://github.com/CL0utM4N/CarrizoRed/releases) (or a CI
build, see *Building*). It needs [Lilu](https://github.com/acidanthera/Lilu/releases) 1.7.2 or later (it is built against Lilu's current SDK).

`scripts/build_efi_d509ba.py` builds the complete OpenCore EFI used for the D509BA. It's a good starting point for
similar machines. It includes:
- SMBIOS iMac19,1;
- AMD_Vanilla;
- the ACPI fixes (`SSDT-XOSI`, EC/USBX) and a USB map;
- VoodooPS2 and VoodooI2C (touchpad);
- [FCHXHCIFix](https://github.com/CL0utM4N/FCHXHCIFix) (USB, see below);
- CarrizoRed and the iGPU spoof below, when the script is run with `--carrizored --spoof` (options of the script,
  not boot-args).

The pieces CarrizoRed needs:
1. **Kexts:** add `Lilu.kext`, then `CarrizoRed.kext`, to `EFI/OC/Kexts` and to `config.plist` (Lilu first).
2. **Device ID spoof:** a `DeviceProperties` entry on the iGPU setting `device-id` to `EF670000` (Polaris 11 / Baffin,
   `0x67EF`). That's what makes Apple's Polaris kexts match. CAIL still reads the real ID from the hardware. The iGPU is
   always PCI device 1, function 0, but the `PciRoot` number differs between machines: `PciRoot(0x0)/Pci(0x1,0x0)` on
   the D509BA, `PciRoot(0x1)/Pci(0x1,0x0)` on a Dell Inspiron 3475. Check yours (see *Finding the iGPU's path* below).
   With the wrong path OpenCore silently skips the entry, and nothing from CarrizoRed's acceleration ever loads.
3. **Boot-args:** `-v keepsyms=1 debug=0x100` for testing, plus `-crnoaccel` to bring up the display without the
   accelerator.
4. **When something goes wrong:** with `crpanicat=N`, CarrizoRed panics on purpose N seconds after the display driver
   loads. The panic text carries a "black box" report: trace, registers, link, interrupts and accelerator state.
   `debug=0x100` makes the machine halt on the panic instead of rebooting. Read the report back from NVRAM
   (`AAPL,PanicInfo`) after the next boot, e.g. with OpenShell `dmpstore -all -s fs0:\nvram.bin`.
5. **Brightness (laptops and all-in-ones):** add `SSDT-PNLF.aml` (compiled from
   [docs/opencore/SSDT-PNLF.dsl](docs/opencore/SSDT-PNLF.dsl): `iasl SSDT-PNLF.dsl`) to `EFI/OC/ACPI` and `config.plist` →
   ACPI → Add, and [BrightnessKeys](https://github.com/acidanthera/BrightnessKeys/releases) after Lilu in Kernel → Add.
   The SSDT is the `"backlight"` device macOS looks for before it treats a panel as built-in; without it the panel is an
   external-style display: no brightness control, and closing the lid doesn't sleep the machine on AC.
6. **After boot:** `log show --last boot --predicate 'eventMessage CONTAINS "CarrizoRed"'` and
   `ioreg -lw0 | grep CarrizoRed`.

### Finding the iGPU's path

OpenCore names a PCI device by its UEFI device path. Its first part, `PciRoot(N)`, is the `_UID` of the PCI root
bridge in the firmware's ACPI tables, not a bus number. AMD's reference firmware uses `_UID` 0 on some machines and 1
on others. The rest, `Pci(0x1,0x0)`, is the iGPU at device 1, function 0 on every one of these APUs.

The usual OpenCore way, [gfxutil](https://github.com/acidanthera/gfxutil), works here too. It reads the same `_UID`,
so the path it prints is the one OpenCore matches. In macOS (it doesn't need acceleration):

```
gfxutil -f VGA
```

The iGPU's ACPI name is usually `VGA` on these machines (`GFX0` on some). If neither matches, `gfxutil` alone lists
every device; look for the line with `00:01.0` and `1002:98e4` (Stoney) or `1002:9874` (Carrizo / Bristol Ridge).

Before macOS is installed:
- **Linux live USB:** `cat /sys/bus/acpi/devices/PNP0A08:00/uid` gives the `PciRoot` number (`0` or `1`).
  `lspci -nn -s 00:01.0` should show the GPU.
- **ACPI dump** (OpenCore's `SysReport` quirk, or `sudo acpidump -b` on Linux, then `iasl -d DSDT.aml`): in `DSDT.dsl`,
  find `Device (PCI0)` and its `Name (_UID, ...)`: `Zero` means `PciRoot(0x0)`, `One` means `PciRoot(0x1)`.

To check that the spoof took effect, run `ioreg -lw0 -r -c IOPCIDevice | grep '"device-id" = <ef670000>'` in
macOS. No output means OpenCore didn't apply the entry. Another sign is "Graphics 7 MB" in About This Mac with no
`/private/var/log/CarrizoRed-*.log`.

## USB on these machines

The APUs' built-in USB 3 controller (FCH xHCI `1022:7914`) never delivers its interrupts to macOS, so USB crawls
(a USB stick reads at ~130 KB/s, and installing macOS from one takes most of a day). The standalone
[FCHXHCIFix](https://github.com/CL0utM4N/FCHXHCIFix) kext fixes it (its own repository, with releases). It doesn't
depend on CarrizoRed or Lilu, so put it in the EFI **before** installing macOS. Its README has the cause, the diagnosis
and the list of affected machines.

For sleep, also add FCHXHCIFix's `SSDT-FCHSLEEP` (with its `_PTS` rename): AMD's reference firmware powers the USB 2
controller off in `_PTS`, which wakes the machine at once. CarrizoRed does the rest at S3 entry (see *Sleep* in
[docs/SLEEP.md](docs/SLEEP.md)).

## Boot arguments

### Fixes (on by default; the flag turns one off)

| Argument | Turns off |
|---|---|
| `-crnovbios` | Injecting the VBIOS from the ACPI VFCT table as `ATY,bin_image` |
| `-crnopipes` | Limiting `AMD9500Controller` to the APU's 2–3 display pipes and line buffers |
| `-crnodiglimit` | Limiting it to 3 DIGs |
| `-crnoplllimit`, `-crnopllremap` | Limiting the PLL assigner to 2 PLLs / remapping Apple PLL 1 to `ATOM_PPLL0` |
| `-crnofblimit` | Limiting the framebuffer count to the display pipes (`CFG_FB_LIMIT`) |
| `-crnomcloc` | Forcing `CFG_NON_ZERO_MC_LOC` (VRAM base from `AtiAsicInfo::getFbLocationBase()`) |
| `-crnovramtest` | Reporting `ATIController::TestVRAM` as passed (the real result is still logged) |
| `-crnolbfix` | DCE 11.0's line-buffer size (0x6B0) in `LB_MEMORY_CTRL` instead of Apple's |
| `-crnoedppoweron` | The eDP panel POWER_ON transmitter action before Apple enables the eDP transmitter, and before the first AUX transaction after a wake |
| `-crnoassrsource` | When CarrizoRed enables ASSR in the eDP panel, don't also put the GPU's DP encoder in eDP panel mode (DP_DPHY_INTERNAL_CTRL = 1, as Linux DC does). The firmware sets it at boot; after a sleep it's 0 and the picture comes out as stripes |
| `-crnodispclk` | After a wake, don't set the display engine clock the way Linux's DC does on DCE 11.0 (VBIOS SetPixelClock with the DFS PLL and the VBIOS's default display clock; Apple's SetDCEClock path doesn't exist in these VBIOSes), so DPREFCLK and the DP DTO stay at their reset values |
| `-crnodppclk` | Don't send Apple's DP/eDP SetPixelClock calls to the VBIOS the way Linux's DC does (ucPpll = ATOM_PPLL_INVALID, no dividers, so the VBIOS programs the DP DTO); Apple passes its Polaris "no PLL" index 6, and after a sleep nothing set the DTO up, so a trained link stayed black |
| `-crnowakerestore` | Don't run Linux DC's resume steps after a wake: VBIOS EnableDispPowerGating INIT/DISABLE on both pipes, the VBIOS scratch and backlight PWM registers from before sleep, the eDP transmitter INIT with DC's parameters, the AUX engine reset |
| `-crwakeregrestore` | Experimental: after a wake, also copy the firmware's boot-time display registers back (display clocks and time bases, PLL bandgap, AUX pad calibration and PHY set-up, panel power pads, UNIPHY0 macro). Not what Linux does |
| `-crnoauxprobe` | Skip the diagnostic native AUX read of DPCD 0 (Linux DC's register sequence, bypassing the VBIOS AUX table) at boot and after a wake |
| `-crnosleeppause` | Keep reading GPU registers from CarrizoRed's watcher (disk log status, clock sampling, display checks, SMU mailbox) while the GPU sleeps; by default they pause from "will sleep" until the GPU is powered again, since a register read once Apple has powered the GPU down can stall the CPU |
| `-crnosleeppoweroff` | Don't power the eDP panel off before sleep (VBIOS POWER_OFF with the panel's parameters, right after Apple's DPCD 0x600 = 2), as Linux's DC does at suspend |
| `-crnowakefilter` | Don't cut the armed wake sources down at S3 entry. By default only the USB controllers, the lid and an ACPI power button (each by its `_PRW` GPE) stay armed, and PCIe WAKE# is off: macOS arms every `_PRW` device, and a PCIe port signalling as the machine goes down brought it straight back |
| `-crnowakeremap` | Leave the FCH's USB wake lines alone: by default the wake lines of FCH USB controllers that don't exist (SciMap sources 25 and 26 on Stoney) are moved off the shared USB GPE at S3 entry, since one of them fired as the machine went down |
| `-crnoehcid2` | Let the FCH EHCI (`1022:7908`) sleep in D3: by default it sleeps in D2, since AMD's EHCI doesn't signal PME properly from D3 (Linux works around the same erratum) |
| `-crnobacklight` | Brightness: don't complete AppleBacklight's panel set-up. On Monterey and later its `AppleIntelPanel::setDisplay` matches the panel to the `Default` entry, then fails storing the match and returns false, so the panel gets no `brightness` parameter (no slider, no keys); CarrizoRed stores it and reports success. Also stops it giving the panel driver a `Default` entry when it has none |
| `-crnoblupdate` | Brightness: don't latch the backlight PWM after Apple's VBIOS brightness call. DCE 11 double-buffers `BL_PWM_CNTL` behind `BL_PWM_GRP1_REG_LOCK`; the VBIOS changes the duty but nothing applies it, so the screen kept its brightness. CarrizoRed locks, rewrites, unlocks and waits, as Linux DC's `driver_set_backlight` does |
| `-crnodevinfo` | Don't record the iGPU as the built-in GPU in Lilu's shared `DeviceInfo`. Lilu recognises an AMD iGPU by its `device-id` property, which the Polaris spoof changes, so plugins looking for the built-in panel through it (BrightnessKeys: the brightness keys' ACPI notifications) found nothing |
| `-crnomacgpu` | HDMI: leave AGDP's board-id lookup and the iGPU's properties alone. By default AGDP's `board-id` is read as `board-ix`, so it uses its `Default` policy instead of the SMBIOS model's (which kept the external port's framebuffer from getting a display), and the iGPU gets `built-in` and `AAPL,slot-name` like a Mac's own GPU. The same idea as WhateverGreen's `agdpmod=pikera` |
| `-crnoddcfix` | HDMI: don't add the hardware I2C engine bit to DDC connectors' sense ids. Apple's `AtiBiosParser1` (ATOM object table rev 1.x, as on these APUs) reports the DDC line as line + 1 without bit `0x10`, and `BaffinHwI2cProxy` only uses the hardware I2C engine for `0x10 \| line`, so the EDID read never ran |
| `-crnoddcprep` | HDMI: don't set the DDC line up for the hardware I2C engine before Apple's EDID read (`DC_I2C_DDCn_SPEED`/`SETUP` and the `DC_GPIO_DDCn` mask, as Linux DC does); without it the read timed out |
| `-crnotmdsfix` | HDMI: leave Apple's TMDS (HDMI/DVI) transmitter and pixel clock calls as they are. CarrizoRed builds them as Linux DC does: the PHY clocked from the PLL the pixel clock actually went to (CarrizoRed remaps Apple's P2PLL to PPLL0, which DCE 11.0 uses), the HDMI connector object, not coherent, and the stream's encoder mode in SetPixelClock. Calls for the eDP panel aren't touched |
| `-crnobootdisp` | HDMI: don't run the display set-up Linux DC's `init_hw` does at boot and before each display enable: the VBIOS EnableDispPowerGating INIT/DISABLE on both pipes and the display engine clock (FirmwareInfo's default, 847 MHz on the A9-9425, through SetPixelClock's DFS). Apple sets that clock through SetDCEClock, which these VBIOSes don't have, so it stayed at the 100 MHz the firmware left: enough for the panel, not for 1080p, and the HDMI monitor stayed black |
| `-crnohpdfix` | HDMI: don't correct a hot-plug pin left waiting for the wrong edge. Each HPD pin interrupts only on connect or only on disconnect (`DC_HPD_INT_POLARITY`), and the driver has to flip it after each event, as Linux does; Apple's sometimes didn't after an unplug, so the next plug-in went unnoticed (Dell 3475). CarrizoRed checks the pins every 0.5 s and flips one that has been waiting for the state it's already in for 1 s |
| `-crnoehciwakefix` | At S3 entry, don't clear the FCH USB 2 hub's wake on connect, disconnect and over-current (EHCI config 0x84 bits 23:21; wake on a device's remote wake stays) or a stale EHCI PME_Status. With them set, the machine woke ~1 s after every S3 entry |
| `-crtps1restore` | Experimental: after a wake, copy LVTMA_PWRSEQ_CNTL, the UNIPHY link controls, TMDP_REG3/5 and DIG_FE_CNTL from the boot's link training (TPS1) back just before Apple's training. A boot-time copy, not what Linux does |
| `-crtmdprestore` | Experimental: after a wake, copy the UNIPHY macro registers' boot values back right after Apple's eDP transmitter ENABLE. A boot-time copy, not what Linux does |
| `-crnowakepowercycle` | After a wake, don't power-cycle the eDP panel the way Linux's DC does (VBIOS POWER_OFF and POWER_ON with the panel's transmitter parameters, 500 ms apart, then wait for HPD); use the all-zero POWER_ON instead |
| `-crnodcaux` | After a wake, don't prepare each VBIOS AUX transfer the way Linux's DC does (clear the AUX pads' calibration errors, force a recalibration, acknowledge the last transfer; once: receive window 1, LS reads off) |
| `-crauxhpd` | Experimental: put the HPD_ID learned from working AUX calls into byte 8 of Apple's VBIOS AUX parameters (put back after each call). Off by default: that byte is Apple's own DPCD address, and AUX works at boot whatever it holds |
| `-crnodpextclk` | Switching the DP PHY to the external reference clock (`DIG1TransmitterControl`) |
| `-crnolanecap` | Capping DP training at the lane count the firmware used for the eDP link |
| `-crnobpccap` | Capping the DP colour depth at the firmware's (encoder `ucBitPerColor`, `DP_PIXEL_FORMAT`) |
| `-crnoassr` | Re-enabling eDP ASSR in the panel (DPCD 0x10A) when Apple sets up the link |
| `-crnoihfix` | Mirroring Apple's IH interrupt enable (Tonga `IH_RB_CNTL.ENABLE_INTR`) into Carrizo/Stoney's `IH_CNTL` |
| `-crnoloadctl` | Setting `LoadAMDSupport` / `LoadController` on the iGPU when the graphics policy hasn't (needed for the controller to match) |
| `-crnoaccel` | The acceleration stack (AMDRadeonX4000, HWServices/CAIL); otherwise CarrizoRed requests it once the display is up |
| `-crnosdmafix` | Stoney: releasing Apple's second sDMA engine and starting sDMA0 alone |
| `-crnoppdummy` | Stoney/Carrizo: don't set `PP_PhmUseDummyBackEnd` (PowerPlay has no back end for these APUs; without the Dummy one, `initializePowerPlay` fails and the accelerator never powers up); also turns PowerPlay off |
| `-crnosoftpptable` | Stoney/Carrizo: don't hand PowerPlay the VBIOS's PowerPlayInfo table as `PP_PhmSoftPowerPlayTable` (HWLibs' own VBIOS lookup appears to fail on the APU, and PowerPlay then fails to initialise) |
| `-crnodirectucode` | Stoney/Carrizo: leave CAIL's `DisableCailLoadUcode` on (the default). CarrizoRed clears it so CAIL reserves its micro-engine VRAM block, which the SMU microcode loader extends and uses; with the flag on, nothing loads the microcode and the accelerator's power-up waits forever |
| `-crvideo` | Video (on hold, for development): have the SMU power UVD and VCE up with CAIL's UVD power gating off, route AppleGVA to the AMD decoder, and refuse UVD and VCE sessions, so video stays in software |
| `-cruvd` | With `-crvideo`: let UVD decode sessions through. **Hangs the GPU** until address translation exists (see docs/VIDEO.md) |
| `-cruvdhevc` | With `-crvideo`: keep HEVC decode published to AppleGVA (withdrawn by default; Stoney can't decode the 10-bit aerial wallpapers) |
| `-crvce` | With `-crvideo`: let VCE encode sessions through (hung in testing; off by default) |
| `-cruvdpanic` | With `-crvideo -cruvd`: panic with the black-box report 20 s after the first UVD session if its firmware hasn't reported |
| `-crnogva` | With `-crvideo`: leave AppleGVA's decoder choice alone (no `hwgva-id` board-id redirect) |
| `-crnosrcfix` | Don't apply the shader fix to the source of Metal's SDMA re-uploads (fixed shaders can then revert for some frames) |
| `-crnosdmawatch` | Don't read SDMA submissions at all (also turns the source fix off) |
| `-crnovmmcheck` | Leave Apple's `AMDHWVMM::endVMPTUpdate` in place (CarrizoRed's copy panics with the black-box report when the GPU's page-table updater has no SDMA channel, instead of an Apple page fault without one) |
| `-crmm` | Also power up the accelerator's UVD, VCE and SAMU engines. On the APU those blocks stay power-gated without a real PowerPlay back end, and programming them can hang the bus, so by default their power-up is skipped (the engines themselves stay) |
| `-crnosmuload` | Stoney: don't have the SMU load the RLC/CP/SDMA microcode (CarrizoRed builds the SMU's task table in VRAM from the images in HWLibs, as Linux `smu8_smumgr` does); direct register loading alone doesn't work on these APUs |
| `-crnoihmove` | With the accelerator: leave the interrupt ring in system memory, where the GPU can no longer reach it once VRAM is moved to GPU address 0 (an interrupt storm, and the accelerator's work never completes) |
| `-crnohwinfofix` | Don't fill the zero clocks (no memory clock on an APU, none from the Dummy PowerPlay back end) in the hardware info the Metal driver asks for; without them every Metal device creation fails |
| `-crnohwinfospoof` | Let the Metal driver's hardware info carry the real device ID (read from config space) instead of the spoofed Polaris one |
| `-crnosmureload` | Don't have the SMU reload the CP microcode when Apple's GPU hang recovery re-initialises the graphics engine (the reset wipes it, and direct loading doesn't work on Stoney) |
| `-crnoflipkick` | Don't deliver a display flip's completion when it has been missing for 250 ms (otherwise a lost completion freezes the screen while macOS keeps running) |
| `-cracceldefer` | Hold the accelerator stack back until the display controller has finished setting up, then re-match it (experimental; off by default) |
| `-crnoautopanic` | Don't panic with the black-box report when the accelerator hasn't loaded the GPU microcode by 120 s |
| `-crnohwlibswire` | Don't lock HWLibs' code in memory before patching it (some boots its patched code reverted to Apple's original before CAIL ran) |
| `-crnohwlibsrepatch` | Don't re-apply the HWLibs patches when they're found missing at `AMDHardware::setupCAIL` (only report it) |
| `-crnorbplusfix` | Stoney: don't turn RB+ blend optimisations off in the GPU's clear-state defaults (`SX_BLEND_OPT_CONTROL`). Stoney is the only GFX8 chip with RB+; Apple's Polaris Metal driver never programs its registers, so they keep whatever the clear state sets (only recorded then) |
| `-crnoscpatch` | Don't patch Apple's Metal shader compiler (`libSC.dylib`, in `MTLCompilerService`) to use its Stoney back end (`SCGfx81Info`, instruction set 0x1E) instead of the generic GFX 8.0 one it picks for every chip the Metal driver reports as VI |
| `-crnosmallprim` | Let Apple's Metal driver enable the small-primitive filter (`PA_SU_SMALL_PRIM_FILTER_CNTL`), a Polaris feature Stoney doesn't get from Mesa |
| `-crnowalkfence` | Keep the Metal driver's `PA_SC_MODE_CNTL_1` walk fence size 3 (Mesa uses 2 on 2-pipe chips such as Stoney) |
| `-crdcc` | Let Apple's Metal driver give textures delta colour compression (DCC); by default CarrizoRed turns it off |
| `-crnointerpfix` | Don't rewrite pixel shaders whose `v_interp_p1_f32` writes the VGPR it reads (wrong on Stoney's 16-bank LDS; Apple's compiler emits it) |
| `-crnointerpsubmit` | Run the interpolation fix once a second from a thread instead of at every submission (content drawn before it gets to a shader stays wrong; no VGPR raises) |
| `-crnointerpbump` | Don't give a shader more VGPRs when it has no free register for the fix (those hazards are left alone) |
| `-crnostallpanic` | Don't panic with the black-box report when the GPU stops executing commands for 10 s |
| `-crats` | Let CAIL turn on the iGPU's PCIe ATS, PRI and PASID capabilities (IOMMUv2 features; macOS has no AMD IOMMU driver to answer them), which CarrizoRed otherwise leaves off |
| `-crnoarbfix` | Leave Apple's Polaris memory-arbiter tweak (`MC_ARB_MISC2` `SEQ_RDY_POP_IDLE` / `POP_IDLE_REPLAY`) in `AMDBaffinHardware::powerUp`; on the APU the arbiter is shared with the CPU |
| `-crnorlcreset`, `-crrlcreset` | With direct microcode loading: when CAIL halts the RLC to load its microcode, `-crnorlcreset` leaves coarse-grain clock gating and power gating alone (otherwise both are turned off); `-crrlcreset` also soft-resets the RLC again (CAIL already does, before its RLC set-up) |
| `-crgating` | Keep CAIL's clock/power-gating settings: when the RLC starts (otherwise MGCG overrides forced, CP/RLC memory light sleep and CP power gating off) and after start-up (otherwise `Cail_Stoney_EnablePowerGating`, `Stoney_UpdateGfxPowerGating` and `Stoney_EnableCpPowerGating` do nothing). With CAIL's power gating the GPU hangs after idling |
| `-crnofbreloc` | With acceleration: leave the carve-out's GPU address window where the firmware put it (`0xF4_0000_0000` on the D509BA) instead of moving it to 0. Apple's accelerator assumes VRAM at GPU address 0; elsewhere its "VRAM" accesses land in physical RAM |
| `-crnopp`, `-crpp` | Force PowerPlay off / on. By default it is on with acceleration and the Dummy back end, and off otherwise (`CFG_NO_PP`; without PowerPlay the accelerator never powers up its engines) |
| `-crnocapsfix` | Stoney: 1 shader engine and the CAIL-reported CU count instead of Polaris 11's 2 SE / 8 CUs |
| `-crnovmfix` | Converting local GPU page-table entries to the carve-out's physical address (`MC_VM_FB_OFFSET << 22`) |

### HDMI

External HDMI works on the D509BA and the Dell Inspiron 3475 (Tahoe), 2026-11-06: detection, EDID, a picture at boot
and on hot-plug, and the desktop moving back to the panel on unplug. No boot arguments needed. Each step that was
missing, in the order they were found (each has its own off switch above):

1. **Port policy:** AGDP, Apple's graphics policy, looks the SMBIOS model up and applies its port policy, which keeps
   the external port off. CarrizoRed makes it use its `Default` policy (`-crnomacgpu`).
2. **DDC (EDID):** Apple's ATOM parser for these VBIOSes reports the DDC line without the hardware I2C engine bit, so
   no EDID was read (`-crnoddcfix`), and the DDC pads need setting up for the engine first, as Linux DC does
   (`-crnoddcprep`).
3. **TMDS transmitter:** Apple's calls clocked the PHY from a PLL DCE 11.0 doesn't use and set the wrong connector and
   encoder mode; CarrizoRed builds them as Linux DC does (`-crnotmdsfix`).
4. **Display engine clock and power gating:** with all of that, the monitor woke but stayed black: the display engine
   clock was still the firmware's 100 MHz, too slow for 1080p, and Apple has no way to set it on these VBIOSes (no
   SetDCEClock). HDMI first worked after a wake, whose restore sets the clock; CarrizoRed now does the same at boot and
   before every display enable (`-crnobootdisp`).
5. **Replug:** on the Dell, after an unplug Apple's driver sometimes left the hot-plug pin waiting for another unplug,
   so plugging back in did nothing; CarrizoRed points the pin at the next plug-in, as Linux does (`-crnohpdfix`).

Still to do: HDMI audio, and DisplayPort outputs on machines that have them.

### Diagnostics and experiments

| Argument | Effect |
|---|---|
| `-croff` | Disable the kext |
| `-crdbg` | Debug logging (DEBUG build) |
| `-crbeta` | Allow loading on macOS versions newer than Tahoe |
| `crpanicat=N` | Panic on purpose N s after the controller loads, with the black-box report (see Installing) |
| `-crdisplaydiag` | Keep the display sections (VBIOS calls, DPCD, register diff, pixels) in the black box on accelerated boots |
| `crsmustep=N` | Stop the SMU microcode load after step N (1 region written … 7 RLC started) and panic with the report |
| `crstopat=<stage>[#n]` | Stop on entering a traced stage (its n-th call) and panic with the report, e.g. `crstopat=pm4.start` |
| `-crnohwlibsprobe` | Skip the test write into HWLibs' code at `setupCAIL` (used to check whether patches can land) |
| `-crnotrace` | No bring-up stage tracing |
| `-crnowatch`, `-crnowatchdog` | No periodic watcher output / no watchdog panic for a traced call that hangs |
| `-crbreadcrumb` | Write the latest trace progress to the NVRAM variable `CR-progress` during start-up (for boots that end in a hard reset, where no panic report is written; it stops once Metal is drawing). Off by default since 0.1.0: each write is a forced flash write |
| `-crnoreglog`, `-crreglogall`, `-crreglogintsoff` | No register logging / log every access from probe / print interrupts-off lines directly |
| `-crnoatomlog` | Don't record VBIOS command-table calls for the black box |
| `-crnommio` | Skip the MMIO register probe |
| `-crnopcitrace` | Don't trace the iGPU's PCI power states and config-space writes |
| `-crkeepgpuon` | Refuse PCI power-down of the iGPU |
| `-crpipeguard` | Drop writes to, and read 0 from, display-pipe register blocks the APU doesn't have |
| `-crnoctl` | Keep `AMD9500Controller` from matching |
| `-crnoagpm`, `-crnoagdp` | Keep AGPM / AGDP (Apple's GPU power management / graphics policy) off |
| `-crnoints`, `-crnoih`, `-crnostormbreak` | Don't install display interrupts / keep the IH ring disabled / no IH storm breaker (it only trips when the handler is flooded with no new ring entries) |
| `-crnovramwrite` | Skip `ATIController::TestVRAM` entirely |
| `-crstutter`, `-crfeds`, `-crfbwrklp` | Keep Apple's `CFG_USE_STUTTER` / `CFG_USE_FEDS` / `CFG_USE_FBWRKLP` (forced off by default) |
| `-crnoaudio`, `-crnotcon` | Force `CFG_NO_AUDIO` on / `CFG_USE_TCON` off |
| `-crrestore` | Write the firmware's boot values back into pipe 0's processing chain and hold them (display bisect) |
| `-crnosclkdpm` | Don't start the SMU's graphics clock scaling |
| `crsclkmin=N`, `crsclkmax=N` | The lowest and highest clock level (defaults: level 2, 400 MHz, and the top level; on the A9-9425 levels 0–7 are 200–900 MHz) |
| `-crnoclkread` | Don't read the SMU's clock table into the report |
| `-crnodisklog` | Don't write the per-boot log `/private/var/log/CarrizoRed-<time>.log` (GPU, SMU and clock state every second, SMU messages, draw census, black-box reports; written straight to disk, so it survives a freeze) |
| `-crnocensus` | Don't record each distinct draw set-up in the disk log |
| `-crnomodel` | Leave the iGPU's `model` alone (by default it is named from the CPU brand string, e.g. "AMD Radeon R5 Graphics", or a `model` from DeviceProperties is kept) |
| `-crwakemark` | Keep the sleep and wake events in the NVRAM variable `CR-wake`, from "will sleep" until 30 s after the wake, with the CPU's sleep and resume (`IOCPUSleepKernel`) and the power manager's phases (`IOPMrootDomain::tracePoint`). For a wake that leaves nothing in the disk log; forced flash writes, at most 100 per sleep. See [docs/SLEEP.md](docs/SLEEP.md) |
| `-crehcidiag` | Test: the FCH EHCI controller (EHC1) right before S3 and right after the firmware returns: PCI PMCSR (PME_Status, PME_En, D-state) and USBCMD/USBSTS/PORTSC, in the disk log ("EHC1 before S3 ...") and the EHCI's hub and S-state configuration |
| `-crcapsprobe` | Test: the Caps Lock LED, set through the keyboard controller, shows how far a wake gets: blinks 1 s going to sleep; on in the wake trampoline (the first macOS code after the firmware); one off-and-on flash per resume step in `acpi_sleep_kernel` (the disk log names them at boot); on 3 s once `acpi_sleep_kernel` returns; off once `IOCPUSleepKernel` has started the CPUs. See [docs/SLEEP.md](docs/SLEEP.md) |
| `-crwakememcheck` | Test, with `-crcapsprobe`: checksums the memory the early wake code reads just before S3 and checks it again in the trampoline; the Caps Lock LED goes off if it all matches, or blinks the number of the group that changed. See [docs/SLEEP.md](docs/SLEEP.md) |
| `-crnosleepflush` | No cache flush before S3 (otherwise `pal_machine_sleep`, where Apple's ACPI code would flush, runs `wbinvd`: without it the CPU state saved for the wake can be lost, see [docs/SLEEP.md](docs/SLEEP.md)) |
| `-crtscsync` | Test: after the wake starts the other CPUs, move any CPU whose TSC is behind forward to the highest one (all CPUs read it together), and measure the difference once at boot. Logged as "TSC ..." in the disk log. See [docs/SLEEP.md](docs/SLEEP.md) |
| `-crnowakepanic` | No wake watchdogs (otherwise a wake that doesn't complete panics with the black-box report, so it lands in NVRAM) |
| `-crpreemptcheck` | Check work loop event sources, interest handlers and the IH handler for leaked preemption disables, and panic naming the one that leaks (instead of a later "thread_invoke: preemption_level" panic with no culprit). Off by default since 0.1.0 |

## Layout

```
CarrizoRed/
  kern_start.cpp   Lilu plugin configuration / entry point
  kern_cr.*        Driver logic: device detection, display and accelerator patches, tracing, black box
  kern_asic.hpp    Device IDs and register offsets (from Linux amdgpu headers)
  kern_vbios.*     VBIOS from the ACPI VFCT table → "ATY,bin_image" (read first by AMDSupport)
  kern_sigs.hpp    Byte signatures for the HWLibs functions on Sonoma and Ventura (stripped there)
  kern_info.c      kmod_info declaration
  Info.plist.in    Bundle template (filled in by the Makefile)
tools/XHCIProbe/   Diagnostic kext for that controller (state, interrupts, link states)
docs/
  ROADMAP.md               Phase plan
  TEST_MACHINES.md         Test hardware and the boot-by-boot bring-up log
  SLEEP.md                 Sleep and wake: what S3 resets, CarrizoRed's resume steps, open issues
  APPLE_KEXTS.md           Getting Apple's AMD kexts from a KDK on Windows, and what's inside them
  CARRIZO_VS_POLARIS.md    Where Linux amdgpu treats Carrizo differently from Polaris
  DCE11_REGISTER_DIFF.md   DCE 11.0 vs 11.2 display register diff (full data in docs/data/)
  OLDER_APUS.md            GCN 2 APUs (Kaveri, Kabini, Mullins): the plan and the measurements behind it
  PHASE0.md                Hardware / environment / reverse-engineering checklist
  opencore/                SSDTs and the USB map for the D509BA
scripts/
  bootstrap.sh             Fetch Lilu + MacKernelSDK (macOS / CI)
  build_efi_d509ba.py      Build the D509BA's OpenCore EFI (and test variants)
  extract_kdk.py           Extract files from a KDK .dmg/.pkg without a Mac
  macho.py                 Minimal Mach-O reader (sections, symbols, disassembly)
  siggen.py                Signatures for HWLibs functions, checked against stripped builds (Sonoma)
  featmatch.py, addrsigs.py  Match functions across recompiled builds by their constants; signatures from those (Ventura)
  dce_regdiff.py           Diff two Linux DCE register header sets
```

## Licensing notes

- Lilu and MacKernelSDK are BSD-3-Clause. Linux `drm/amd` register headers are MIT.
- NootedRed and LegacyRed are under the "Thou Shalt Not Profit" licence: use them to understand
  approaches, **do not copy code from them**.
- Apple's kexts are never committed; they are read from a Kernel Debug Kit locally.

## Credits

- acidanthera for Lilu, WhateverGreen, OpenCore and MacKernelSDK
- The Linux `amdgpu` developers: the primary hardware reference
- ChefKiss / NootInc for proving the approach with NootedRed and exploring it with LegacyRed
- The VoodooI2C team (AMD I²C/GPIO support) and the Dortania guides

## License

BSD-3-Clause, see [LICENSE](LICENSE). CarrizoRed builds on [Lilu](https://github.com/acidanthera/Lilu) (BSD-3-Clause)
and uses register definitions and behaviour documented in Linux `amdgpu` (MIT). No code from NootedRed, LegacyRed or
other projects under restrictive licences is used, and no Apple binaries are included.
