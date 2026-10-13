# Older AMD APUs (GCN 2: Kaveri, Kabini, Mullins) on current macOS

**Summary.** macOS dropped the GCN 1–3 drivers in Ventura, but Sequoia's AMD hardware libraries still contain AMD's own
chip-initialisation code **and the complete GPU firmware** for the GCN 2 APUs: Kaveri ("Spectre"), Kabini
("Kalindi") and Beema/Mullins ("Godavari"). What's gone is the display driver for their DCE 8 display engine and a GCN 2 hardware
layer in the accelerator. A driver for these APUs on Ventura and later is therefore not a port of an existing Apple
driver, as CarrizoRed is for Polaris. It would partly have to be built from the ground up, on top of the pieces that
survive. This page records what survives (checked in the macOS 15.8.1 KDK, build 24H32) and what a project would
need.

CarrizoRed's own targets (Carrizo, Bristol Ridge, Stoney Ridge) come first. This is groundwork for later.

## The chips

| Codename (Apple CAIL name) | Products | PCI IDs | GPU | Display | Linux driver |
|---|---|---|---|---|---|
| Kaveri, incl. its 2015 refresh ("Spectre") | A10-7850K/7870K/7890K, A8-7600, A10-7300/7400P, FX-7600P | 1002:1304–131D | GCN 2 (gfx700), up to 8 CUs | DCE 8.1 | `amdgpu` (CIK) / `radeon` |
| Kabini ("Kalindi") | A4-5000, A6-5200, E1/E2-2xxx, Athlon/Sempron on AM1 | 1002:9830–983D | GCN 2 (gfx703), 2 CUs | DCE 8.3 | same |
| Beema / Mullins ("Godavari") | A4-6210, A6-6310, E1-6010, E2-6110; A10 Micro-6700T, A4 Micro-6400T | 1002:9850–985F | GCN 2 (gfx703), 2 CUs | DCE 8.3 | same |

CAIL's names are AMD-internal. Its `GODAVARI` rows cover 9850–985F, the IDs Linux lists as Mullins (and Beema),
and they use the Kalindi code path family.

## What Sequoia still contains

### CAIL, AMD's ASIC initialisation library: present, with GCN 2 support

Every HWLibs plug-in of `AMDRadeonX4000HWServices.kext` (`AMDRadeonX4000HWLibs`, `…4100…`, `…4400…`, `…4700…`) is built
from the same CAIL source and still knows these chips:

- **Device table** (`_CAILAsicCapsInitTable`; rows of family, device ID, revisions, caps, golden settings):
  - family `0x7D` (KV, the GCN 2 APUs): `SPECTRE_A0` with device IDs 1304–1318, `SPECTRE_A0_E` (131B–131D),
    `KALINDI_A0/A1` (9830–983D), `KALINDI_A0_E/A1_E`, `GODAVARI_A0` (9850–985F) and `GODAVARI_NO_EYEFINITY_A0`
    (9854–9856);
  - family `0x78` (CI, GCN 2 dGPUs: Bonaire, Hawaii, Saturn, Maui) and `0x6E` (SI, GCN 1: Tahiti, Pitcairn, Cape Verde,
    Oland, Hainan…) are there too, next to VI (Polaris), CZ (Carrizo/Stoney), Vega, Navi and Raven/Renoir.
- **Per-chip function tables:** `_CAILInitFunctionPointer` still dispatches to `_Cail_Spectre_InitFunctionPointer`
  and `_Cail_Kalindi_InitFunctionPointer` (and Tahiti/Bonaire/Hawaii/Iceland ones).
- **Setup data:** `_SPECTRE_GoldenSettings_A0_8812(_E)`, `_aSpectre_GoldenRegisterSettings`,
  `_Spectre_HwConstants_8812(_E)`; `_KALINDI_GoldenSettings_A0_4882(_E)`, `_aKalindi_GoldenRegisterSettings`,
  `_Kalindi_HwConstants_4882(_E)`.
