# Apple's AMD kexts: getting them and what's inside

## Getting the binaries (Windows, no Mac, no Apple login)

Apple's Kernel Debug Kits contain full kext bundles, executables included. Dortania mirrors every KDK publicly.

```sh
# 1. Download the KDK for the macOS build you test on (Sequoia 15.8.1 = 24H32), and check it
gh release download 24H32 -R dortania/KdkSupportPkg -p '*.dmg' -p SHA256SUM
sha256sum -c SHA256SUM

# 2. Extract the AMD kexts (needs Python 3 and 7-Zip; run from Git Bash or PowerShell)
python scripts/extract_kdk.py Kernel_Debug_Kit_15.8.1_build_24H32.dmg -o ../CarrizoRed-RE/Sequoia-15.8.1-24H32 \
    -m Extensions/AMD9500Controller.kext -m Extensions/AMDSupport.kext -m Extensions/AMDFramebuffer.kext \
    -m Extensions/AMDRadeonX4000 -m Extensions/AMDRadeonServiceManager.kext
```

[`scripts/extract_kdk.py`](../scripts/extract_kdk.py) takes a `.dmg`, `.pkg` or bare `Payload`. 7-Zip unpacks the
DMG (HFS+) and the pkg (XAR). The script then streams the `pbzx` payload (xz chunks around an odc cpio archive),
so the 3.7 GB of contents never sit in memory. Use `--list` for a dry run. Symlinks come out as `<name>.symlink`
text files, because Windows symlinks need admin rights.

> **Git Bash gotcha:** arguments starting with `/` are rewritten to Windows paths (`-m /AMDSupport.kext` becomes
> `C:/Program Files/Git/AMDSupport.kext`) and match nothing. Write match strings without a leading slash.

**Never commit these binaries.** They're Apple's copyright. Keep them outside the repo (we use
`Desktop/CarrizoRed-RE/`).

## Sequoia 15.8.1 (24H32) inventory

KDK SHA-256 `658a7905a47f6c788264b7aaac7a8f6f9e1f387cc41ae8ce227244471ef6def1`. AMD driver bundle version
7.0.25. All binaries are x86_64 Mach-O kext bundles. **No dSYMs** for the AMD kexts, but they aren't stripped
(thousands of mangled C++ symbols each), so Ghidra shows real class and method names.

