# Apple's GCN 2 drivers in macOS Monterey

Everything here was checked in **macOS 12.7.6 (21H1320)**, the last Monterey release, read on Windows.

## Getting the binaries without a Mac or a KDK

Dortania's KDK mirror starts at Ventura (13.0): there is no Monterey Kernel Debug Kit to download without an Apple
developer login. It isn't needed: Monterey still ships every kext binary on disk, and Apple's public software update
catalog has the full installer. `scripts/fetch_macos.py` reads files straight out of it with HTTP range requests:

```sh
python scripts/fetch_macos.py --catalog                       # installers Apple currently lists
python scripts/fetch_macos.py --build 21H1320 --list -p System/Library/Extensions/AMD
python scripts/fetch_macos.py --build 21H1320 -o ../KaberiRed-RE/Monterey-12.7.6-21H1320 \
    -p System/Library/Extensions/AMD8000Controller.kext -p System/Library/Extensions/AMDSupport.kext ...
```

How it works: `InstallAssistant.pkg` (XAR) stores `SharedSupport.dmg` uncompressed; the DMG (UDIF, raw blocks) holds an
HFS+ volume with the update ZIP; the ZIP's `AssetData/payloadv2/payload.000`–`.035` are pbzx streams (independent 8 MB
xz blocks) of Apple Archive (`YAA1`) entries. Each chunk is a byte-sorted run of paths (files are grouped into chunks
by size), so a path is found by binary search over a chunk's blocks; `fixup.manifest` lists every path and is used to
check that nothing was missed. A set of kexts costs a few hundred MB of a 12.4 GB installer, and blocks are cached.

**Never commit these binaries.** They're Apple's copyright: keep them outside the repo (`Desktop/KaberiRed-RE/`).

## User space: the Metal and OpenGL drivers

`AMDMTLBronzeDriver.bundle` (the GCN Metal driver) and `AMDShared.bundle` (with `libSC.dylib`, AMD's shader compiler,
and `libGCNASM.dylib`) have only an `Info.plist` on disk: their code lives in the **dyld shared cache**. On Kabini and
Mullins (Jaguar/Puma cores, no AVX2/BMI2/FMA) macOS uses the plain **`dyld_shared_cache_x86_64`**, not `x86_64h`:
user-space patterns must come from that cache. In 12.7.6 it's split into a main file and four subcaches; all of the AMD
user-space code is in **`dyld_shared_cache_x86_64.2`** (subcache at shared-region offset `0x200000000`):

| Image | Address (unslid) |
|---|---|
| `AMDShared.bundle/Contents/PlugIns/libSC.dylib` | `0x7ffa0c888000` |
| `AMDShared.bundle/Contents/PlugIns/libGCNASM.dylib` | `0x7ffa0f10c000` |
| `AMDShared.bundle/Contents/MacOS/AMDShared` | `0x7ffa10626000` |
| `AMDMTLBronzeDriver.bundle/Contents/MacOS/AMDMTLBronzeDriver` | `0x7ffa1c8dd000` |
| `AMDRadeonX4000GLDriver.bundle/Contents/MacOS/ATIRadeonX4000SCLib.dylib` | `0x7ffa1ccc5000` |

(`AMDRadeonX4000GLDriver` itself, the OpenGL driver, is on disk.)

### Is there a GFX7 code generator? Yes.

- **`libSC`** has GCN 2 back ends: `SCBonaireInfo`, `SCHawaiiInfo`, `SCBonaireEmitter`, `SCBonaireTransform` (next
  to `SCTahitiInfo` for GCN 1, `SCIcelandInfo`/`SCTongaInfo`/`SCGfx81Info` for GCN 3, and GFX9/10). There is no Kalindi
  or Spectre variant.
- **`SCGetInstructionSet(family, revision, deviceId)`** (`0x7ffa0d13ed30`) switches on `family − 0x6E`:
  - family `0x78` (CI): revision `0x28`–`0x3B` and device ID `0x67A0`/`0x67A1` → `0x0D` (Hawaii); otherwise `0x0C`
    (Bonaire);
  - family **`0x7D` (KV: Kaveri, Kabini, Mullins) → `0x0C` (Bonaire), any revision**;
  - `0x82` VI, `0x87` CZ (revision `0x61`+ = Stoney → `0x1E`, `SCGfx81Info`), `0x8D`/`0x8E` AI, `0x8F` RV, `0x90`/`0x92`
    NV; others unsupported.

  So whether the APU is reported as a Bonaire (family `0x78`) or as itself (`0x7D`), the compiler generates GFX7 code
  with the Bonaire back end. No compiler patch is needed to get a code generator.