- **Power and SMU helpers:** `_Cail_Spectre_InitializePowerGating`, `_Spectre_Disable…PowerGating`,
  `_Cail_Spectre_GetSmuFwVersion`, `_Cail_Kalindi_SetSmuDfsBypassMode`, `_Godavari_FormatSmuDramDataBuffer`, and the
  KV SMU buffer helpers. Device groups for OEM variants: `_KaveriMobileGroup1–3`, `_KaveriDesktopGroup`,
  `_KaveriAIOGroup1–2`, `_KaveriEmbeddedGroup`.
- **Firmware, embedded in the binary.** Each `*_UCODE` symbol is a descriptor: size in dwords, version string, version
  number, data pointer. The data is real and non-empty:

| Firmware | Kaveri (`Spectre_*`) | Kabini/Mullins (`Kalindi_*`) |
|---|---|---|
| CE (constant engine) | 8576 B, #122 | 8576 B, #122 |
| PFP (prefetch parser) | 8576 B, #229 | 8576 B, #229 |
| ME (micro engine) | 8576 B, #187 | 8576 B, #187 |
| MEC1 (compute) | 16768 B, #421 | 16768 B, #421 |
| MEC2 | 16768 B, #421 | none (Kabini has one MEC) |
| RLC | 10240 B, #57 | 10240 B, #43 |
| SDMA0 / SDMA1 | 4200 B each, #76 | 4200 B each, #76 |

The dword counts are exactly what Linux's `radeon` driver expects for these chips: `CIK_CE/PFP/ME_UCODE_SIZE` 2144,
`CIK_MEC_UCODE_SIZE` 4192, `KV_RLC_UCODE_SIZE` / `KB_RLC_UCODE_SIZE` 2560 and `CIK_SDMA_UCODE_SIZE` 1050. That is
the firmware Linux loads from `kaveri_*.bin` / `kabini_*.bin` / `mullins_*.bin`. So **no firmware has to be supplied
from outside**: the
copies Apple ships can be used, as CAIL already does for Stoney on the D509BA.

CAIL reads the real device ID from PCI config space (`AmdCailServices::AmdCailServices` →
`extendedConfigRead16(2)`), so on these APUs it would select the Spectre/Kalindi paths by itself, as it selects
Stoney today.

### Display: absent

Sequoia ships one AMD display controller, `AMD9500Controller` (Polaris, DCE 11.2; the only ASIC class is
`ASIC_INFO__BAFFIN`), plus the shared `AMDSupport` and `AMDFramebuffer`. Neither has any DCE 8, Sea Islands, Kaveri
or Kabini code (no matching symbols in either). The older controllers (`AMD7000Controller` for SI,
`AMD8000Controller` for CI/DCE 8, `AMD9000Controller` for Tonga/Fiji) were removed with Ventura.

DCE 8 differs from DCE 11 far more than DCE 11.0 differs from 11.2: different register layout, no DCE 11 pipe blocks,
and different PLL and encoder handling (Linux has separate `dce_v8_0.c` / `dce_v11_0.c`). Bending `AMD9500Controller`
onto DCE 8, as CarrizoRed does for DCE 11.0, is unlikely to be practical. Options:

