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
