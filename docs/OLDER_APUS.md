# Older AMD APUs (GCN 2: Kaveri, Kabini, Mullins)

Updated 2026-11-03.

**Decision (2026-11-03): GCN 2 support goes into CarrizoRed, as a pure Lilu plugin on Apple's current Polaris
drivers, the same way Stoney is done.** No OpenCore Legacy Patcher, no files added to the system, no root patching,
SIP left on. This page records what that route has to do and the measurements behind it.

In short:
- **Metal can already compile for these chips.** Every macOS from Monterey to Tahoe still ships AMD's GFX7 shader
  compiler back end (Bonaire), and Sequoia's compiler still sends the GCN 2 APU family to it.
- **CAIL, AMD's chip-initialisation code inside Apple's HWLibs, still initialises these chips** and carries their
  complete firmware. They load firmware directly, so there's no SMU loader to write (unlike Stoney).
- **The accelerator is mostly a register-offset job.** 94% of the GFX registers sit at the same offsets on GCN 2 as
  on Polaris. The interrupt handler block moved, and the memory controller differs in places.
- **The display is closer than this page used to say.** For the first display pipe, the timing, surface, line buffer
  and scaler registers are at the same offsets on DCE 8 as on Polaris's DCE 11.2. The buffer/watermark, encoder and
  DisplayPort blocks moved, and the per-pipe stride differs. That fits CarrizoRed's existing register wrapper.
- **The first blocker isn't the GPU.** macOS doesn't boot yet on either Kabini (Jaguar) test laptop. A Kaveri
  machine avoids that problem.

## The chips

| Codename (Apple CAIL name) | Products | PCI IDs | GPU | Display | Linux driver |
|---|---|---|---|---|---|
| Kaveri, incl. its 2015 refresh ("Spectre") | A10-7850K/7870K/7890K, A8-7600, A10-7300/7400P, FX-7600P | 1002:1304–131D | GCN 2 (gfx700), up to 8 CUs | DCE 8.1, 4 pipes | `amdgpu` (CIK) / `radeon` |
| Kabini ("Kalindi") | A4-5000, A6-5200, E1/E2-2xxx, Athlon/Sempron on AM1 | 1002:9830–983D | GCN 2 (gfx703), 2 CUs | DCE 8.3, 2 pipes | same |
| Beema / Mullins ("Godavari") | A4-6210, A6-6310, E1-6010, E2-6110; A10 Micro-6700T, A4 Micro-6400T | 1002:9850–985F | GCN 2 (gfx703), 2 CUs | DCE 8.3, 2 pipes | same |

CAIL's names are AMD-internal. Its `GODAVARI` rows cover 9850–985F, the IDs Linux lists as Mullins (and Beema).