1. **Keep the firmware framebuffer.** The GOP framebuffer works today (that's how the D509BA ran before CarrizoRed):
   a fixed resolution, no brightness, no external displays. Enough to use the machine, and enough to test
   acceleration.
2. **A new, small `IOFramebuffer` driver for DCE 8**, using the VBIOS's ATOM command tables for mode setting
   (`SetCRTC_Timing`, `SetPixelClock`, `DIGxEncoderControl`, `DIGxTransmitterControl`, `EnableCRTC`). AMDSupport's
   ATOM interpreter (`AtiAtomBiosParserService`, `AtiBiosParser1` for legacy atombios) is still there and could be
   reused. Linux `dce_v8_0.c` and the `radeon` ATOM code (both MIT) are the reference.

### Acceleration: hardware layer absent, much of the rest shared

`AMDRadeonX4000` only has a GFX8 hardware layer: `AMDHardware` → `AMDVIHardware` → `AMDBaffinHardware` /
`AMDEllesmereHardware`, plus the VI PM4, sDMA, VM manager and interrupt classes. There is no `AMDCIHardware`, no CI
PM4 engine and no CI VM manager. Classes named after older generations are still there, because the shipping driver
inherits from them. They are generation-independent code, not a GCN 2 hardware layer:
- `AMDSIGLContext`, the GL/command-stream context
- `AMDCICLContext`, the compute context
- `AMDSIVideoContext`
- the CI VCE and SAMU engine and ring classes

What a GCN 2 accelerator would need, compared with the GFX8 path CarrizoRed is adapting:

| Area | GCN 2 vs GCN 3 | Size of the job |
|---|---|---|
| ASIC init, golden registers, firmware load | In CAIL (above) | Mostly done by Apple's code |
| Memory controller / VM | GMC 7 vs GMC 8: similar VM, different register offsets | Moderate: patch offsets |
| Interrupt handler | CIK IH vs Tonga IH: close (CarrizoRed already fixed one IH difference) | Small |
| SDMA | SDMA 2 (CIK) vs SDMA 3: similar packets | Moderate |
| PM4 command processor | Same packet format, a few register differences | Moderate |
| **Shader instruction set** | **GFX7 (GCN 2) and GFX8 (GCN 3) use different instruction encodings: opcodes and formats changed in GCN 3** | **Large; see below** |

**The real obstacle is user space.** macOS's Metal and GL drivers compile shaders for the GPU at run time. Sequoia's
compiler is expected to emit GFX8 and newer code only, since the last Macs with GCN 1/2 GPUs aren't supported any
more. GCN 3 re-encoded the instruction set (opcode numbers and some formats changed), so GFX8 binaries won't run on a
GCN 2 GPU. Making acceleration work would mean one of:
- forcing the compiler to target GFX7, if the code is still in the user-space driver (unverified: the user-space
  bundles aren't in the KDK and have to be checked on an installed system);
- translating GFX8 shader binaries to GFX7 when they're submitted (hard: register and encoding differences, and
  instructions without a GFX7 equivalent);
- or display-only support (the framebuffer, or a DCE 8 driver), with no Metal.

## A possible route

1. **Display:** start with the GOP framebuffer. Then, if worth it, a small DCE 8 `IOFramebuffer` driver driven by the
   VBIOS's ATOM tables.
2. **Bring the GPU up with CAIL:** attach HWServices to the APU (as CarrizoRed does for Stoney) and check that CAIL's
   Spectre/Kalindi paths initialise it and load the embedded firmware. CarrizoRed's tracing and black box already
   cover this.
3. **Kernel accelerator:** a GFX7 hardware layer derived from `AMDVIHardware`, with GMC 7, CIK IH, SDMA 2 and CI PM4
   register differences patched in. Linux `gfx_v7_0.c`, `gmc_v7_0.c`, `cik_ih.c` and `cik_sdma.c` are the reference.
4. **User space:** find out what the installed system's shader compiler can target. That decides whether Metal is
   reachable at all.

## Open questions (to check on hardware or an installed system)

- Do Sequoia's user-space AMD drivers (`AMDMTLBronzeDriver`, `AMDShared…`) still contain a GFX7 code generator?
- Does CAIL's Spectre/Kalindi path run cleanly on real hardware? It may not have been exercised since macOS stopped
  supporting these chips.
- Does CAIL's `GODAVARI` (Beema/Mullins) code path have its own firmware and setup data, or does it reuse Kalindi's? It
  has its own SMU helpers (`_Godavari_FormatSmuDramDataBuffer` etc.), but only 2 firmware descriptors were found
  under that name.
- How do the SMU and power management behave? CAIL has KV SMU helpers; the PowerPlay side is unchecked.

## How this was checked

`AMDRadeonX4000HWLibs` (and the 4100/4400/4700 variants), `AMDRadeonX4000`, `AMDSupport` and `AMD9500Controller` from
the macOS 15.8.1 KDK (24H32), read on Windows with `scripts/extract_kdk.py` and a small Mach-O symbol / disassembly
helper. The firmware sizes come from each `*_UCODE` descriptor's dword count, and the data was checked to be
non-empty. All four HWLibs variants contain the Spectre (50 symbols) and Kalindi (24 symbols) code.
