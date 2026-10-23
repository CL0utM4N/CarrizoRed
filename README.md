# CarrizoRed

A [Lilu](https://github.com/acidanthera/Lilu) plugin that brings macOS graphics to AMD's GCN 3 (GFX8) APUs by adapting
Apple's own Polaris drivers (`AMD9500Controller` + `AMDSupport` for display, `AMDRadeonX4000` for acceleration). It's
the same idea NootedRed applies to Vega iGPUs on top of `AMDRadeonX5000`. macOS has never supported these APUs (no Mac
used them), so on a hackintosh they've only had the firmware framebuffer.

## Status

> **Experimental, in active development.** There are no releases yet. Expect kernel panics and black screens, and
> don't run it on a machine you depend on. Everything below is from one test laptop.

**What it does today:** on a Stoney Ridge laptop, macOS Sequoia boots to the desktop and drives the built-in panel
through Apple's own Polaris display driver, instead of the firmware's fixed framebuffer. **Metal acceleration works
and the desktop renders correctly:** WindowServer draws the login screen and desktop through the GPU, apps use Metal,
and vibrancy, text, icons and the Dock draw properly. Folder icons still draw as flat blue squares, and it needs
longer testing. The GPU clock now scales with load (400 MHz idle, up to the chip's top level).

| | Status |
|---|---|
| Built-in eDP panel | **Works** (1366×768) through Apple's `AMD9500Controller` / `AMDFramebuffer`: Recovery, the installer and an installed Sequoia 15.8.1 |
| Display interrupts (vblank) | **Works** |
| Acceleration / Metal (`AMDRadeonX4000`) | **Works.** The SMU loads the GPU microcode, the command processor and SDMA run, interrupts work, and WindowServer and apps render with Metal. Apple's shader compiler emits an interpolation pattern Stoney's 16-bank LDS gets wrong; CarrizoRed fixes the compiled pixel shaders before the GPU runs them (about 0.1% of one CPU core). Boot with `-crnoaccel` for an unaccelerated desktop |
| GPU clock scaling (SCLK DPM) | **Works, on by default.** Apple's PowerPlay runs its Dummy back end here, so nothing would start the SMU's clock scaling and the GPU would stay at its lowest clock. CarrizoRed starts it 90 s after boot, from the SMU's own clock table: 400 MHz minimum, up to the top level (900 MHz on an A9-9425, 800 MHz on an A9-9420). `-crnosclkdpm` turns it off |
| Brightness, external displays (HDMI), sleep/wake, HDMI audio | Not done or untested |
| Video decode/encode (UVD/VCE) | **In development.** Stoney's UVD 6.2 / VCE 3.4 are close to Polaris's UVD 6.3 / VCE 3.4, which Apple's driver supports. Stage 1 (`-crvideo`, opt-in): the SMU powers both engines and Apple's driver starts them; firmware and codec work follow. Plan in [docs/VIDEO.md](docs/VIDEO.md) |
| Carrizo / Bristol Ridge | Same display engine and code paths; the SMU microcode loader, clock scaling and shader-engine fix include Carrizo's differences (second SDMA engine, second MEC), following Linux. **Untested** (no hardware): reports from owners are very welcome |

**Tested on:**

| Machine | APU | macOS | Result |
|---|---|---|---|
| ASUS VivoBook 15 D509BA | A9-9425 (Stoney Ridge, Radeon R5) | Sequoia 15.4 (Recovery, installer), 15.8.1 (installed) | Display and Metal acceleration work; desktop renders correctly except folder icons |

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
- the video engines left off.

And Metal needed:
- the compiled pixel shaders fixed for Stoney's 16-bank LDS: `v_interp_p1_f32` must not write the VGPR it reads, and
  Apple's shader compiler (even its own Stoney back end) does that for the last component of an attribute. CarrizoRed
  rewrites those shaders in GPU memory as each command buffer is submitted, before the GPU runs them. When the
  broken result is part of a register block (an image coordinate), the whole block moves to fresh registers;
- the small-primitive filter off, the 2-pipe walk fence and no delta colour compression (Polaris settings Mesa doesn't
  use on Stoney), and the Metal compiler switched to its Stoney back end (patched in the dyld shared cache);
- the GPU and CPU named after the APU in About This Mac.

## Development status

| Area | State |
|---|---|
| Display (built-in eDP panel) | Works |
| Metal acceleration | Works; the desktop renders correctly except folder icons |
| Stoney's 16-bank LDS shader fix | Works, about 0.1% of one CPU core |
| GPU clock scaling | Works, on by default, up to the chip's top level |
| Freezes with clock scaling | Fixed: CarrizoRed's own interrupt-storm breaker was tripping on real interrupt bursts at higher clocks |
| Folder icons (flat squares: shape mask not applied) | Being diagnosed with the disk log's draw census |
| OpenGL | Expected to work, untested: Apple's AMD GL driver compiles through the same `libSC` CarrizoRed patches for Stoney (its `ATIRadeonX4000SCLib` wraps it), and the shader fix works on every submission whatever the API |
| Video decode/encode | Stage 1 (power and engine start) in testing behind `-crvideo`; then Stoney firmware and codec limits ([docs/VIDEO.md](docs/VIDEO.md)) |
| Carrizo / Bristol Ridge | Code paths written (SMU loader, clock scaling, caps), untested: no hardware |
| External displays, brightness, sleep/wake, HDMI audio | Not started / untested |
| Rare shaders the fix leaves alone | Open (a handful per session, none visibly wrong so far) |
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
| Stoney Ridge | A9-9420/9425, A6-9220/9225, A4-9120, E2-9000 | 1002:98E4 | gfx810 | DCE 11.0, 2 pipes | Display and Metal working (D509BA) |
| Carrizo / Bristol Ridge | A12-9800, A10-9700, FX-9800P, A10-8700P, FX-8800P | 1002:9870, 9874–9877 | gfx801 | DCE 11.0, 3 pipes | Untested |

Older GCN 2 APUs (Kaveri, Kabini, Beema/Mullins) may follow later. Sequoia still contains AMD's init code and
complete GPU firmware for them, but no display or GCN 2 accelerator driver; see
[docs/OLDER_APUS.md](docs/OLDER_APUS.md).

**macOS:** the development target is **Sequoia (15.x)**; Monterey through Tahoe are allowed. Excavator CPUs have AVX2,
so Ventura and later run without CPU hacks (AMD_Vanilla patches are still needed).

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
2. **Device ID spoof:** a `DeviceProperties` entry on the iGPU (`PciRoot(0x0)/Pci(0x1,0x0)`) setting `device-id` to
   `EF670000` (Polaris 11 / Baffin, `0x67EF`). That's what makes Apple's Polaris kexts match. CAIL still reads the real
   ID from the hardware.
3. **Boot-args:** `-v keepsyms=1 debug=0x100` for testing, plus `-crnoaccel` to bring up the display without the
   accelerator.
4. **When something goes wrong:** with `crpanicat=N`, CarrizoRed panics on purpose N seconds after the display driver
   loads. The panic text carries a "black box" report: trace, registers, link, interrupts and accelerator state.
   `debug=0x100` makes the machine halt on the panic instead of rebooting. Read the report back from NVRAM
   (`AAPL,PanicInfo`) after the next boot, e.g. with OpenShell `dmpstore -all -s fs0:\nvram.bin`.
5. **After boot:** `log show --last boot --predicate 'eventMessage CONTAINS "CarrizoRed"'` and
   `ioreg -lw0 | grep CarrizoRed`.

## USB on these machines

The APUs' built-in USB 3 controller (FCH xHCI `1022:7914`) never delivers its interrupts to macOS, so USB crawls
(a USB stick reads at ~130 KB/s, and installing macOS from one takes most of a day). The standalone
[FCHXHCIFix](https://github.com/CL0utM4N/FCHXHCIFix) kext fixes it (its own repository, with releases). It doesn't
depend on CarrizoRed or Lilu, so put it in the EFI **before** installing macOS. Its README has the cause, the diagnosis
and the list of affected machines.

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
| `-crnoedppoweron` | The eDP panel POWER_ON transmitter action before Apple enables the eDP transmitter |
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
| `-crvideo` | Video stage 1 (in development): have the SMU power UVD and VCE up, then let Apple's driver start them (SAMU stays off) |
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
| `-crnobreadcrumb` | Don't write the latest trace progress to the NVRAM variable `CR-progress` during start-up (kept for boots that end in a hard reset, where no panic report is written; it stops once Metal is drawing) |
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
| `-crmacgpu` | Neutralise AGDP's board-id lookup; set `built-in` and `AAPL,slot-name` on the iGPU |
| `-crnosclkdpm` | Don't start the SMU's graphics clock scaling |
| `crsclkmin=N`, `crsclkmax=N` | The lowest and highest clock level (defaults: level 2, 400 MHz, and the top level; on the A9-9425 levels 0–7 are 200–900 MHz) |
| `-crnoclkread` | Don't read the SMU's clock table into the report |
| `-crnodisklog` | Don't write the per-boot log `/private/var/log/CarrizoRed-<time>.log` (GPU, SMU and clock state every second, SMU messages, draw census, black-box reports; written straight to disk, so it survives a freeze) |
| `-crnocensus` | Don't record each distinct draw set-up in the disk log |
| `-crnomodel` | Leave the iGPU's `model` alone (by default it is named from the CPU brand string, e.g. "AMD Radeon R5 Graphics", or a `model` from DeviceProperties is kept) |

## Layout

```
CarrizoRed/
  kern_start.cpp   Lilu plugin configuration / entry point
  kern_cr.*        Driver logic: device detection, display and accelerator patches, tracing, black box
  kern_asic.hpp    Device IDs and register offsets (from Linux amdgpu headers)
  kern_vbios.*     VBIOS from the ACPI VFCT table → "ATY,bin_image" (read first by AMDSupport)
  kern_info.c      kmod_info declaration
  Info.plist.in    Bundle template (filled in by the Makefile)
tools/XHCIProbe/   Diagnostic kext for that controller (state, interrupts, link states)
docs/
  ROADMAP.md               Phase plan
  TEST_MACHINES.md         Test hardware and the boot-by-boot bring-up log
  APPLE_KEXTS.md           Getting Apple's AMD kexts from a KDK on Windows, and what's inside them
  CARRIZO_VS_POLARIS.md    Where Linux amdgpu treats Carrizo differently from Polaris
  DCE11_REGISTER_DIFF.md   DCE 11.0 vs 11.2 display register diff (full data in docs/data/)
  OLDER_APUS.md            What Sequoia still has for GCN 2 APUs (Kaveri, Kabini, Mullins)
  PHASE0.md                Hardware / environment / reverse-engineering checklist
  opencore/                SSDTs and the USB map for the D509BA
scripts/
  bootstrap.sh             Fetch Lilu + MacKernelSDK (macOS / CI)
  build_efi_d509ba.py      Build the D509BA's OpenCore EFI (and test variants)
  extract_kdk.py           Extract files from a KDK .dmg/.pkg without a Mac
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
