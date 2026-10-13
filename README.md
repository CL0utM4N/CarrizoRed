# CarrizoRed

A [Lilu](https://github.com/acidanthera/Lilu) plugin that brings macOS graphics to AMD's GCN 3 (GFX8) APUs by adapting
Apple's own Polaris drivers (`AMD9500Controller` + `AMDSupport` for display, `AMDRadeonX4000` for acceleration). It's
the same idea NootedRed applies to Vega iGPUs on top of `AMDRadeonX5000`, for chips macOS dropped in Ventura.

## Status

**Phase 2 (display) works on the test laptop; Phase 3 (acceleration) has started.** The test laptop is an ASUS
VivoBook 15 D509BA (A9-9425, Stoney Ridge) on macOS Sequoia 15.4 (Recovery and installer):

| | |
|---|---|
| Built-in eDP panel | **Working:** 1366×768 through Apple's `AMD9500Controller` / `AMDFramebuffer`; boots to the Recovery and installer GUI |
| Display interrupts (vblank) | **Working** |
| Acceleration (Metal, `AMDRadeonX4000`) | **In progress.** CAIL (Apple's AMD init library) supports Stoney natively; first patches for Stoney's 1 sDMA engine, 1 shader engine and APU page tables are in. The accelerator hasn't attached in a test boot yet |
| Brightness, HDMI, sleep/wake, audio | Not done or untested |
| Carrizo / Bristol Ridge | Same display engine and code paths, **untested** (no hardware) |

The display needed fixes for the differences between DCE 11.0 (these APUs) and DCE 11.2 (Polaris):
- 2–3 display pipes instead of 6;
- the APU's PLLs and DIGs;
- the line-buffer size;
- the eDP link (one lane, 6 bpc, panel power-on, external reference clock, and re-enabling ASSR in the panel);
- the interrupt handler's enable bit, which sits in a different register on Carrizo/Stoney than on Polaris.

Each one is documented in the code and in [docs/TEST_MACHINES.md](docs/TEST_MACHINES.md).

Expect panics and black screens. This is a development project, not something to run on a machine you depend on.

## Targets

| APU family | Examples | PCI ID | GFX | Display | Status |
|---|---|---|---|---|---|
| Stoney Ridge | A9-9420/9425, A6-9220/9225, A4-9120, E2-9000 | 1002:98E4 | gfx810 | DCE 11.0, 2 pipes | Display working (D509BA) |
| Carrizo / Bristol Ridge | A12-9800, A10-9700, FX-9800P, A10-8700P, FX-8800P | 1002:9870, 9874–9877 | gfx801 | DCE 11.0, 3 pipes | Untested |

Older GCN 2 APUs (Kaveri, Kabini, Beema/Mullins) may follow later. Sequoia still contains AMD's init code and
complete GPU firmware for them, but no display or GCN 2 accelerator driver; see
[docs/OLDER_APUS.md](docs/OLDER_APUS.md).

**macOS:** the development target is **Sequoia (15.x)**; Monterey through Tahoe are allowed. Excavator CPUs have AVX2,
so Ventura and later run without CPU hacks (AMD_Vanilla patches are still needed).

**Why this family:** macOS removed the GCN 1–3 drivers in Ventura but kept Polaris (GFX8, DCE 11.2). Carrizo-class
APUs are the closest to Polaris: the same GFX8 instruction set and a near-identical display engine. Apple's
still-shipping kexts are the base, and Apple's own CAIL library still contains Carrizo and Stoney support.

## Building

Requires macOS with the Xcode command-line tools:

```sh
bash scripts/bootstrap.sh   # fetches Lilu.kext (SDK) and MacKernelSDK
make DEBUG=1                # build/Debug/CarrizoRed.kext
make                        # build/Release/CarrizoRed.kext
make -C FCHXHCIFix          # FCHXHCIFix/build/FCHXHCIFix.kext (USB fix, see below)
```

No Mac? Push to GitHub: `.github/workflows/build.yml` builds CarrizoRed (Debug and Release), FCHXHCIFix and the
XHCIProbe diagnostic on a macOS runner, and uploads them with a debug `Lilu.kext` as one workflow artifact.
Development for this project is done entirely on Windows this way.

## Installing (test machine)

`scripts/build_efi_d509ba.py` builds the complete OpenCore EFI used for the D509BA. It's a good starting point for
similar machines. It includes:
- SMBIOS iMac19,1;
- AMD_Vanilla;
- the ACPI fixes (`SSDT-XOSI`, EC/USBX) and a USB map;
- VoodooPS2 and VoodooI2C (touchpad);
- FCHXHCIFix;
- with `--carrizored --spoof`, CarrizoRed and the iGPU spoof below.

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
[FCHXHCIFix](FCHXHCIFix/README.md) kext fixes it. It doesn't depend on CarrizoRed or Lilu, so put it in the EFI
**before** installing macOS. The cause, the diagnosis and the list of affected machines are in
[docs/FCH_XHCI.md](docs/FCH_XHCI.md).

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
| `-crnoppdummy` | Stoney/Carrizo: don't set `PP_PhmUseDummyBackEnd` (PowerPlay has no back end for these APUs; without the Dummy one, `initializePowerPlay` fails and the accelerator never powers up) |
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
| `-crnotrace` | No bring-up stage tracing |
| `-crnowatch`, `-crnowatchdog` | No periodic watcher output / no watchdog panic for a traced call that hangs |
| `-crnobreadcrumb` | Don't write the latest trace progress to the NVRAM variable `CR-progress` (kept for boots that end in a hard reset, where no panic report is written) |
| `-crnoreglog`, `-crreglogall`, `-crreglogintsoff` | No register logging / log every access from probe / print interrupts-off lines directly |
| `-crnoatomlog` | Don't record VBIOS command-table calls for the black box |
| `-crnommio` | Skip the MMIO register probe |
| `-crnopcitrace` | Don't trace the iGPU's PCI power states and config-space writes |
| `-crkeepgpuon` | Refuse PCI power-down of the iGPU |
| `-crpipeguard` | Drop writes to, and read 0 from, display-pipe register blocks the APU doesn't have |
| `-crnoctl` | Keep `AMD9500Controller` from matching |
| `-crnoagpm`, `-crnoagdp` | Keep AGPM / AGDP (Apple's GPU power management / graphics policy) off |
| `-crnoints`, `-crnoih`, `-crnostormbreak` | Don't install display interrupts / keep the IH ring disabled / no IH storm breaker |
| `-crnovramwrite` | Skip `ATIController::TestVRAM` entirely |
| `-crstutter`, `-crfeds`, `-crfbwrklp` | Keep Apple's `CFG_USE_STUTTER` / `CFG_USE_FEDS` / `CFG_USE_FBWRKLP` (forced off by default) |
| `-crnoaudio`, `-crnotcon` | Force `CFG_NO_AUDIO` on / `CFG_USE_TCON` off |
| `-crrestore` | Write the firmware's boot values back into pipe 0's processing chain and hold them (display bisect) |
| `-crmacgpu` | Neutralise AGDP's board-id lookup; set `built-in`, `AAPL,slot-name` and `model` on the iGPU |

## Layout

```
CarrizoRed/
  kern_start.cpp   Lilu plugin configuration / entry point
  kern_cr.*        Driver logic: device detection, display and accelerator patches, tracing, black box
  kern_asic.hpp    Device IDs and register offsets (from Linux amdgpu headers)
  kern_vbios.*     VBIOS from the ACPI VFCT table → "ATY,bin_image" (read first by AMDSupport)
  kern_info.c      kmod_info declaration
  Info.plist.in    Bundle template (filled in by the Makefile)
FCHXHCIFix/        Standalone kext: services the FCH xHCI (1022:7914), whose interrupts never reach macOS
tools/XHCIProbe/   Diagnostic kext for that controller (state, interrupts, link states)
docs/
  ROADMAP.md               Phase plan
  TEST_MACHINES.md         Test hardware and the boot-by-boot bring-up log
  APPLE_KEXTS.md           Getting Apple's AMD kexts from a KDK on Windows, and what's inside them
  CARRIZO_VS_POLARIS.md    Where Linux amdgpu treats Carrizo differently from Polaris
  DCE11_REGISTER_DIFF.md   DCE 11.0 vs 11.2 display register diff (full data in docs/data/)
  FCH_XHCI.md              Why USB crawls on these APUs under macOS, and FCHXHCIFix
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
- A licence for this project has not been chosen yet.

## Credits

- acidanthera for Lilu, WhateverGreen, OpenCore and MacKernelSDK
- The Linux `amdgpu` developers: the primary hardware reference
- ChefKiss / NootInc for proving the approach with NootedRed and exploring it with LegacyRed
- The VoodooI2C team (AMD I²C/GPIO support) and the Dortania guides
