# CarrizoRed

A [Lilu](https://github.com/acidanthera/Lilu) plugin that aims to bring macOS graphics support to AMD's
GCN 3 (GFX8) APUs by adapting Apple's own Polaris drivers (`AMDRadeonX4000` + `AMD9500Controller`) — the
same idea NootedRed applies to Vega iGPUs on top of `AMDRadeonX5000`.

**Status: Phase 1 (observe only).** The kext detects the iGPU and logs hardware information. It does not
patch anything yet and will not give you graphics acceleration.

## Targets

| APU family | Examples | PCI ID | GFX | Display | Status |
|---|---|---|---|---|---|
| Carrizo / Bristol Ridge | A12-9800, A10-9700, A10-8700P | 1002:9870, 9874–9877 | gfx801 | DCE 11.0 | Primary target |
| Stoney Ridge | A9-9420, A6-9220 | 1002:98E4 | gfx810 | DCE 11.0 | Later |

macOS: Monterey through Tahoe (26); the primary development target is **Sequoia (15.x)**. Excavator CPUs have AVX2, so Ventura+ runs without CPU hacks
(AMD_Vanilla patches still required).

Why this family: macOS removed the GCN 1–3 drivers in Ventura, but kept Polaris (GFX8, DCE 11.2). Carrizo is
the closest APU to Polaris — same GFX8 ISA family and a near-identical display engine — so Apple's
still-shipping kexts are the base.

## Building

Requires macOS with Xcode command-line tools.

```sh
bash scripts/bootstrap.sh   # fetches Lilu.kext (SDK) and MacKernelSDK
make DEBUG=1                # build/Debug/CarrizoRed.kext
make                        # build/Release/CarrizoRed.kext
```

No Mac? Push to GitHub — `.github/workflows/build.yml` builds both configurations on a macOS runner and
uploads them (plus a debug `Lilu.kext`) as a workflow artifact.

## Installing (test machine)

1. OpenCore with AMD_Vanilla patches, booting to desktop on the firmware framebuffer.
2. Add `Lilu.kext` (DEBUG build) then `CarrizoRed.kext` (DEBUG build) to `EFI/OC/Kexts` and `config.plist`,
   Lilu first.
3. Boot-args: `keepsyms=1 debug=0x100 -liludbgall -crdbg liludelay=1000`
4. After boot: `log show --last boot --predicate 'eventMessage CONTAINS "CarrizoRed"'` (or
   `sudo dmesg | grep CarrizoRed`) and `ioreg -lw0 | grep CarrizoRed`.

### Boot arguments

| Argument | Effect |
|---|---|
| `-croff` | Disable the kext |
| `-crdbg` | Enable debug logging (DEBUG build) |
| `-crbeta` | Allow loading on macOS versions newer than Tahoe |
| `-crnommio` | Skip the MMIO register probe |

## Layout

```
CarrizoRed/
  kern_start.cpp   Lilu plugin configuration / entry point
  kern_cr.*        Main driver logic (device detection, kext load hooks)
  kern_asic.hpp    Device IDs and register offsets (from Linux amdgpu headers)
  kern_info.c      kmod_info declaration
  Info.plist.in    Bundle template (filled in by the Makefile)
docs/
  ROADMAP.md               Phase plan
  PHASE0.md                Hardware / environment / reverse-engineering checklist
  CARRIZO_VS_POLARIS.md    Where Linux amdgpu treats Carrizo differently from Polaris
  DCE11_REGISTER_DIFF.md   DCE 11.0 vs 11.2 display register diff (full data in docs/data/)
  APPLE_KEXTS.md           Getting Apple's AMD kexts from a KDK on Windows, and what's inside them
scripts/
  bootstrap.sh             Fetch Lilu + MacKernelSDK (macOS / CI)
  extract_kdk.py           Extract files from a KDK .dmg/.pkg without a Mac
  dce_regdiff.py           Diff two Linux DCE register header sets
```

## Licensing notes

- Lilu and MacKernelSDK are BSD-3-Clause. Linux `drm/amd` register headers are MIT.
- NootedRed and LegacyRed are under the "Thou Shalt Not Profit" licence: use them to understand
  approaches, **do not copy code from them**.
- A licence for this project has not been chosen yet.

## Credits

- acidanthera for Lilu, WhateverGreen and MacKernelSDK
- The Linux `amdgpu` developers — the primary hardware reference
- ChefKiss / NootInc for proving the approach with NootedRed and exploring it with LegacyRed