- **The 16-bank LDS hazard.** In LLVM terms Bonaire (gfx704) and Kaveri (gfx700) have 32 LDS banks; Kabini/Mullins
  (gfx703) have 16, where `v_interp_p1_f32` must not write the VGPR holding its I barycentric (LLVM marks the
  destination early-clobber for gfx703 only; `GCNFeatures.td`: `FeatureISAVersion7_0_3` = `FeatureLDSBankCount16`).
  **Confirmed: Apple's GCN 1/2 code has the hazard.** `AMDMTLBronzeDriver`'s `__TEXT` embeds 435 precompiled GCN
  programs (`s_endpgm`, with VINTRP in the GFX6/7 encoding `0b110010`; GFX8 moved it to `0b110101`), and 33 of them
  end a vec4 attribute exactly as CarrizoRed saw on Stoney:

  ```
  v_interp_p1_f32 v4, v0, attr0.z
  v_interp_p2_f32 v4, v1, attr0.z
  v_interp_p1_f32 v0, v0, attr0.w     <- writes v0, its own I source: wrong on 16-bank LDS
  v_interp_p2_f32 v0, v1, attr0.w
  v_cvt_pkrtz_f16_f32 v1, v2, v3
  ```

  So on Kabini/Mullins, CarrizoRed's interpolation fixer is needed, ported to GFX7 encodings (VINTRP `0x32`, SMRD,
  GFX7 VOP opcodes). The built-in shaders could also be fixed statically with page patches; shaders compiled at run
  time need the submit-time rewrite. Kaveri (32 banks) is unaffected.
- **`AMDMTLBronzeDriver`** knows SI, CI and VI chips by name (Tahiti, Pitcairn, Verde, Bonaire, Hawaii, Saturn,
  Tonga, Fiji, Baffin, Ellesmere, Lexa, and "CI Unknown"); its chip record has `isBonaire`, `isHawaii`, `isUnknownCI`,
  `isSeaIslandsFamily` and an `isAPU` bit. It gets its hardware description from the kernel
  (`_sAMD_GET_HW_INFO_VALUES`): device and revision IDs, raster/shader pipe counts, active CUs, enabled RB mask,
  `GB_ADDR_CONFIG` (`globalGBReg1`), `PA_SC_RASTER_CONFIG`, and **the GB tile-mode and macro-tile-mode tables**
  (`gbTileModeTable[32]`, `gbMacroTileModeTable[16]`). If the kernel side reports what CAIL programmed for Kabini, Metal's
  texture layouts follow the 2-pipe hardware, not a Bonaire.