| Kext | Size | Role | Matches |
|---|---:|---|---|
| `AMD9500Controller` | 1.1 MB | Display controller (DCE 11.2) | `IOPCIDevice` 0x67C0–0x67DF (Ellesmere), 0x67E0–0x67FF (Baffin) |
| `AMDSupport` | 2.4 MB | Shared display / ATOM BIOS / connector code | any `1002:xxxx` |
| `AMDFramebuffer` | 0.3 MB | `IOFramebuffer` per display | `AtiFbStub` |
| `AMDRadeonX4000` | 5.2 MB | GFX8 accelerator (Metal kernel side) | `AMDRadeonX4000_AMDBaffinGraphicsAccelerator`, `…EllesmereGraphicsAccelerator` (same IDs) |
| `AMDRadeonX4000HWServices` | 0.1 MB | Loads CAIL and PowerPlay for the accelerator | same IDs, class `…HWServicesPolaris` |
| `…HWServices/PlugIns/AMDRadeonX4{0,1,4,7}00HWLibs` | 13–19 MB | **CAIL** (AMD's ASIC init library) + PowerPlay hwmgr | `IOPropertyMatch LoadPlugIn = Load4x00` |
| `AMDRadeonServiceManager` | 0.1 MB | Power/hive services | `IOResources` |

### Device IDs
Sequoia supports only **Polaris 10 (Ellesmere)** and **Polaris 11 (Baffin)**. There's no Lexa/Polaris 12
(0x699F) and no Tonga/Fiji, so the spoof target stays **Baffin 0x67EF** (or another Baffin ID).

### HWLibs selection
`AMDRadeonX4000_AMDRadeonHWServicesAbstract` has `readProjectName` (reading an EFI "project name" property),
`ProjectNameTable`, `MatchTable` and `findPlugIn`. It sets `LoadPlugIn` = `Load4000`/`Load4100`/`Load4400`/`Load4700`,
and the HWLibs plugin with the matching `IOPropertyMatch` attaches. The four builds differ (X4400 and X4700 are
the same size but not byte-identical). **To find in Ghidra:** which variant Baffin gets on an unknown Mac model,
and whether we can force one through that property.

## The key finding: CAIL already contains Carrizo and Stoney

All four HWLibs builds include AMD's CAIL code for **Carrizo and Stoney** (and Kaveri/Kalindi/Godavari), not
only Polaris. In `AMDRadeonX4400HWLibs` there are 86 Carrizo-specific symbols, including:

| What | Symbols | Linux item it covers ([CARRIZO_VS_POLARIS.md](CARRIZO_VS_POLARIS.md)) |
|---|---|---|
| ASIC table / IDs | `_CarrizoAsicSetupTable`, `_CarrizoId1..3`, `_CarrizoGroup1..3`, `_CarrizoFeatureSupport1`, `_CAIL_DDI_CAPS_CARRIZO_A0/A1`, `_Carrizo_FindAsicRevID` | #1, #20 |
| Setup | `_Carrizo_SetupASIC`, `_Cail_Carrizo_InitFunctionPointer`, `_Carrizo_UpdateSwConstantForHwConfig`, `_Carrizo_HwConstants_8812(_A1)(_ST)` | #12 |
| Golden registers | `_CARRIZO_GoldenSettings_A0/A1/A1_ST`, `_aCarrizo_GoldenRegisterSettings` | #13 golden |
| **Tiling** | `_sCarrizoGbTileModeTbl`, `_sCarrizoGbMacroTileModeTbl` (+ `…Records`) | **#13, the biggest risk** |
| **Firmware** | `_Carrizo_{PFP,ME,CE,MEC1,MEC2,RLC,SDMA0,SDMA1}_UCODE`, `_Carrizo_UcodeInfo`, `_carrizo_init_ucode_buffer`, `_Carrizo_GetFirmwareInfoTable` | #14 (firmware embedded, no injection needed) |
| **SMU8 loading** | `_Carrizo_FormatSmuDramDataBuffer`, hwmgr `carrizo/cz_smumgr.c`, `cz_hwmgr.c`, `cz_powertune.c`, `cz_clockpowergating.c` | **#4, #5, #14** |
| RLC | `_Cail_Carrizo_InitCPJumpTable`, `_Carrizo_LoadRlcUcode`, `_carrizo_reset_rlc`, `_carrizo_halt_rlc`, `_carrizo_send_serdes_cmd` | #15 |
| SMC access | `_Carrizo_get_indirect_register_smc`, `_Carrizo_set_indirect_register_smc` | #3 |
| Doorbells | `_Carrizo_set_cpg_door_bell`, `_Carrizo_set_mec_door_bell_range` | #17 |
| Memory | `_Cail_Carrizo_ReserveFbMcRange`, `_Carrizo_ZeroFbConfigAndSize`, `_carrizo_init_nonsurf_aperture`, `_carrizo_init_enable_ats` | #8–#10 |
| Power / clock gating | `_Carrizo_PowerGatingControl`, `_Carrizo_Update{Coarse,Medium}GrainClockGating`, `_Carrizo_EnableCpPowerGating`, `_carrizo_init_power_gating` | #19 |
| Hang detection | `_Carrizo_IsGFXHung`, `_Carrizo_GetHungBlocks` | #18 |
| UVD / VCE | `_carrizo_patch_uvd_dpg_tables`, `_carrizo_vce_pgfsm`, `_Carrizo_check_vce_harvested` | #21 |
| Display offsets | `_CARRIZO_DCP_OFFSETS`, `_STONEY_CRTC_OFFSETS` | (CAIL-side display tables) |

This changes the Phase 3 plan. NootedRed uses the same trick with Raven code inside `AMDRadeonX5000HWLibs`.
**The goal becomes getting CAIL to identify the device as Carrizo** (family/ASIC ID, revision, `CarrizoAsicSetupTable`),
so it runs its own Carrizo init, firmware load, tiling and SMU8 paths, instead of porting those from Linux.
The Polaris personality is then only needed for the *accelerator* (`AMDRadeonX4000`) and the *display* side
(`AMD9500Controller`), which have no Carrizo code (no codename strings at all).

### How CAIL selects the ASIC (confirmed in Ghidra, `AMDRadeonX4400HWLibs`, Sequoia 24H32)

**1. Device table lookup: `_get_asic_caps_set_from_table(adapter, info, loose)`.** Called from
`_ReinitializeCapTblManager(adapter)` as `get_asic_caps_set_from_table(adapter, adapter + 0x178, 0)`, which is
in turn called from `_CailInitCapTable`. It walks `_CAILAsicCapsInitTable` (data starts at `0x953b28`), made of
0x38-byte rows and ending with device ID `0xFFFFFFFF`:

| Row offset | Carrizo example | Meaning |
|---|---|---|
| +0x00 | `0x9874` | PCI device ID, matched against `info+0x04` |
| +0x08 | `1` | internal revision (fuse `rev_id`), matched against `info+0x1C` for an exact match |
| +0x10 | `2` | external revision (Linux `rev_id + 1`), also used as the priority when no exact match |
| +0x18 | `0xFFFFFFFF` / `0xC4`… | PCI revision (`FFFFFFFF` = any), matched against `info+0x10` |
| +0x20 | `→ CAIL_DDI_CAPS_CARRIZO_A1` | 64-byte caps bitmap, copied to `info+0x1D8` |
| +0x28 | `→ CARRIZO_GoldenSettings_A1` | golden settings |
| +0x30 | `0x87` | family (`AMDGPU_FAMILY_CZ`), written to `info+0x18` |

Matching: same device ID **and** (row revision is `FFFFFFFF`, or `loose`, or equal to the PCI revision). An exact
internal-revision match wins at once; otherwise the highest `+0x10` wins. No match returns 5. On a match it sets
`info+0x1D0` (row pointer), `info+0x18` (family), `info+0x20`/`+0x24` (external revision) and copies the caps,
then runs `_Cail_Detect_Grenada` and `_CheckAsicCapExceptionTbl`.

`0x9870` has an any-revision row (`CARRIZO_A0`). `0x9874` has an any-revision row plus per-revision rows
(`0xC4`, `0xC5`, `0xC6`, …), all `CARRIZO_A1`.

**2. Function-table dispatch: `_CAILInitFunctionPointer(adapter)`.** Reads family at `adapter+0x190`
(= `info+0x18`) and external revision at `adapter+0x19C` (= `info+0x24`):
- family `0x87`, external revision 0x01–0x40: `Cail_Tonga_InitFunctionPointer` then
  `Cail_Carrizo_InitFunctionPointer`
- family `0x82`: 0x3C–0x4F Fiji, 0x50–0x59 Ellesmere (Tonga + Ellesmere), 0x5A–0x6D Baffin (Tonga + Ellesmere + Baffin)

These match Linux `vi.c` (`external_rev_id`, `AMDGPU_FAMILY_CZ`/`VI`) exactly.

**3. `_Cail_Carrizo_InitFunctionPointer(adapter)`.** Only acts if `CailCapsEnabled(adapter+0x178, 0x53)`, i.e. the
Carrizo caps are loaded. It then installs the Carrizo implementations into the adapter's function table:
`+0xFB8 Carrizo_SetupASIC` (= `Cail_ExecuteAsicSetupTable(adapter, &CarrizoAsicSetupTable /*12 steps*/)`),
`+0x1010 SetupCgReferenceClock`, `CheckDsmuSupport`, `UpdateSwConstantForHwConfig`, `ZeroFbConfigAndSize`,
`FillMetaData`, `GetFirmwareInfoTable`, `LoadRlcUcode`, `DetectEDCSupport`, `Cail_Carrizo_ReserveFbMcRange`,
`RestoreAdapterCfgRegisters`, `FindAsicRevID`, clock-gating updates, `PowerGatingControl`, `GetHungBlocks`,
`CheckAcpHarvested`, `FormatSmuDramDataBuffer`, and more.

**Conclusion:** CAIL's whole chip identity follows from three inputs in `info` (adapter `+0x178`): device ID
(`+0x04`), PCI revision (`+0x10`) and internal revision (`+0x1C`). With `0x9874` there, CAIL selects
`CARRIZO_A1`, family `0x87`, and the Carrizo function table, whatever the rest of the driver believes the device is.
`_get_asic_caps_set_from_table` is an exported symbol, so CarrizoRed can hook it (Lilu) and force the real device
ID and revision before it runs, if the input turns out to be the spoofed ID.

**4. Where the inputs come from.** `_CAILInitialize(adapter, initInfo)` checks `*adapter == 0x1690`, then:
`CailSaveCailInitInfo(adapter, initInfo, adapter+0x178)` → `CailInitCapTable(adapter)` → `CailCheckASICInfo` →
`CailCheckChipSetInfo` → `CailCheckBIOSDependentASICInfo` → … (it later compares `info+0x04` with `0x7300` for a
Fiji ROM patch, which confirms `+0x04` is the device ID).
- `CailSaveCailInitInfo` requires `initInfo+0x04 == 0x1FFFF` (interface version) and copies **`initInfo+0x08` →
  `info+0x04` (device ID)**. Flags come from `initInfo+0x0C`, plus pointers/handles from `+0x10…+0x84`.
- The PCI revision (`info+0x10`) and internal revision (`info+0x1C`) are not set there. They're probably filled
  by the `adapter+0xFC8` callback that `CailInitCapTable` calls just before the table lookup (if it returns
  non-zero, the lookup is skipped). `0x9874` has an any-revision row, so the lookup doesn't depend on them.

**5. The device ID is read from real PCI config space.** The full chain:
- `AMDRadeonX4000HWServices`: `AMDRadeonX4000_AMDRadeonHWServicesCAIL::createCailInterface(IOPCIDevice *pci)` just
  calls `hwLibsPlugin->callPlatformFunction("AmdCailInterfaceKey", false, pci, &iface, 0, 0)` (`this+0xA8` is
  the HWLibs plugin, vtable `+0x6C0`). The CAIL interface is built inside HWLibs.
- HWLibs `AmdCailServices::AmdCailServices(IOPCIDevice *pci)` (`__ZN15AmdCailServicesC2EP11IOPCIDevice`,
  `0x1a08b0`): `this+0x10 = pci->extendedConfigRead16(0x2)` (`MOV ESI,0x2` at `0x1a090a`), i.e. the **hardware
  device ID**. It also formats `[bus:dev:func]` and sets `this[0x14] = (deviceId != 0xFFFF)`.
- HWLibs `AmdCailServices::initialize(_CailInitialize *)`: runs `CAILPostVBIOS`, then builds `initInfo` on the
  stack (`+0x00 = 0x88` size, `+0x04 = 0x1FFFF` version, **`+0x08 = this+0x10`**, `+0x0C` flags, `+0x10…`
  from `this+0x20400…`) and calls `CAILInitialize` → `CAILQueryASICInfo` → `CAILQuerySystemInfo` →
  `CAIL_InitParameter`s → `CAILReserveMCAddressRange` (if flag `0x8000`) → `initializeAdapter`.

**Consequence:** an OpenCore `DeviceProperties` `device-id` spoof (e.g. Baffin `0x67EF`) changes only the
IORegistry property. That's enough for IOKit to match Apple's Polaris kexts, while CAIL still reads `0x9874`
from the hardware and selects `CARRIZO_A1` / family `0x87` / the Carrizo function table **with no CAIL patch**.
The remaining work is in the code that is *not* CAIL: `AMDRadeonX4000` (accelerator) and
`AMD9500Controller`/`AMDSupport` (display), which still assume a Baffin.

What still has to be checked in Ghidra:
1. Whether the Carrizo paths are complete or have been compiled out in places (function bodies present vs stubs).
   First hardware boot with the spoof will show this quickly (CAIL errors are printed as
   `AMD Error: … failed (CAILRESULT=%d)`).
3. What the X4000 accelerator assumes about the ASIC that CAIL won't fix: VRAM/aperture, PTE base (#10), pipe
   config passed to user-space (#13).
4. Whether the PowerPlay hwmgr `cz_*` code is reachable from `createPowerPlayInterface`.

## Display side (`AMDSupport`)

### VBIOS source
`AtomBiosProxy::readAtomBios()` (`0x710f0`) fills a 64 KB buffer at `this+0xC`:
1. `readEfiAtomBiosImage(buf, 0x10000)`: `getProperty("ATY,bin_image")` on the GPU's provider (`this+0x10038`),
   copies `min(length, 0x10000)` bytes. The result of `validateAtomBiosImage` is **ignored** on this path.
2. Only if that returned 0: `readPciAtomBiosImage` (PCI expansion ROM), which must also pass `validateAtomBiosImage`.

APUs have no expansion ROM, so CarrizoRed copies the VBIOS from the ACPI VFCT table into `ATY,bin_image`
([`kern_vbios.cpp`](../CarrizoRed/kern_vbios.cpp)). It can also be injected by hand through OpenCore
`DeviceProperties` (a Linux `amdgpu_vbios` dump), in which case CarrizoRed leaves it alone.

### Parsers
`AtiBiosParser1` handles the `atombios.h` format (`_ATOM_I2C_RECORD`, `_ATOM_HPD_INT_RECORD`): GCN 1–4, including
Polaris **and Carrizo**. `AtiBiosParser2` handles `atomfirmware.h` (Vega+). Command tables come in V1/V2 interface
variants (`AtiSetPixelClockInterface_V1/V2`, `AtiSetDceClockInterface_V1/V2`, `AtiTransmitterControlInterface_V1/V2`,
…), and `AtomBiosProxy::setDisplayClock` has both a `(uchar, uint)` and a `(DCEClockParams *)` overload. Still to
check: which variant Apple picks for DCE 11.0 (see [DCE11_REGISTER_DIFF.md](DCE11_REGISTER_DIFF.md), pixel clock).

### Pixel clock path
`AtomBiosProxy::setPixelClock(ClockParams *, ActiveConnectorInfo *)` (`0x70a50`, called from other kexts through its
vtable) only forwards to the parser (`this+0x10070`, vtable `+0x1D8`). `AtiBiosParser1::setPixelClock` forwards
again to its SetPixelClock object (`this+0xB8`, vtable `+0x138`).

`AtiSetPixelClockInterface_V1::createSetPixelClock` picks the class from the **VBIOS's own `SetPixelClock` table
revision**: 7 → `AtiSetPixelClock_V1_V7` (Polaris, frequency only), **6 → `AtiSetPixelClock_V1_V6`**,
**5 → `AtiSetPixelClock_V1_V5`**, anything else → no object ("Not a valid table revision"). So the older
divider-style revisions DCE 11.0 VBIOSes use are still supported, and selected automatically.

`AtiSetPixelClock_V1_V6::setPixelClock` (`0x98890`) builds `PIXEL_CLOCK_PARAMETERS_V6` exactly as in Linux
`atombios.h`:

| V6 field | Source |
|---|---|
| `ulCrtcPclkFreq` (24-bit clock + CRTC id) | `ClockParams+0x08` (DP: `connector+0x20`), CRTC `connector[0x2D]` |
| `usFbDiv` | `ClockParams+0x14 / 10` |
| `ucPostDiv` | `ClockParams[0x18]` |
| `ucRefDiv` | `ClockParams[0x10]` |
| `ucPpll` | `connector[0x2E]` |
| `ucTransmitterID` / `ucEncoderMode` | `translateConnectorInfoToEncoderObjectId` / `…EncoderMode` |
| `ucMiscInfo` | HDMI deep-colour bits, `0x10` (`REF_DIV_SRC`) |
| `ulFbDivDecFrac` | `ClockParams+0x1C` |

`AtiAdjustDisplayPll_V3::adjustDisplayPll(ClockParams *, …)` (`0x92aa0`) runs the VBIOS `AdjustDisplayPll` v3 table
(`ADJUST_DISPLAY_PLL_INPUT/OUTPUT_PARAMETERS_V3`; config flags `0x40` dual link, `0x20` coherent, `0x10` SS) and
writes back `ClockParams+0x04 = ulDispPllFreq`, **`+0x10 = ucRefDiv`, `+0x18 = ucPostDiv`**.

**The feedback divider is computed in `AMD9500Controller`, so the V5/V6 path is complete.**

`AMD9500Controller::hwProgramPixelClock(crtc, clock)` (the mode-set entry):
1. builds a 0x34-byte `ClockParams` (`+0x00 = this+0x31208`, `+0x08 = clock`), releases the PLL;
2. calls vtable `+0xD50` (`getPllParameters`), which runs `adjustPllParameters` and then
   `hwCalculateBasicClockParameters`;
3. spread spectrum (`hwProgramPllSpreadSpectrum`), then `hwProgramPpllUsingBios(crtc, pll, params)`, which copies
   the active connector info (0x98 bytes), sets **CRTC id at `+0x2D`** and **PLL index at `+0x2E`** (rejects PLL > 6), and
   calls `AtomBiosProxy::setPixelClock`;
4. `acquirePll`, `hwSetCrtcToPixelClock`, caches the PLL frequency per PLL (`this+0x312C8 + 0x38 + pll*4`).

`AMD9500Controller::hwCalculateBasicClockParameters(crtc, ClockParams &, ATIConnection *)` (`0x53f40`) is a full PLL
divider search (same idea as Linux `radeon_compute_pll_avivo`):
- target = `+0x04` (the frequency `adjustDisplayPll` returned) if non-zero, otherwise `+0x08`;
- skipped if ref, feedback and post dividers are all set; keeps the ref/post dividers `adjustDisplayPll` returned
  and solves only for feedback when only those are set;
- post divider range from VCO 600–1200 MHz (max 127), ref divider ≤ 255, feedback ×10 in 6…4095, fraction in
  millionths, picks the closest match within 0.1 % (0.01 % for some connections, vtable `+0x198`);
- `ClockParams`: `+0x00` reference clock, `+0x04` adjusted PLL freq, `+0x08` target, `+0x0C` achieved, `+0x10` ref
  div, `+0x14` fb div ×10, `+0x18` post div, `+0x1C` fb fraction, `+0x24` spread-spectrum flag, `+0x30` SS-related.

**Consequence:** the DCE 11.0 "driver must supply dividers" difference ([DCE11_REGISTER_DIFF.md](DCE11_REGISTER_DIFF.md),
pixel clock) is already handled by Apple's code for V5/V6 VBIOSes. No pixel-clock patch is expected. Things to
verify on hardware: the 600–1200 MHz VCO limits match Carrizo's PLLs (Linux takes them from the VBIOS firmware info),
and the PLL index the Baffin assigner hands out (sent raw as `ucPpll`). Carrizo only has `ATOM_PPLL1` (0),
`ATOM_PPLL2` (1) and external/DCPLL, while Baffin has 6 combo-PHY PLLs. The PLL count probably comes from
`BaffinSharedController::getNumPixelClocks`.

### Pipes and line buffers
- `BaffinSharedController::getNumLineBuffers()` returns **6** (one line buffer per pipe). Carrizo has 3 pipes, Stoney 2
  (Linux DC `carrizo_resource_cap` / `stoney_resource_cap`). Planned patch: wrap it to return 3 / 2. Still to find:
  where Apple gets its CRTC count (no `getNum*Crtc*` symbol exists; candidates are the VBIOS display-path table or
  a per-pipe loop bound elsewhere).
- Per-pipe objects are created per `ATIController` (one per framebuffer):
  `DEVICE_COMPONENT_FACTORY::create{Timing,Scaler,Gamma}Generator(uint, ATIController *, uchar)` and
  `BaffinTimingGenerator::initWithController(uchar crtc, ATIController *)`; register blocks come from
  `BaffinTimingGenerator::calculateCrtcOffset(uint)`. **Hypothesis:** the number of pipes in use follows the number of
  framebuffers, i.e. the connector count (from `AtiBiosParser{1,2}::getNumberOfConnectors` when no personality
  matches). The D509BA VBIOS has ~2 connectors (eDP + HDMI), which would keep Apple on pipes 0–1, all Stoney has.
  **Answered (hypothesis was wrong):** `AMD9500Controller::initializeProjectDependentResources()` loops over
  `getDisplayLimit()` (vtable `+0x8f8`) creating the timing generator, hardware cursor, scaler, pipe control and gamma
  generator per pipe, and over `getNumLineBuffers()` (`+0x918`) for the line buffers. Both are constant getters in
  `BaffinSharedController`: `getDisplayLimit` = 6, `getNumLineBuffers` = 6. Neighbours: `getFrameBufferLimit` (`+0x900`)
  returns the byte at `this+0x311f2` (framebuffer count, passed to the connector assigner), `getDigLimit` (`+0x908`) =
  6, `getNumPixelClocks` (`+0x910`) = 7 (passed to the PLL assigner). **CarrizoRed routes `getDisplayLimit` and
  `getNumLineBuffers` to 3 (Carrizo) / 2 (Stoney)** (`-crnopipes` disables it). The PLL count is still to check.
- `BaffinSharedController::initializeResources()` has no per-pipe loop; it maps register sub-ranges of a PCI BAR
  (`[0, 0xC0000)`, `+0x40000`, `+0x100000`, `[0, 0x300000)`; check which BAR before the spoof test fails there).
- `BaffinLineBuffer::initWithController(pipe, ctrl)` sets size `this+0x34 = 0x6013`, `this+0x28 = LBOffset[pipe]`
  (per-pipe register block table), and `this+0x30 = 1` (not shared, so `AtiLineBuffer::getLbSize()` returns the full
  value; it halves it when the flag is clear). `BaffinLineBuffer::calculatePitch(depth, width)` returns entries per
  line: `(w+7)/8`, `≈4w/3/8`, `≈5w/3/8`, `(w+3)/4` for the four depths.
- `BaffinSharedController::getNumNotUsedLbLines` / `hwGetDisplayLineBufferPixels` only do arithmetic on `getLbSize()`
  (used by `canEnablePrefetch` and the scaler).
- `AMD9500Controller::hwProgramDisplayLineBuffer(crtc)` calls the line buffer's vtable `+0x188` and
  `+0x1A0(params+0x53C)` (setup, then pixel storage depth).
- Linux programs `LB_MEMORY_SIZE = 1712 (0x6B0)` for both DCE 11.0 and 11.2 (`dce_transform_construct`), which fits
  Carrizo's 12-bit field. Apple's `0x6013` is an internal capacity in different units. Low risk for first light
  (only affects how many lines fit for scaled or very wide modes); revisit if underflow (flicker or tearing) appears.

### Register `0x13E` (Polaris `PHYPLLF_PIXCLK_RESYNC_CNTL` = Carrizo `PHYPLL_PIXCLK_CNTL`)
A whole-binary scalar search of `AMD9500Controller` for `0x13e` and `0x4f8` finds **no** instruction operands. The
resync registers may still come from a data table (Polaris's six are at irregular offsets). Watch for it on the first
boot (pixel-clock changes, `hwSetCrtcToPixelClock`).

### PLL indexes (patched: limit + remap)
**Implemented.** `getNumPixelClocks()` turned out not to size the assigner:
`BaffinSharedController::initializeProjectDependentResources` calls
`AtiPllAssignerFactory::createPllAssigner(ctrl, connectors, n, 7)` with a constant 7, and the dynamic assigner hands
out the lowest free index (`0xFE` = none). The index reaches the hardware twice: as `ucPpll` through
`hwProgramPpllUsingBios` → `AtomBiosProxy::setPixelClock` (`ActiveConnectorInfo +0x2E`, CRTC at `+0x2D`), and in
`AMD9500Controller::hwSetCrtcToPixelClock(crtc, pll, params)`, which writes it into
`CRTCn_PIXEL_RATE_CNTL[1:0]` (byte `0x500 + 0x10·crtc`) and sets bit 0 of a per-PLL Polaris resync register
(bytes `0x400/0x404/0x408/0x40C/0x430/0x4F8` = dword `0x100–0x103`, `0x10C`, `0x13E`). CarrizoRed passes 2 to
`createPllAssigner` (so `0x13E` is unreachable) and remaps Apple 1 → ATOM 2 in both places (`-crnoplllimit`,
`-crnopllremap`). `getDigLimit()` (6, used by `doConnectionsProbe`) is routed to 3. Earlier notes:

`BaffinSharedController::getNumPixelClocks()` (`0x2f490`) returns **7** (Baffin PHY PLLs 0–5, plus index 6, which
`hwProgramPixelClock` turns into `0xFE` = no PLL). The V6 handler sends the PLL index **unchanged** as `ucPpll`.
Linux DC's DCE 11.0 mapping (`dc/bios/dce110/command_table_helper_dce110.c`):
`PLL0 → ATOM_PPLL0 (2)`, `PLL1 → ATOM_PPLL1 (0)`, `PLL2 → ATOM_PPLL2 (1)` (not created for Carrizo),
external / DP DTO → `ATOM_PPLL_INVALID (0xFF)`. Carrizo's DC resource has `num_pll = 2`.

So on Carrizo, Apple index 0 and 2 are real pixel PLLs, 1 is doubtful, and 3–5 must never be used. Planned fix:
wrap `getNumPixelClocks` (return 2) and remap the index before `SetPixelClock`/spread spectrum/transmitter
control (Apple 0 → ATOM 0, Apple 1 → ATOM 2). Before writing it, get from the first boot: the VBIOS
`SetPixelClock` revision (Apple logs `ATOM: … offset:%#x, V %u.%u` at debug level) and the PLLs the assigner
hands out (`AtiPllAssigner::printPllInfo`).

## Ghidra starting points

Load each binary as **Mac OS X Mach-O, x86-64**. Run auto-analysis with the "Demangler GNU" analyzer enabled.

- `AMDRadeonX4400HWLibs` (or whichever variant Baffin loads): `_CarrizoAsicSetupTable` and its xrefs, then
  find the routine that walks the ASIC setup tables (it'll reference `_BaffinAsicStateRegInfo` /
  `_CarrizoAsicSetupTable` side by side).
- `AMDRadeonX4000HWServices`: `…HWServicesAbstract::start`, `findPlugIn`, `readProjectName`, `populateDeviceInfo`,
  `…HWServicesCAIL::createCailInterface`.
- `AMDRadeonX4000`: `AMDRadeonX4000_AMDBaffinGraphicsAccelerator` vtable, then the shared
  `AMDRadeonX4000_AMDGraphicsAccelerator::start`.
- `AMD9500Controller` / `AMDSupport`: VBIOS fetch, `ATOMBIOS` command-table execution (`SetPixelClock`,
  `DIGxTransmitterControl`). See [DCE11_REGISTER_DIFF.md](DCE11_REGISTER_DIFF.md) "What to do with this".

## How AMD9500Controller gets loaded (GPU wrangler / AGDC)

First spoof boot on the D509BA (`device-id 0x67EF`, `-crnoaccel`): only `AMD9500ControllerWrangler` and `AMDSupport`
attached to the iGPU. The `Controller` personality has `IOPropertyMatch {LoadController=True}`, and the only place that
sets it is `AtiGpuWrangler::vendor_doDeviceAttribute` (AMDSupport), an AGDC vendor method that the graphics policy calls
on a Mac. Attribute 3 with flags: bit 0 sets `LoadAMDSupport` + `LoadController`, bit 1 sets `LoadAMDSupport` +
`LoadHWServices` + `LoadAccelerator`; then `registerService()` (vtable `+0x5b0`; `+0x278` is `setProperty(char*, bool)`).
Nothing requested it for the iGPU, so the controller never matched, and since the wrangler holds the iGPU's
`IOFramebuffer` match category, the boot framebuffer driver couldn't attach either: the GUI ran with no display and the
panel kept the boot console. CarrizoRed now sets `LoadAMDSupport` + `LoadController` and re-registers the iGPU once the
wrangler is attached (`-crnoloadctl` disables it).

`AMD9500Controller` reads its `CFG_*` options with `ATIController::getConfigProperty` from the personality's
`aty_config` (and per-framebuffer `ATY,<name>` overrides). `CFG_NON_ZERO_MC_LOC` is off for Polaris, so `ATY,vram_base`
would be 0; CarrizoRed forces it on so the base comes from `AtiAsicInfo::getFbLocationBase()` (`-crnomcloc` disables).

### AMD9500Controller start-up on Stoney (spoof boots 3–4)
`ATIController::start` → `AMD9500Controller::initializeResources` fails with `0xe00002c9`. Inside it,
`powerUpHardware()` (vtable `+0xcd0`) returns `0xe00002bc`: `detectPowerDown()` (`+0x9a8`) correctly says the GPU is
already up (so no ASIC_Init), but `ATIController::TestVRAM(PCI_REG_INDEX 7, false)` fails (the check is gated by bit 19
of `this+0x31332`). Region 7 is the 3 MB sub-range mapped at offset 0 in `BaffinSharedController::initializeResources`;
the boot console ghosted while it ran, so it writes test patterns into the displayed memory. The other source of the
same error, `createAsicInfo` (`AtiAsicInfo::initWithController` → `ASIC_INFO__BAFFIN::populate{DeviceInfo,FbLocation,
MemoryConfig}`), wasn't reached. CarrizoRed now reports TestVRAM as passed (logging the real result; `-crnovramtest`
disables) to see what fails next.

## Acceleration (`AMDRadeonX4000`) on Stoney (Sequoia 15.8.1 KDK, 2026-10-12)

**Loading.** Both personalities need a property on the iGPU: HWServices `IOPropertyMatch {LoadHWServices}`, the
accelerator `{LoadAccelerator}` (set by `AtiGpuWrangler::vendor_doDeviceAttribute` flag bit 1 on a Mac). CarrizoRed now
sets both with `LoadController` unless `-crnoaccel`.

**HWLibs / CAIL.** `AMDRadeonX4000_AMDRadeonHWServicesPolaris::getMatchProperty` picks the HWLibs plug-in from a table of
Apple subsystem IDs (`Load4100`/`Load4400`/`Load4700`); anything else gets `Load4000` → **`AMDRadeonX4000HWLibs`**. Its
CAIL has full Stoney support: `_CAILAsicCapsInitTable` (rows: family, device, internal rev, external rev, PCI rev, caps,
golden) has an any-revision row `0x98E4 → CAIL_DDI_CAPS_STONEY_A0, STONEY_GoldenSettings_A0_3CU, external rev 0x61`,
and `_CAILInitFunctionPointer` sends family 0x87 with external revision 0x61–0xFE to `_Cail_Stoney_InitFunctionPointer`.
There are `StoneyAsicSetupTable`, `Stoney_{CE,ME,PFP,MEC1,RLC}_UCODE`, `Stoney_HwConstants_2CU/3CU`, `Stoney_LoadRlcUcode`,
power gating and hang detection. Since CAIL reads the device ID from config space, the spoof doesn't affect it.

**Accelerator.** `AMDBaffinGraphicsAccelerator` (← `AMDVIGraphicsAccelerator`) creates `AMDBaffinHardware`
(← `AMDVIHardware` ← `AMDHardware`). Its `getDeviceType` compares the *config-space* device ID with Polaris IDs, so
Stoney gets type 0. Polaris 11 assumptions that CarrizoRed patches on Stoney:
- `AMDBaffinHardware::allocateHWEngines` fills the engine array at `+0x3B0`: PM4 (`+0x3B0`), sDMA0 (`+0x3B8`), sDMA1
  (`+0x3C0`), UVD 6.3 (`+0x3D8`), VCE 3.4 (`+0x3E8`), SAMU (`+0x400`). Stoney has one sDMA. `initializeHWEngines` (11
  slots) and `getHWChannel` skip empty slots, but `AMDHardware::startHWEngines` starts `+0x3B8` and `+0x3C0` and fails if
  either is missing. CarrizoRed releases sDMA1 and starts sDMA0 alone, setting the same status bit (bit 9 of the word
  at `+8` of the block returned by `(this+0x18)->vtable[0x178]`).
- `AMDBaffinHardware::setupAndInitializeHWCapabilities` sets `+0x64` = 2 shader engines, `+0x6C` = 1 SH per SE,
  `+0xA0` = 8 CUs per SH (Ellesmere 4/1/9), then `AMDVIHardware`'s version reads `GB_ADDR_CONFIG` (0x263E),
  `PA_SC_RASTER_CONFIG(_1)` (0xA0D4/5) and the active-CU bitmaps from CAIL (query 3) into `+0x70…`, and the minimum
  active CUs into `+0xA4`. CarrizoRed sets 1 SE and `+0xA0` = `+0xA4` afterwards.
- Tiling: `AMDHWAlignManager::init` takes its configuration from the hardware interface at run time (AddrLib), i.e.
  from what CAIL programmed for Stoney, so the kernel side should follow the real 2-pipe layout. User space is unchecked.
- GPU VM: `AMDVIVMM::getPTEValue(level, addr, flags, fragment)` = `addr & 0xFFFFFFF000 | flags` (bit 0 valid, bit 1
  system); `getPDEValue` = `addr & 0xFFFFFFFFC0 | 1`. Bulk updates go through sDMA:
  `AMDHWVMContext::updateContiguousPTEsWithDMAUsingAddr(dst, count, addr, flagsTemplate, increment)`, with the
  template from `getPTE/PDEValue(…, 0, …)`. On an APU local entries need the carve-out's physical address
  (`MC_VM_FB_OFFSET << 22`, Linux `gmc_v8_0_get_vm_pde` / `vram_base_offset`), so CarrizoRed converts MC addresses
  (inside `MC_VM_FB_LOCATION`) and FB offsets in all three. The black box records which form Apple actually uses.
- Not handled yet: BAR0 (256 MB) is smaller than the 512 MB carve-out (CPU mappings of VRAM above 256 MB); UVD/VCE/SAMU
  on Stoney (UVD 6.2, VCE 3.4, no GPUVM for UVD/VCE on APUs); power management (SMU8 vs SMU7 messages); user space.

### Bring-up on the D509BA (2026-10-13, boots nvram30–66)

What stood between `accel.start` and a running command processor, in the order it was found. Each item has a
boot-arg to undo it (see the README).

| Step | Polaris assumption / gap | CarrizoRed |
|---|---|---|
| `createAccelChannels` | spreads sDMA channels over sDMA0 and sDMA1, uses `getHWChannel` unchecked | sDMA1 lookups return sDMA0's channel |
| `BaffinPowerPlayManager::initialize` | `CFG_NO_PP` was forced by CarrizoRed itself (display-era workaround) | PowerPlay on with acceleration |
| `PHM_Initialize` | no back end for family 0x87; the VI path (Tonga table parser, Polaris SMU7 back end) fails | `PP_PhmUseDummyBackEnd` answered as 1 in `PECI_ReadRegistry` (the iGPU property reads back as 0); VBIOS PowerPlayInfo (rev 6.1) as `PP_PhmSoftPowerPlayTable` |
| `Stoney_LoadRlcUcode` | `DisableCailLoadUcode` (CAIL flags `+0x6F0` bit 21, default 1) leaves the RLC/CP/SDMA microcode to the SMU, which the Dummy back end never asks | flag cleared: CAIL loads the microcode itself (as Linux `fw_load_type=0`); the embedded Stoney RLC/CE/PFP/ME/MEC images are byte-identical to linux-firmware's |
| GPU addresses | `AMDRadeonX4000` assumes VRAM at MC 0 (system aperture `[0, GART)`, GART at 0x20000000) and never reads `MC_VM_FB_LOCATION`; the firmware maps the carve-out at 0xF4_0000_0000, so "VRAM" accesses hit physical RAM | FB window moved to 0 (as Linux `radeon` did on APUs), with HDP and live scanout |
| RLC / gating | the RLC firmware starts but never answers a safe-mode request; CP power gating was allowed (`CP_PG_DISABLE` = 0) and MGCG/CGCG/memory light sleep on | gating off when CAIL loads the RLC: CGCG/CGLS, MGCG overrides, CP/RLC memory light sleep, `CP_PG_DISABLE` |
| `powerUpHWEngines` | UVD 6.3, VCE 3.4 and SAMU stay power-gated without SMU messages | those engines released |
| `AMDBaffinHardware::powerUp` | ORs Polaris GDDR5 bits (`SEQ_RDY_POP_IDLE`, `POP_IDLE_REPLAY`) into `MC_ARB_MISC2` | both ORs patched to 0 |

Checked and found to match Linux's Stoney code: CAIL's `aStoney_GoldenRegisterSettings` (all 37 entries:
`GB_ADDR_CONFIG` 0x12010001, `TCP_CHAN_STEER_LO` 0x10101010, `TCC_EXE_DISABLE`, raster config 0, MGCG/CGCG init,
MC/HDP/ATC/SDMA entries); CAIL's set-up order `StoneyAsicSetupTable` (vce_pgfsm, register_update_for_asic_sku,
init_ucode_buffer, halt_rlc, reset_rlc, init_power_gating = CSIB + save/restore list + jump-table restore + AO CU mask,
init_LBPW, init_clock_gating, enable_perfmon_mgcg, init_nonsurf_aperture, power_down_uvd_block_for_dpg_mode,
init_enable_ats), the same order as Linux `gfx_v8_0_rlc_resume` / `init_pg`; the VMID0 set-up (L1 TLB, L2 cache,
context 0, 1 GB GART); the CP ring set-up and its start-up stream (CSB IB, EOP fence, CE partition `SET_BASE`); compute
rings on MEC1 only. Apple's accelerator itself writes almost no GFX registers; the chip-specific work is CAIL's.

Still open at the time of writing: the CP's second ring fetch never returning (RLC not answering is the lead);
`stoney_init_enable_ats` (→ `Cail_Spectre_EnableAts`) turns on the iGPU's ATS/PRI/PASID capabilities, which belong to
an IOMMUv2 driver macOS doesn't have; CAIL applies SDMA golden settings to a second instance (`SDMA1_POWER_CNTL` read
error).