The CPUs matter as much as the GPUs: Kaveri has Steamroller cores (family 15h, like CarrizoRed's Excavator), and
Kabini/Mullins have Jaguar/Puma cores (family 16h). Neither has AVX2, so:
- macOS uses the plain `dyld_shared_cache_x86_64`, not the `x86_64h` cache that CarrizoRed's user-space page
  patches were written against. Every user-space pattern has to be found again in the `x86_64` cache.
- Ventura and later need [CryptexFixup](https://github.com/acidanthera/CryptexFixup), acidanthera's Lilu plugin for
  CPUs without AVX2.

## The route, and the routes not taken

| Route | Status |
|---|---|
| **Pure Lilu in CarrizoRed** on Apple's Polaris drivers (`AMD9500Controller`, `AMDRadeonX4000`), with GCN 2 differences patched in | **Chosen.** Works on any macOS CarrizoRed supports, with nothing installed but kexts |
| Apple's own GCN 2 drivers (`AMD8000Controller` and the CI classes in `AMDRadeonX4000`) on Monterey 12.7.6, the last release with them, behind a Bonaire `device-id` spoof | Researched in the sister project KaberiRed (see below). Monterey only |
| Monterey's GCN 2 kexts on Ventura and later, injected by OpenCore | **Not possible.** OpenCore injects into the Boot kernel collection, and since Big Sur `IOGraphicsFamily`, `IOAcceleratorFamily2`, `AppleGraphicsDeviceControl` and `AMDSupport` exist only in the System collection (checked in Big Sur's and Tahoe's KCs). OpenCore's System-collection injection ([OpenCorePkg #385](https://github.com/acidanthera/OpenCorePkg/pull/385)) was never finished |
| Monterey's GCN 2 kexts installed into `/Library/Extensions` (Auxiliary KC) | Technically close (measurements below), **rejected**: Apple's binaries on the user's disk, renamed bundle IDs, SIP's unsigned-kext bit off |
| OpenCore Legacy Patcher's legacy-GCN root patches | Rejected: root patching the system volume |

## What Sequoia still contains

### CAIL: present, with GCN 2 support

Every HWLibs plug-in of `AMDRadeonX4000HWServices.kext` (`AMDRadeonX4000HWLibs`, `…4100…`, `…4400…`, `…4700…`) is built
from the same CAIL source and still knows these chips:

- **Device table** (`_CAILAsicCapsInitTable`; rows of family, device ID, revisions, caps, golden settings): family
  `0x7D` (KV, the GCN 2 APUs) with `SPECTRE_A0` (1304–1318), `SPECTRE_A0_E` (131B–131D), `KALINDI_A0/A1` (9830–983D),
  `KALINDI_A0_E/A1_E`, `GODAVARI_A0` (9850–985F) and `GODAVARI_NO_EYEFINITY_A0` (9854–9856). Family `0x78` (CI) and
  `0x6E` (SI) are there too.
- **Init code:** `_CAILInitFunctionPointer` dispatches to `_Cail_Bonaire_InitFunctionPointer`, then to
  `_Cail_Spectre_…`, `_Cail_Kalindi_…` and (new finding) **`_Cail_Godavari_InitFunctionPointer`**. Godavari has its
  own init table, not only SMU helpers. `_BonaireAsicSetupTable` and `_Cail_Bonaire_LoadUcode` are present in
  Sequoia's `AMDRadeonX4000HWLibs`.
  KaberiRed mapped how this composes in Monterey's build: on Kabini, CAIL runs the Bonaire implementation (97
  function pointers) with 15 Spectre and 3 Kalindi overrides. Sequoia's dispatch is the same.
- **Setup data:** `_SPECTRE_GoldenSettings_A0_8812(_E)`, `_aSpectre_GoldenRegisterSettings`,
  `_Spectre_HwConstants_8812(_E)`; `_KALINDI_GoldenSettings_A0_4882(_E)`, `_aKalindi_GoldenRegisterSettings`,
  `_Kalindi_HwConstants_4882(_E)`.
- **Power and SMU helpers:** `_Cail_Spectre_InitializePowerGating`, `_Spectre_Disable…PowerGating`,
  `_Cail_Spectre_GetSmuFwVersion`, `_Cail_Kalindi_SetSmuDfsBypassMode`, `_Godavari_FormatSmuDramDataBuffer`, the KV
  SMU buffer helpers, and OEM groups (`_KaveriMobileGroup1–3`, `_KaveriDesktopGroup`, `_KaveriAIOGroup1–2`,
  `_KaveriEmbeddedGroup`).
- **Firmware, embedded in the binary.** Each `*_UCODE` symbol is a descriptor (size in dwords, version string, version
  number, data pointer), and the data is real:

| Firmware | Kaveri (`Spectre_*`) | Kabini/Mullins (`Kalindi_*`) |
|---|---|---|
| CE (constant engine) | 8576 B, #122 | 8576 B, #122 |
| PFP (prefetch parser) | 8576 B, #229 | 8576 B, #229 |
| ME (micro engine) | 8576 B, #187 | 8576 B, #187 |
| MEC1 (compute) | 16768 B, #421 | 16768 B, #421 |
| MEC2 | 16768 B, #421 | none (Kabini has one MEC) |
| RLC | 10240 B, #57 | 10240 B, #43 |
| SDMA0 / SDMA1 | 4200 B each, #76 | 4200 B each, #76 |

The dword counts are exactly what Linux's `radeon` driver expects (`CIK_CE/PFP/ME_UCODE_SIZE` 2144,
`CIK_MEC_UCODE_SIZE` 4192, `KV_RLC_UCODE_SIZE`/`KB_RLC_UCODE_SIZE` 2560, `CIK_SDMA_UCODE_SIZE` 1050). No firmware has
to come from outside. GCN 2 loads it directly through the `*_UCODE_ADDR/DATA` registers: **CarrizoRed's SMU8
microcode loader must stay off** for these chips.

CAIL reads the real device ID from PCI config space (`AmdCailServices::AmdCailServices` →
`extendedConfigRead16(2)`), so with a Polaris `device-id` spoof it still selects the Spectre/Kalindi/Godavari paths,
as it selects Stoney today.

### Metal: the GFX7 compiler is still there

macOS's Metal compiler for GCN is `libSC.dylib` (inside `AMDShared.bundle`, in the dyld shared cache), and the GCN
Metal driver is `AMDMTLBronzeDriver`.

- **The Bonaire (GFX7) back end is in every release checked:** `SCBonaireInfo`, `SCBonaireEmitter`,
  `SCBonaireTransform` and `SCHawaiiInfo` are in the Monterey, Ventura, Sonoma, Sequoia and Tahoe caches.
- **Sequoia 15.8.1's `SCGetInstructionSet(family, revision, device)`** (cache `.03`, `0x7ffb00eaed40`) switches on
  `family − 0x6E`:

| Family | Returns |
|---|---|
| `0x6E` (SI) | `0x0B` (Tahiti) |
| `0x78` (CI) | `0x0D` (Hawaii) for revision `0x28`–`0x3B` with device `0x67A0`/`0x67A1`, else **`0x0C` (Bonaire)** |
| **`0x7D` (KV: Kaveri, Kabini, Mullins)** | **`0x0C` (Bonaire), any revision** |
| `0x82` (VI) | `0x0E` (generic GFX8), or Tonga/Polaris targets by revision |
| `0x87` (CZ) | `0x1E` (`SCGfx81Info`) for revision `0x61`+ (Stoney), else `0x0E` |
| `0x8D`, `0x8E`, `0x8F`, `0x90`, `0x92`, `0x95` | Vega, Raven, Navi targets |

- **`AMDMTLBronzeDriver` still has its CI chip records**: `isBonaire`, `isHawaii`, `isUnknownCI`,
  `isSeaIslandsFamily`, "CI Unknown". It gets the chip from the kernel's hardware-info call (device and revision IDs,
  pipe counts, active CUs, render-backend mask, `GB_ADDR_CONFIG`, `PA_SC_RASTER_CONFIG`, the tile-mode tables).

How to use it:
- **The Metal driver must take its CI path, not just the compiler.** The GFX register headers show 220 registers at
  the same offset with different fields, led by the colour-buffer state (`CB_COLOR*_INFO`). A VI-path Metal driver
  would program GFX8 field layouts into GFX7 hardware. So the kernel should report a Bonaire device ID and family
  `0x78` (or KV `0x7D`) to user space. Then the CI path and the Bonaire back end follow without patching the compiler.
  CarrizoRed already controls the device ID that the kernel's hardware-info call reports.
- **Fallback:** CarrizoRed's existing page patch of `SCGetInstructionSet` already rewrites the VI branch's return value
  (to `0x1E` for Stoney), and `0x0C` would select Bonaire. This only covers the compiler, not the CI paths above.
- **The 16-bank LDS hazard (Kabini/Mullins only).** Kabini/Mullins (gfx703) have 16 LDS banks, where
  `v_interp_p1_f32` must not write the VGPR holding its I barycentric. The Bonaire back end assumes 32 banks:
  KaberiRed found the hazard in 33 of the 435 precompiled GCN programs in Monterey's `AMDMTLBronzeDriver`. So
  CarrizoRed's interpolation fixer (`-crnointerpfix`) has to learn GFX7 encodings: VINTRP is `0b110010` on GFX6/7
  and `0b110101` on GFX8, and SMRD and the VOP opcodes differ too. Kaveri (gfx700) has 32 banks and is unaffected.
- `ATIRadeonX4000SCLib` (OpenGL's compiler glue) still names `bonaire` and `hawaii`.

## Acceleration: Sequoia's `AMDRadeonX4000` on GCN 2 hardware

Sequoia's accelerator only has a GFX8 hardware layer (`AMDVIHardware` → `AMDBaffinHardware` /
`AMDEllesmereHardware`, plus the VI PM4, sDMA, VM manager and interrupt classes). It has no `AMDCIHardware`. As for
Stoney, the APU is spoofed as a Polaris (`0x67EF`), and every GFX8 assumption the GCN 2 hardware doesn't share gets
patched.

Register offsets, GCN 2 vs Polaris (Linux `asic_reg` headers: `gca/gfx_7_0` vs `gfx_8_0`, `gmc/gmc_7_1` vs `gmc_8_1`,
`oss/oss_2_0` vs `oss_3_0`, `bif/bif_4_1` vs `bif_5_0`). "Collisions" are GFX8 offsets that land on a *different*
GCN 2 register, i.e. writes that would hit the wrong register unless remapped or dropped:

| Block | Common registers | Same offset | Moved | Collisions | Same offset, fields differ | What moved |
|---|---|---|---|---|---|---|
| GFX (CP, RLC, shaders, context) | 2312 | **2203 (95%)** | 109 | 25 | 220 | RLC 83 (firmware/save-restore, CAIL's job), CP 11 (microcode ports to `0xF818`+ on GFX8; compute queue HQD registers), SQ 12 |
| GMC (memory controller, VM) | 885 | 848 | 37 | 61 | 84 | `MC_HUB_RDREQ_*` client registers (37); collisions in `MC_ARB_*` and `MC_HUB` |
| OSS (interrupts, SDMA, SRBM) | 350 | 307 | 43 | 6 | 50 | **IH block (28): `IH_*` at `0xE3x` on Polaris → `0xF8x` on GCN 2**; SRBM 9; 3 per SDMA engine |
| BIF (bus interface) | 286 | 285 | 1 | 32 | 17 | collisions are GFX8-only doorbell/IOV registers |

What that means for the patch list:

| Area | GCN 2 vs Polaris/Stoney | Job |
|---|---|---|
| ASIC init, golden registers, firmware | CAIL's own Spectre/Kalindi/Godavari paths | Keep CarrizoRed's SMU8 loader off; check the PCIe link-speed, ASPM and ATS steps of `BonaireAsicSetupTable` (Linux skips them on IGPs; CarrizoRed already skips ATS on Stoney) |
| Interrupt handler | IH registers moved `0xE30` → `0xF80` (28 registers). Enabling works as on Carrizo (`IH_CNTL.ENABLE_INTR` plus `IH_RB_CNTL.RB_ENABLE`, Linux `cik_ih.c`) | Remap the IH offsets, then CarrizoRed's existing IH enable fix applies |
| SDMA | SDMA 2 (CIK) vs SDMA 3: same packet opcodes (Linux `cik_sdma.c`), 3 registers moved per engine | Remap; Kabini has **two** SDMA engines like Polaris, so CarrizoRed's single-SDMA patch for Stoney must stay off |
| Memory / VM | GMC 7: same VM registers as GMC 8 at the offsets that matter; UMA carve-out like Stoney | CarrizoRed's VRAM-window-at-0 and page-table fixes carry over; skip GDDR5/MC set-up (`MC_ARB_*` collisions) |
| CP / compute | Graphics ring registers unchanged; compute queue (HQD) registers partly reshuffled (25 collisions) | Remap or keep compute queues off at first |
| Shape | Kabini/Mullins: 1 shader engine, 2 CUs, 1 render backend, 2 tile pipes (`ADDR_SURF_P2`), raster config 0. Kaveri: 1 SE, 8 CUs, 2 RBs, 4 pipes, `RB_MAP_PKR0(2)`, 2 MECs | Report CAIL's values to the kernel layer and to Metal (as for Stoney) |
| Power | `kv_dpm`: messages to the SMU inside the APU, different from Stoney's SMU8 messages | PowerPlay's dummy back end first, then `kv_dpm` messages if worth it |
| Video | UVD 4.2 / VCE 2.0, physical addressing only; Sequoia's accelerator has only UVD 6 code, but still Apple's CI VCE classes and (Ventura, checked) a CIK VCE firmware | VCE: remap and address translation. UVD: an engine layer of our own. See *Video* below |

## Video: UVD 4.2 and VCE 2.0

Researched 2026-11-06. Apple's side was read in Monterey 12.5's and Ventura 13.5.2's `AMDRadeonX4000` (the copies
in OpenCore Legacy Patcher's PatcherSupportPkg, which keep class symbols). **Sequoia is not checked yet**:
`scripts/video_gcn2_check.py` runs the same checks on the KDK's binaries.

### The hardware (Linux)

- Every GCN 2 APU has UVD 4.2 and VCE 2.0. **The firmware is the same file for all of them**: linux-firmware's
  `bonaire`, `hawaii`, `kaveri`, `kabini` and `mullins` `_uvd.bin` are byte-identical (232,752 B), and so are the
  `_vce.bin` (101,072 B, `VCEFW 50.5.2`).
- Decode: MPEG-2, MPEG-4, H.264 and VC-1, up to 2048×1152 (`cik_video_codecs_decode`). No HEVC. Encode: H.264, up to
  2048×1152.
- **UVD 4.2 vs UVD 6.0 registers** (`uvd_4_2_d.h` vs `uvd_6_0_d.h`): of 58 common registers, 54 are at the same
  offset. `UVD_PGFSM_CONFIG`, `PGFSM_READ_TILE1/2` and `UVD_POWER_STATUS` moved (`0x38F8`+ → `0x38C0`+). UVD 6 adds
  the 64-bit address registers (`LMI_RBC_RB/IB_64BIT_BAR_*`, `LMI_VCPU_CACHE_64BIT_BAR_*`), the VMID registers and the
  encode rings. Two offsets **mean different things**:

| Offset | UVD 4.2 | UVD 6.0 |
|---|---|---|
| `0x3DA1` | `UVD_RBC_IB_BASE` | `UVD_LMI_RBC_IB_VMID` |
| `0x3DA3` | `UVD_RBC_RB_BASE` | `UVD_LMI_RBC_RB_VMID` |

- **UVD 4.2 addressing is 32-bit plus extensions**: bits 28–31 in `UVD_LMI_ADDR_EXT`, bits 32–39 in
  `UVD_LMI_EXT40_ADDR` (`uvd_v4_2_mc_resume`); Linux allows 64-bit addresses only from UVD 5.0
  (`adev->uvd.address_64_bit`). So, on top of physical mode (as Stoney's UVD, [VIDEO.md](VIDEO.md) stage 7):
  - no buffer may cross a 256 MB boundary;
  - message and feedback buffers must be in the same 256 MB segment as the firmware (VCPU) buffer
    (`amdgpu_uvd_cs_pass2`; Linux forces them there, `amdgpu_uvd_force_into_vcpu_segment`).
- **VCE 2.0 vs VCE 3.0** (`vce_2_0_d.h` vs `vce_3_0_d.h`): 39 common registers, 25 at the same offset, including the
  ring registers (`VCE_RB_BASE_LO/HI`, `VCE_RB_SIZE` at `0x8060`–`0x8062`, the LLQ ring at `0x805B`–`0x805D`). The
  LMI block moved by `+0x80` (`VCE_LMI_CTRL` `0x8526` → `0x85A6`, …) and so did `VCE_SYS_INT_*`. VCE 2.0 has one
  firmware cache BAR (`VCE_LMI_VCPU_CACHE_40BIT_BAR`); VCE 3 programs three.
- **VCE addressing:** VCE 2.0 runs in physical mode (`amdgpu_vce_ring_parse_cs`: addresses rewritten, no buffer may
  cross a 4 GB boundary). That's a much looser rule than UVD 4.2's.

### What Apple's accelerator has

| | Monterey 12.5 | Ventura 13.5.2 |
|---|---|---|
| UVD hardware classes | `AMDCIUVDHWEngine` → `AMDSIUVDHWEngine`, with SI's ring and channel | VI only (`AMDVIUVDHWEngine`, `AMDUVD6v3HWEngine`) |
| VCE hardware classes | `AMDCIVCEHWEngine`, `AMDCIVCERing`, `AMDCIVCEChannel` (+ LLQ) | **the same CI classes**: VI's derive from them |
| Multimedia layer (firmware, messages) | `AMDUVDTrinity`, `AMDUVDCIK`, `AMDUVDVI`, `AMDUVD6v3`; `AMDSIVCE`, `AMDCIKVCE`, `AMDVIVCE`, `AMDVCE3v4` | the same eight |
| CIK VCE firmware | plain, in the accelerator | plain, in the accelerator (below) |
| CI hardware (`AMDCIHardware`, `AMDBonaireHardware`) | yes | no |

- **UVD: Ventura's code programs UVD 6.** The UVD IB command written by `AMDVIUVDChannel` and
  `AMDUVD6v3DecChannel::initializeIndirectBufferCmd` sets `0x3DA1` (the VMID), `0x3C67`/`0x3C66` (64-bit address)
  and `0x3DA2` (size). On UVD 4.2 that puts the VMID into `UVD_RBC_IB_BASE` and the address into registers that
  don't exist. The ring set-up (`AMDVIUVDRing::init`) is the same: `0x3C68`/`0x3C69` and `0x3DA3`.
  Monterey's SI channel shows the UVD 3/4 version: `UVD_LMI_EXT40_ADDR` (`0x3D26`), `UVD_RBC_IB_BASE`,
  `UVD_RBC_IB_SIZE`, and the ring at `UVD_RBC_RB_BASE` plus `EXT40`. Ventura removed that code with the SI/CI
  hardware classes.
- **VCE: the CI engine is the one Polaris uses.** `AMDVIVCEHWEngine` derives from `AMDCIVCEHWEngine` and doesn't
  override `initHardware`, `configCacheWindows`, `initHardwareRings` or the clock functions. `AMDVIVCEChannel`
  derives from `AMDCIVCEChannel`. `AMDCIVCERing` writes the ring registers that VCE 2 and 3 share;
  `AMDVIVCERing` / `AMDVCE3v4Ring` also write `0xC200` (`GRBM_GFX_INDEX`, VCE instance select).
- **The CIK VCE firmware is embedded** (Ventura, `AMDCIKVCE`'s image): file offset `0x26B770`, 103,760 B, tagged
  `[ATI LIB=VCEFW,52.1.2]` at `+0x16AF0`. Linux's `kaveri_vce.bin` has the same layout (tag at `+0x16AB0`), and
  211 of its 1,268 64-byte chunks occur in Apple's image: a newer build of the same firmware. The other two images
  are `AMDVIVCE`'s (177,344 B, Carrizo-like) and `AMDVCE3v4`'s (166,752 B, Polaris).
- **No plain UVD firmware for any chip**, as known from Sequoia (CAIL decrypts UVD firmware). Whether Sequoia's
  HWLibs still carries a CIK UVD image is open. Linux's `kaveri_uvd.bin` works for every GCN 2 APU, and
  `LICENSE.amdgpu` allows shipping it.

### What that means

- **VCE (H.264 encode) is the nearer target.** Firmware, engine, ring and channel classes are Apple's. To do:
  - the multimedia layer picks `AMDCIKVCE` for the chip;
  - the LMI registers are remapped (`+0x80`), with one cache BAR instead of three (CAIL or `configCacheWindows`, to
    check);
  - every address in each VCE IB is rewritten to a memory-controller address (physical mode, 4 GB rule). It's the
    same translation as Stoney's UVD stage 7, with a simpler rule set.
- **UVD (decode) needs an engine layer of our own.** Apple's code is UVD 6 only, so:
  - rewrite the ring set-up and every IB command into the UVD 4.2 form (`EXT40` + `RBC_IB_BASE` + `RBC_IB_SIZE`);
  - program the firmware cache windows with `LMI_ADDR_EXT`/`EXT40`;
  - remap `PGFSM`/`POWER_STATUS`;
  - translate every message address to physical (as on Stoney);
  - keep all UVD buffers inside 256 MB segments, with messages and feedback in the firmware's segment. With the
    carve-out moved to GPU address 0, its first 256 MB is the natural window; user-space bitstreams need bounce
    buffers there;
  - supply the firmware, Apple's (if Sequoia's CAIL still has it) or Linux's.
  Stoney's stage 7 address translation comes first: most of it carries over.
- **A hybrid shader decoder is relatively more attractive here than on Stoney**, for HEVC (no hardware decode at
  all on GCN 2) on Kaveri-class chips. Kabini and Mullins (2 CUs) are too small for it.

### To check on Sequoia

`python scripts/video_gcn2_check.py --accel <KDK>/AMDRadeonX4000 --hwlibs <KDK>/AMDRadeonX4000HWLibs --fw kaveri_vce.bin kaveri_uvd.bin`:
- do `AMDCIVCE*`, `AMDCIKVCE` and `AMDUVDCIK` and the `52.1.2` image survive;
- does CAIL have UVD/VCE code for the KV family (Spectre/Kalindi/Godavari), or only generic `CailUvdInit` /
  `CailVceInit`. That decides whether CAIL can bring the engines up, or CarrizoRed does it as Linux's
  `uvd_v4_2_start` / `vce_v2_0_start`.

## Display: `AMD9500Controller` (DCE 11.2) on DCE 8

Sequoia ships one AMD display controller, `AMD9500Controller` (Polaris, DCE 11.2). CarrizoRed already bends it onto
DCE 11.0 with a register wrapper ([DCE11_REGISTER_DIFF.md](DCE11_REGISTER_DIFF.md)). An earlier version of this page
called the same for DCE 8 "unlikely to be practical". The register headers say otherwise for the parts that produce a
picture.

Offsets of pipe 0's registers, DCE 8.0 vs DCE 11.2 (Linux `asic_reg/dce/dce_8_0` vs `dce_11_2`):

| Block | DCE 11.2 registers | Also in DCE 8.0 | Same offset | Moved | Fields differ |
|---|---|---|---|---|---|
| CRTC (timing) | 214 | 207 | **205** | 2 | 39 |
| GRPH/DCP (surface, cursor, gamma) | 200 | 182 | **182** | 0 | 6 |
| LB (line buffer) | 66 | 66 | **66** | 0 | 3 |
| SCL (scaler) | 56 | 56 | **56** | 0 | 2 |
| DMIF / watermarks / DCFE | 187 | 94 | 25 | 69 | 4 |
| DIG (encoders) | 36 | 25 | 0 | 25 | 5 |
| DP (link) | 246 | 220 | 12 | 208 | 6 |

- **The CRTC field differences are widths.** DCE 11.2 widened the timing fields (for example `CRTC_H_TOTAL`,
  `CRTC_V_TOTAL`, sync and blank start/end from 13 to 14–15 bits). Values for normal modes are unchanged. A few
  DCE 11.2-only bits were added (`CRTC_CONTROL`, static screen, stereo).
- **Pipe strides differ.** For pipe n's register block (first offsets of `CRTC_H_TOTAL`, `GRPH_PRIMARY_SURFACE_ADDRESS`,
  `LB_DATA_FORMAT`):

| Pipe | 0 | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|---|
| DCE 11.2 offset | +0 | +0x200 | +0x400 | +0x2600 | +0x2800 | +0x2A00 |
| DCE 8.0 offset | +0 | **+0x300** | **+0x2600** | **+0x2900** | +0x2C00 | +0x2F00 |

  Kabini/Mullins only have pipes 0–1, so only +0x200 → +0x300 matters, and pipe limit 2. Kaveri has 4 pipes.
  CarrizoRed's wrapper already limits the pipe count (`displayPipes`, `CFG_FB_LIMIT`).
- **Whole-block remaps:** DMIF/watermark arbitration, DIG encoder front ends and DP link registers moved. DP matters
  first: the A4-5000's panel is eDP, and CarrizoRed's eDP work (ASSR) touches that code. Watermarks also depend on
  clocks from `kv_dpm` instead of GDDR5; wrong values give underflow (flicker), not a black screen.
- **Mode setting through the VBIOS still works.** Apple's `AtiSetPixelClockInterface_V1` accepts SetPixelClock
  revision 6, which Kabini's VBIOS uses ([APPLE_KEXTS.md](APPLE_KEXTS.md), pixel clock path). Encoder and transmitter
  control go through the VBIOS's own tables.
- **PLLs:** Kabini/Mullins have only PPLL1 and PPLL2; Kaveri has PPLL0–2 (Linux `dce_v8_0_pick_pll`). Check which
  PLL ids `AMD9500Controller` hands out, as CarrizoRed did for Stoney.
- **LVDS:** the A6-5200 test laptop has an LVDS panel. Polaris has no LVDS output, so `AMD9500Controller` probably
  has no path for it. The A4-5000 (eDP) comes first.
- **Fallback:** the firmware (GOP) framebuffer works without any of this: fixed resolution, no brightness. Whether
  Apple's accelerator works without Apple's AMD display controller is untested.

## Monterey's GCN 2 kexts on Sequoia (measured, route rejected)

Recorded in case the Auxiliary-KC route is ever reconsidered. Monterey 12.7.6's binaries (fetched from Apple's
installer with KaberiRed's `scripts/fetch_macos.py`) against Sequoia 15.8.1's kernel and families (KDK 24H32):
- **Imports:** `AMDSupport` 503/503, `AMDRadeonX4000HWServices` 301/301, `AMDRadeonX4050HWLibs` 315/315,
  `AMD8000Controller` 837/838 and `AMDRadeonX4000` 1226/1228 resolve. The 3 misses are stale references that are
  also unresolved on Monterey itself: `CISharedController::statusSMURunning`, and
  `IOGraphicsAccelerator2::createDrawable(IOAccelLegacySurface…)` (not in Monterey's `IOAcceleratorFamily2`
  either). The third is `IOAccelMemoryMap`'s reserved slot 0.
- **Class layout:** all 24 `IOAcceleratorFamily2` classes that Monterey's `AMDRadeonX4000` subclasses have the same
  instance size on Sequoia. Their vtables match slot for slot, except `IOAccelMemoryMap` slot 50: reserved on
  Monterey, `getAssignedGPUVirtualAddressLength` on Sequoia.
- **`AMD8000Controller` needs Monterey's `AMDSupport`:** Sequoia's `ATIController` grew from 106,912 to 201,768 bytes.
  The other 21 `AMDSupport` classes it subclasses are unchanged.
- **AMD user client** (`AMDAccelSharedUserClient`, selectors `0x100`–`0x117`): Sequoia dropped `ForceLegacyMode`
  (`0x102`) and added `LockVMIDForCapture`, so `0x102`–`0x10D` shift by one, and three output sizes changed. The ten
  `Qk*` submission and memory methods (`0x10E`–`0x117`) are identical.

## Test machines and the boot blocker

KaberiRed's two Kabini laptops (HP A4-5000 1002:9832 eDP; HP 15-f008cl A6-5200 1002:9830 LVDS, VFCT present) don't
boot macOS yet, with or without any GPU driver. The kernel stops about 2–4 s in. KaberiRed's platform probe ruled
out:
- interrupt storms;
- EC events (the battery is dead, a first suspect);
- NVRAM writes;
- the HPET (`SSDT-XOSI` turns it on, no change);
- idle `hlt`;
- SMM time.

What fits every run: one CPU stuck with interrupts off, so timers queued on it never fire. The next test is
`krprobepanic=30`, a deliberate panic, so XNU names the CPUs that don't answer. Other findings from those installs:
`HfsPlus.efi` needs RDRAND, which Jaguar lacks (use `HfsPlusLegacy.efi`), and ECEnabler is required on those DSDTs.
Details are in KaberiRed's `docs/TEST_MACHINE.md`.

**A Kaveri machine is the shorter path to a first picture.** An FM2+ desktop (A8-7600, A10-7850K) is the best fit:
- Steamroller is family 15h, the same AMD_Vanilla path as CarrizoRed's working Excavator machines;
- 32 LDS banks, so no interpolation fixer;
- 4 tile pipes and 2 render backends, closer to what Metal already handles;
- a PCIe slot for a second GPU or a serial card while debugging.

## Plan

1. **Hardware that boots.** Either Kabini past the freeze (`krprobepanic`), or a Kaveri machine. Collect the Linux
   dump (`scripts/collect_linux_info.sh`) and boot macOS on the firmware framebuffer, without CarrizoRed.
2. **Detection and observation.** Teach CarrizoRed the GCN 2 IDs as their own family (pipe limits, no SMU8 loader, no
   single-SDMA patch, no RB+ fix). First boot with `-crnoaccel` and the black box: VBIOS from VFCT or the legacy ROM
   (KaberiRed has the fallback), carve-out, `GB_ADDR_CONFIG`.
3. **Display.** Extend the DCE register wrapper with a DCE 8 table: pipe strides, then DMIF/watermark, DIG and DP
   remaps. Generate it from the Linux headers as `scripts/dce_regdiff.py` does for DCE 11.0. Milestone: native
   resolution on the internal panel.
4. **Accelerator.** Remap the IH, SDMA, GMC-client and compute-queue registers; let CAIL run its own path; VRAM
   window and page tables as on Stoney. Milestone: rings up, interrupts delivered.
5. **Metal.** Report a CI chip to user space so `AMDMTLBronzeDriver` takes its CI path and libSC its Bonaire back
   end. Find the user-space patterns again in the `x86_64` cache. Kabini/Mullins: port the interpolation fixer to GFX7
   encodings. Milestone: WindowServer composites on the GPU.
6. **Later:** `kv_dpm` clock control, sleep, Ventura and later (with CryptexFixup), video: VCE (H.264 encode)
   first, then UVD 4.2 (see *Video*).

## Relation to KaberiRed

KaberiRed (`Desktop/KaberiRed`, the Monterey/`AMD8000Controller` route) stays the reference for:
- the hardware differences, Bonaire vs Kabini vs Kaveri: `docs/KABINI_VS_BONAIRE.md`;
- the Monterey binaries and how CAIL's KV path composes: `docs/APPLE_KEXTS.md`;
- the test laptops and their install log: `docs/TEST_MACHINE.md`;
- its tools: `scripts/fetch_macos.py` (files from Apple's installer, no KDK), `scripts/build_efi_hp.py` (OpenCore
  for the HP laptops), and the platform probe.

Code for GCN 2 support goes into CarrizoRed.

## Open questions

- Is `AMDMTLBronzeDriver`'s CI path still functional in Sequoia, or only its chip table? And does it run correctly
  against Sequoia's VI-based kernel accelerator?
- Do CAIL's Spectre/Kalindi/Godavari paths run cleanly on real hardware? They may not have been exercised since macOS
  dropped these chips.
- Does Apple's accelerator need Apple's AMD display controller attached, or could it run alongside the firmware
  framebuffer?
- Which PLL ids does `AMD9500Controller` use, and does it have any LVDS path?
- What does the Kabini boot freeze turn out to be?

## How this was checked

- **CAIL, the accelerator and the display controller:** `AMDRadeonX4000HWLibs` (and the 4100/4400/4700 variants),
  `AMDRadeonX4000`, `AMDSupport` and `AMD9500Controller` from the macOS 15.8.1 KDK (24H32), read on Windows with
  `scripts/extract_kdk.py` and KaberiRed's `scripts/macho.py`. Firmware sizes come from each `*_UCODE` descriptor's
  dword count.
- **The compiler:**
  - `SCGetInstructionSet` was disassembled from Sequoia's `libSC.dylib`, and its jump table decoded per family.
  - The back-end class names were searched in the Monterey, Ventura, Sonoma, Sequoia and Tahoe `x86_64h` caches. The
    `x86_64` caches that Kaveri/Kabini use weren't checked.
- **Register comparisons:** generated from Linux master's `drivers/gpu/drm/amd/include/asic_reg` headers (MIT), with
  `scripts/dce_regdiff.py` for DCE and the same method for GFX/GMC/OSS/BIF. Pipe 0 is compared with instance prefixes
  folded.
- **Monterey kexts on Sequoia:**
  - Monterey 12.7.6 (21H1320) binaries came from Apple's installer.
  - Each kext's undefined symbols were resolved against Sequoia's kernel and family kexts. Symbols that Sequoia's own
    AMD kexts import too count as resolvable (they come through the kernel's symbol sets).
  - Class sizes come from each `OSMetaClass` constructor's size argument.
  - Vtables were compared slot by slot by symbol name.
  - The user-client tables were read through `getTargetAndMethodForIndex`.
- **Kernel-collection membership:** bundle identifiers searched in Big Sur's and Tahoe's Boot and System KCs.