- `ATIRadeonX4000SCLib` (OpenGL's compiler glue) names `tahiti`, `pitcairn`, `capeverde`, `bonaire`, `hawaii`, `tonga`,
  `fiji`, `ellesmere`, `baffin`.

## Kernel side

All bundles are version 4.0.8. Addresses below are file offsets in the 12.7.6 binaries (unslid).

### Matching and the spoof target

| Kext | Personality / class | Matches |
|---|---|---|
| `AMD8000Controller` | `AMD8000ControllerWrangler` (`IOFramebuffer` category) and `AMD8000Controller` (`IOPropertyMatch LoadController = true`) | `0x6640 0x6641 0x6646 0x6647 0x6650 0x6651 0x665C 0x665D` (Bonaire family) and `0x67B0` (Hawaii) |
| `AMDRadeonX4000` | `AMDBonaireGraphicsAccelerator` (`LoadAccelerator = true`) | the same eight Bonaire IDs; `AMDHawaiiGraphicsAccelerator`: `0x67B0` |
| `AMDRadeonX4000HWServices` | `AMDRadeonHWServicesCI` (`LoadHWServices = true`) | the eight Bonaire IDs and `0x67B0` |

No kernel code compares against individual Bonaire IDs (an immediate-operand search of `AMD8000Controller` and
`AMDRadeonX4000` finds none), so the eight are interchangeable for the kexts. **Spoof target: `0x665C`** (Bonaire
XT), provisionally; revisit if the Metal driver's per-ID chip record treats mobile IDs (`0x6640`-`0x6647`) better. Not
Hawaii: Hawaii's shader back end assumes fast FMA and half-rate FP64 (LLVM gfx701), which Kabini doesn't have.

As on Polaris, the controller and accelerator personalities need properties that the AGDC graphics policy sets on a
real Mac (`LoadController`, `LoadHWServices`, `LoadAccelerator`, through `AtiGpuWrangler::vendor_doDeviceAttribute`),
so CarrizoRed's watcher logic (set them once the wrangler attached, re-register) carries over.

The controller's `aty_config` (`CFG_USE_AGDC`, `CFG_USE_FEDS`, `CFG_USE_FBWRKLP`, `CFG_PULSE_INT`, ...) and
`aty_properties` are Bonaire PowerPlay settings: **`PP_EnableLoadFalconSmcFirmware = 1`** (Bonaire's SMC firmware,
which an APU doesn't have) and `PP_PhmUseDummyBackEnd = 0`. Expect to force the Dummy back end, as CarrizoRed does.
There's one framebuffer personality, `ATY,Exmoor` (`CFG_FB_LIMIT` 6, `CFG_DIAG_LED`).

### AMD8000Controller (DCE 8)

The same structure as AMD9500Controller, with generic CI classes instead of per-chip ones: `AMD8000Controller`,
`ASIC_INFO__CI` (`populateDeviceInfo` `0x39080`, `populateFbLocation` `0x39300`, `populateMemoryConfig` `0x39280`),
`CISharedController`, `CITimingGenerator`, `CILineBuffer`, `CIScaler`, `CIInterruptManager`, `CIPowerPlayManager`,
`CIRegisterService`, `CIDisplayPortConnection`, `CIHdmiConnection`, ... (58 classes). All of KaberiRed's traced
start-up stages exist under the same names as in AMD9500Controller.

Constant getters in `CISharedController` (each ≥ 20 bytes, no RIP-relative prologue, so routable):

| Getter | Returns | Kabini/Mullins | Kaveri |
|---|---|---|---|
| `getDisplayLimit` (`0x1ace0`) | 6 | **2** | 4 |
| `getNumLineBuffers` (`0x1ad40`) | 6 | **2** | 4 |
| `getDigLimit` (`0x1ad20`) | 6 | 6 | 7 |
| `getNumPixelClocks` (`0x1acc0`) | 2 | 2 | 3 |
| `getFrameBufferLimit` (`0x1ad00`) | byte `this+0x19f72` | | |
| `getDpPllId` (`0x1abc0`) | `0xFF` (none) | | |
| `getFamilyId` (`0x1ac90`) | `0x78` (CI) | KV is `0x7D` | |
| `getDCEVersion` (`0x1ad60`) | `0x50` | | |

- **PLLs: probably no remap needed.** `CISharedController::initializeProjectDependentResources` creates the PLL
  assigner with **2** PLLs (`createPllAssigner(this, connectors, n, 2)`, at `0x1a9f5`), and
  `AMD8000Controller::hwProgramPpllUsingBios` (`0x490f0`) passes the index to `AtomBiosProxy::setPixelClock`
  unchanged (CRTC at connector-info `+0x2D`, PLL at `+0x2E`). Indexes 0 and 1 are ATOM `PPLL1` and `PPLL2`: exactly
  the two Kabini has (Linux `dce_v8_0_pick_pll`). Kaveri has PPLL0 too, unused by Apple. `hwMiscSetup` also sets the
  assigner's external (DP) clock from the VBIOS (`AtomBiosProxy::getExternalClock`).
- **Hard-coded 6 pipes.** `AMD8000Controller::hwMiscSetup` (`0x23a30`) loops over 6 pipes itself (not through
  `getDisplayLimit`) and writes per-pipe registers at dword `0x300 × pipe` (pipes 0-1) or `0x2600 + 0x300 × (pipe − 2)`
  (2-5): on Kabini, pipes 2-5 don't exist. Needs a guard (drop register writes to pipes the APU doesn't have), as
  CarrizoRed's register wrapper does.
- `AMD8000Controller::hwSetCrtcToPixelClock(crtc, pll, ClockParams &)` (`0x48dc0`) and
  `hwProgramPpllUsingBios` have stack-protector prologues: route their callers or use `routeChecked` (it refuses them).

### AMDSupport

The same as Sequoia's in the parts CarrizoRed relies on: `AtomBiosProxy::readAtomBios` (`0x74e30`) tries
`readEfiAtomBiosImage` (`ATY,bin_image`) first, `ATIController::TestVRAM` (`0x8120`), `ATIController::getConfigProperty`
(`0x2f840`), `AtiGpuWrangler::vendor_doDeviceAttribute` (`0x5c500`), and SetPixelClock handlers V1_V5, V1_V6, V1_V7
(DCE 8 VBIOSes use table revision 6 in Linux's `atombios_crtc.c`).

### AMDRadeonX4000 (accelerator)

A complete GCN 2 layer: `AMDCIGraphicsAccelerator` → `AMDBonaireGraphicsAccelerator` / `AMDHawaiiGraphicsAccelerator`;
`AMDCIHardware` → `AMDBonaireHardware` / `AMDHawaiiHardware`; `AMDCIPM4Engine`, `AMDCIsDMAEngine`, `AMDCIUVDHWEngine`,
`AMDCIVCEHWEngine`, `AMDCISAMUEngine`, `AMDCIKIQHWChannel`, `AMDCIHWMemory`, `AMDCIAlignManager`, CI rings and channels.
Page tables use **`AMDSIVMM`** (`getPTEValue` `0x6da56`, `getPDEValue` `0x6da3e`); bulk updates go through
`AMDHWVMContext::updateContiguousPTEsWithDMAUsingAddr` (`0x64c04`), as on Polaris.

| Function | What it assumes | On Kabini |
|---|---|---|
| `AMDBonaireHardware::setupAndInitializeHWCapabilities` (`0x8b502`) | `+0x5C` = 2 shader engines, `+0x64` = 1 SH per SE, `+0x98` = 7 CUs per SH, then the CI base class | 1 / 1 / 2 (offsets differ from Sequoia's Polaris layout) |
| `AMDCIHardware::initializeFamilyType` (`0x88ba0`) | `+0x308` = family `0x78` | KV is `0x7D`; libSC gives both the Bonaire back end |
| `AMDCIHardware::allocateHWEngines` (`0x889c2`) | PM4 `+0x3B0`, sDMA0 `+0x3B8`, **sDMA1 `+0x3C0`**, UVD `+0x3D8`, SAMU, VCE `+0x3E8` | two SDMA engines: fine |
| `AMDCIHardware::initializeSystemApertureRegs` (`0x893f8`) | `MC_VM_SYSTEM_APERTURE_LOW` = 0, `HIGH` = (VRAM size − 1) >> 12: VRAM at MC 0 | the carve-out sits elsewhere: move the FB window to 0 (CarrizoRed's fix) |
| `AMDBonaireHardware::powerUp` (`0x8b3fe`) | waits for the PM4 engine, then programs registers `0xC240`, `0xC241`, `0xC24E`, ... | to check (Bonaire-only clock or memory set-up?) |
| `AMDAccelDevice::getHardwareInfo` (`0xda30`) | reports the config-space device ID (on Stoney: the real one) | report the spoofed ID, as CarrizoRed does |

### HWServices and CAIL

- `AMDRadeonHWServicesCI::getMatchProperty` (`0x2cc0`): its only project name, `ATY,Exmoor`, and the default both give
  **`Load4050`**, so a CI GPU loads **`AMDRadeonX4050HWLibs`** (7.3 MB; X4030 is the SI build, X4000/X4100/X4200/
  X4400/X4700 the VI builds).
- **The Monterey HWLibs are stripped** (about 55 exported symbols each; Sequoia's have thousands): CAIL's functions
  and tables have to be found by content.
- **CAIL knows the APUs.** X4050's device table (rows of 7 qwords: device ID, internal revision, external revision, PCI
  revision, caps pointer, golden-settings pointer, family, as in Sequoia) has family **`0x7D`** rows for Kaveri
  `1304`-`131D`, **Kabini `9830`-`9839`, `983D`** and **Mullins `9850`-`985F`**, next to 38 CI (`0x78`) rows. Every
  HWLibs build has the same KV rows.
- **CAIL embeds the APU microcode**, most of it byte-identical to linux-firmware's `radeon/*.bin`:

| Image | In X4050 |
|---|---|
| Kabini CE v120, MEC v413, RLC v43, SDMA v76 | identical (`0x15c170`, `0x344bf0`, `0x349ca0`, `0x157e80`) |
| Kabini ME, PFP | present, different version from linux-firmware's v183/v226 (`0x15e320`, `0x159fc0`) |
| Mullins RLC v9 | identical (`0x280590`): Godavari has its own RLC |
| Kaveri RLC v57 | identical (`0x3540e0`) |
| Kaveri MEC2 v396 | not found (a different version, presumably) |

### CAIL's KV code path in the stripped X4050HWLibs

Named by comparison with Sequoia's symbolised `AMDRadeonX4000HWLibs` (same CAIL source, newer build), with
`scripts/match_functions.py`: register-immediate fingerprints, plus the order of the function pointers each
`*_InitFunctionPointer` installs. The device tables correspond row for row (Monterey's Kalindi rows point at caps
`0x16afd0`/`0x16b050` and golden settings `0x349ad0`; Godavari `0x16b390`/`0x2803c0`; Spectre `0x16af50`/`0x353c80`).

The dispatch (`CAILInitFunctionPointer`, around `0x1e640`) is the same as Sequoia's: with the CI caps (`0x125`)
enabled it calls **`Cail_Bonaire_InitFunctionPointer`** (`0x6c66`), **`Cail_Spectre_InitFunctionPointer`** (`0x42eaa`),
**`Cail_Kalindi_InitFunctionPointer`** (`0x3fc48`) and `Cail_Hawaii_InitFunctionPointer` (`0x3b99f`), each checking its
own caps. `CailCapsEnabled` is `0x12810`. The adapter structure is 8 bytes shorter than Sequoia's at the front (family
at `adapter+0x188`, external revision at `+0x194`).

So on Kabini, CAIL runs **the Bonaire implementation (97 function pointers) with 15 Spectre (KV) overrides and 3
Kalindi ones**:

| Adapter offset | Monterey | Function |
|---|---|---|
| `+0xee8` | `0x42fad` | `Spectre_SetupCgReferenceClock` |
| `+0xef0` | `0x42258` | `Cail_Spectre_MemoryConfigAndSize` |
| `+0xf00` | `0x42349` | `Cail_Spectre_GetIntegrateAsicFbMcBaseAddr` |
| `+0xf28` | `0x422b0` | `Cail_Spectre_ReserveFbMcRange` |
| `+0xed0` | `0x42fcd` | `Spectre_UpdateSwConstantForHwConfig` |
| `+0x1390` | `0x430ea` | `Spectre_CheckDsmuSupport` |
| `+0x12f8` | `0x43125` | `Spectre_GetRlcSaveRestoreRegisterListInfo` |
| `+0x1310` | `0x4316a` | `Spectre_PowerGatingControl` |
| `+0xff0` | `0x43187` | `Spectre_CheckAcpHarvested` (Kalindi: dummy `0x1db9a`) |
| `+0xee0` | `0x42d88` | `Cail_Spectre_CheckFireGL` |
| `+0x1380` | `0x42dfd` | **`Cail_Spectre_EnableAts`** (CarrizoRed had to skip it: IOMMUv2 features macOS doesn't set up) |
| `+0x1330` | `0x431d0` | `Spectre_EventNotification` |
| `+0x1320`, `+0x1328` | `0x42bc2`, `0x42cbe` | `Spectre_EnterRlcSafeMode`, `Spectre_ExitRlcSafeMode` |
| `+0x1398` | `0x3f9dc` | `Cail_Kalindi_SetSmuDfsBypassMode` |

Also matched: `Cail_Spectre_InitializePowerGating` `0x424bf`, the Spectre power-gating helpers `0x427a7`-`0x428fb`,
`Cail_Kalindi_Save/RestoreRegistersForTdr` `0x3fc91`/`0x3fd47`.

Consequences:
- **Microcode**: `Cail_Bonaire_LoadUcode` waits for an SMU load only when the Carrizo caps (`0x53`) are set; for KV it
  writes the CP/RLC/SDMA RAMs directly, as GCN 2 should. No SMU loader (CarrizoRed's biggest Stoney job) is expected.
- **ASIC set-up** is Bonaire's table (`BonaireAsicSetupTable` in Sequoia, 20 steps): golden settings, ucode buffer,
  `init_ECC`, RLC halt/reset/init/unhalt, power gating, LBPW, clock gating, nonsurface aperture, **PCIe Gen3, link
  width, ASPM, extended tags**, UVD/VCE/ACP/SAMU clocks, **`init_enable_ats`**. Linux skips the PCIe steps on IGPs and
  CarrizoRed skipped ATS: trace these on the first accelerated boot.
- **PowerPlay**: decide on hardware. Start with the Dummy back end (`PP_PhmUseDummyBackEnd`, a key this PowerPlay
  reads) and record what `PHM_Initialize` returns.

### Files (12.7.6)

From `fixup.manifest`: the AMD kexts with binaries are `AMD7000Controller` (SI), **`AMD8000Controller`** (CI, DCE 8),
`AMD9000Controller` (Tonga/Fiji), `AMD9500Controller` (Polaris), `AMD10000Controller` (Vega), `AMDSupport`,
`AMDFramebuffer`, `AMDRadeonServiceManager`, **`AMDRadeonX4000`** (GCN 1-4 accelerator) with
`AMDRadeonX4000HWServices` and eight HWLibs plug-ins (`AMDRadeonX4000HWLibs`, `X4030`, `X4050`, `X4070`, `X4100`,
`X4200`, `X4400`, `X4700`), `AMDRadeonX4000GLDriver`, `AMDRadeonX5000`/`X6000` (Vega/Navi) and their HWLibs, and the
`AMDRawCounterPlugin`.
