# Carrizo vs Polaris: where Linux treats them differently

Every place in Linux `amdgpu` where Carrizo/Bristol Ridge (`CHIP_CARRIZO`, APU) is handled differently from
Polaris (`CHIP_POLARIS10/11/12`, dGPU). Apple's `AMD9500Controller` / `AMDRadeonX4000` are written for Polaris,
so **each row is a place we expect to patch, or at least check in Ghidra**.

Source: `torvalds/linux` master @ `47324d3a5b3a` (2026-10-08), `drivers/gpu/drm/amd/`. Function names are given
instead of line numbers so this stays useful as the tree moves.

Notes on reading this:
- "Linux" columns are what the code does (verified). "Apple impact" is our expectation of what Apple's Polaris
  code does there, which **still has to be confirmed in Ghidra**.
- Phase numbers match [ROADMAP.md](ROADMAP.md).
- Stoney Ridge differences are noted in passing; it is a later target.

## Summary

| # | Area | Carrizo (APU) | Polaris 11 (Baffin) | Phase | Risk |
|---|---|---|---|---|---|
| 1 | IP block versions | GMC 8.0, CZ IH, SDMA 3.0, UVD 6.0, VCE 3.1 | GMC 8.1, Tonga IH, SDMA 3.1, UVD 6.3, VCE 3.4 | 1 | Med |
| 2 | VBIOS source | ACPI VFCT (part of system BIOS), no PCI ROM | PCI expansion ROM / SMC ROM read | 2 | Med |
| 3 | SMC indirect regs | `MP0PUB_IND_INDEX/DATA` (0x180/0x181) | `SMC_IND_INDEX_11/DATA_11` (0x1AC/0x1AD) | 2 | High |
| 4 | SMU messaging | SMU8: `SMU_MP1_SRBM2P_MSG/ARG/RESP_0` (0x1C0/0x1E0/0x1D0) | SMU7: `SMC_MESSAGE_0/MSG_ARG_0/RESP_0` (0x94/0xA4/0x95) | 2 | High |
| 5 | SMU firmware | In system BIOS, already running | Driver uploads `polaris11_smc.bin` | 2 | High |
| 6 | Display clock | VBIOS `SetPixelClock` path (`program_display_engine_pll`) + DFS bypass | VBIOS `SetDCEClock` table | 2 | High |
| 7 | Display PLLs | PLL0, PLL1 + external; 3 CRTCs, 3 DDC | 6 combo-PHY PLLs + DP DTO; 5 CRTCs | 2 | High |
| 8 | VRAM size/type | `CONFIG_MEMSIZE` = carve-out; no VRAM type reg (`MC_SEQ_MISC0` absent) | `CONFIG_MEMSIZE` + `MC_SEQ_MISC0` (GDDR5) | 2–3 | Med |
| 9 | VRAM CPU access | Carve-out in system RAM at `MC_VM_FB_OFFSET << 22` | BAR0 aperture (resizable BAR) | 2–3 | Med |
| 10 | GPU page tables | VRAM PTEs offset by `MC_VM_FB_OFFSET << 22` | VRAM PTEs offset 0 | 3 | **Very high** |
| 11 | MC firmware | None | `polaris11_mc.bin` uploaded | 3 | Low |
| 12 | GFX config | 1 SE, 1 SH, 8 CU, 2 RB, 2 tile pipes | 2 SE, 4 tile pipes (from VBIOS gfx info) | 3 | High |
| 13 | Tiling | `ADDR_SURF_P2` everywhere, `GB_ADDR_CONFIG` = 0x22010001 | `P4_16x16`, `GB_ADDR_CONFIG` = 0x22011002 | 3 | **Very high** |
| 14 | CP/RLC/SDMA firmware load | Driver builds TOC in DRAM, SMU8 loads it (`ExecuteJob`) | SMU7 loads via its own TOC format | 3 | **Very high** |
| 15 | RLC | Needs CP jump table + GDS buffer (`RLC_JUMP_TABLE_RESTORE`); 5 jump tables | 4 jump tables, no CP table buffer | 3 | High |
| 16 | GUI idle interrupt | Enabled only *after* CP is up | Toggled around RLC start / CP resume | 3 | Low |
| 17 | Doorbells | Aperture enable and `CP_RB_DOORBELL_RANGE` skipped | Programmed | 3 | Med |
| 18 | ASIC reset | No full ASIC reset | BACO / PCI config reset | 4 | Med |
| 19 | Clock/power gating | Driver-programmed (`cz_mgcg_cgcg_init`), PG via SMU8 messages | Mostly SMU7-managed | 4 | Off until Ph. 4 |
| 20 | Revision ID | Read from SMC fuse `0xC0014044` | `PCIE_EFUSE4` strap | 1–2 | Low |
| 21 | UVD/VCE | No GPUVM; APU DFS clock regs (`GNB_CLK1/2/3`) | All engines use GPUVM; `CG_VCLK/DCLK/ECLK` | 5 | Phase 5 |

The rest of this document explains each row.

---

## 1. IP blocks (`vi.c`: `vi_set_ip_blocks`)

| Block | Carrizo | Stoney | Polaris |
|---|---|---|---|
| Memory controller | `gmc_v8_0` | `gmc_v8_0` | `gmc_v8_1` |
| Interrupt handler | `cz_ih` | `cz_ih` | `tonga_ih` |
| Graphics | `gfx_v8_0` | `gfx_v8_1` | `gfx_v8_0` |
| SDMA | `sdma_v3_0` (2 instances) | `sdma_v3_0` (1 instance) | `sdma_v3_1` |
| Power | `pp_smu` → smu8 | smu8 | `pp_smu` → smu7 |
| Display | DC, DCE 11.0 | DC, DCE 11.0 | DC, DCE 11.2 |
| UVD / VCE | 6.0 / 3.1 | 6.2 / 3.4 | 6.3 / 3.4 |

- `amdgpu_device.c`: `adev->family` is `AMDGPU_FAMILY_CZ` (APU) vs `AMDGPU_FAMILY_VI`. DC chooses the DCE 11.0
  clock manager based on `FAMILY_CZ` (`clk_mgr.c: dc_clk_mgr_create`).
- IH: `cz_ih` and `tonga_ih` differ only in that Tonga has `IH_DOORBELL_RPTR`. Carrizo has no IH doorbell, so
  Apple's IH setup must not depend on it.
- GPU recovery is disabled by default on Carrizo (`amdgpu_device_should_recover_gpu`).

**In Ghidra:** find where Apple picks the per-ASIC subclass, family ID and capability table. Expect Apple to
use its own enums, not Linux's.

## 2. VBIOS (`vi.c`: `vi_read_bios_from_rom`, `amdgpu_bios.c`)

The APU has no PCI expansion ROM. `vi_read_bios_from_rom` returns early for `AMD_IS_APU` ("APU vbios image is
part of sbios image"), and Linux gets it from the ACPI **VFCT** table instead.

**Apple impact:** the controller's VBIOS fetch (PCI ROM / `ATY,bin_image` / SMC ROM index) will find nothing.
Easiest fix: read VFCT ourselves and inject it as the `ATY,bin_image` property, or hook the fetch routine.

## 3–5. SMC/SMU access

### Indirect SMC registers (`vi.c`: `cz_smc_rreg` vs `vi_smc_rreg`)

| | Index reg | Data reg |
|---|---|---|
| Carrizo | `mmMP0PUB_IND_INDEX` = 0x180 | `mmMP0PUB_IND_DATA` = 0x181 |
| Polaris | `mmSMC_IND_INDEX_11` = 0x1AC | `mmSMC_IND_DATA_11` = 0x1AD |

Every `RREG32_SMC` / `WREG32_SMC` in Apple's code (clock pins, fuses, CG registers) goes through these. It's
probably one or two small accessor functions in `AMDSupport`/`AMD9500Controller`, which makes them good hook
points.

### SMU message mailbox (`smu8_smumgr.c`: `smu8_send_msg_to_smc_with_parameter`)

| | Message | Argument | Response |
|---|---|---|---|
| SMU8 (Carrizo) | `SMU_MP1_SRBM2P_MSG_0` = 0x1C0 | `SMU_MP1_SRBM2P_ARG_0` = 0x1E0 | `SMU_MP1_SRBM2P_RESP_0` = 0x1D0 |
| SMU7 (Polaris) | `SMC_MESSAGE_0` = 0x94 | `SMC_MSG_ARG_0` = 0xA4 | `SMC_RESP_0` = 0x95 |

Sequence: wait for RESP≠0, write ARG, write RESP=0, write MSG, wait for RESP≠0. The message **IDs** are also
different (SMU8 `PPSMC_MSG_*` in `pm/powerplay/inc/smu8_*`), so translating a Polaris message 1:1 is not enough.
Polaris messages with no SMU8 equivalent will have to be dropped or emulated.

SMU8 messages Linux actually uses (`smu8_hwmgr.c`): `SetSclkSoftMin/Max`, `SetSclkHardMin`,
`SetEclkSoft/HardMin/Max`, `SetUvdSoft/HardMin/Max`, `Enable/DisableAllSmuFeatures`, `UVD/VCE/ACPPowerON/OFF`,
`SetDisplaySizePowerParams`, `SetMinDeepSleepSclk`, `SetWatermarkFrequency`, `Enable/DisableLowMemoryPstate`,
`GetMaxSclk/Eclk/Uvd/AclkLevel`, `GetAverageGraphicsActivity`, `DramAddrHi/Lo{Virtual,Physical}`,
`DramBufferSize`.

### SMU firmware

Carrizo's SMU firmware ships in the system BIOS and is already running at boot. `smu8_start_smu` just reads its
version from SMU SRAM (`SMN_MP1_SRAM_START_ADDR` 0x10000000 + `SMU8_FIRMWARE_HEADER_LOCATION` 0x1FF80).

**Apple impact:** Apple's Polaris path uploads SMC firmware and starts the SMC. On Carrizo that must be skipped
entirely. Uploading to the wrong registers could hang the APU.

## 6–7. Display engine (DC `dce110` vs `dce112`)

| | Carrizo (DCE 11.0) | Polaris 11 (DCE 11.2) |
|---|---|---|
| Timing generators (CRTCs) | 3 (Stoney 2) | 5 (Polaris 10: 6) |
| Stream encoders / audio / DDC | 3 / 3 / 3 | 5 / 5 / 5 |
| Clock sources | `PLL0`, `PLL1`, `EXTERNAL` | `COMBO_PHY_PLL0..5`, `DP_DTO` |
| Display clock set via | `dce_set_clock` → VBIOS `program_display_engine_pll` (SetPixelClock), DFS bypass support | `dce112_set_clock` → VBIOS `set_dce_clock` (SetDCEClock) |
| DPREFCLK | n/a | `dce112_set_dprefclk` |
| Display → power requirements | `dce11_pplib_apply_display_requirements` → SMU8 | same entry, SMU7 behind it |
| Extra | Underlay (video) pipe, FBC compressor | none |

Files: `resource/dce110/dce110_resource.c` (`carrizo_resource_cap`, `dce110_clock_source_create`),
`resource/dce112/dce112_resource.c`, `clk_mgr/dce110/dce110_clk_mgr.c`, `clk_mgr/dce112/dce112_clk_mgr.c`,
`clk_mgr/dce100/dce_clk_mgr.c` (`dce_set_clock`).

**Apple impact:**
- Apple's per-ASIC display tables will assume 5–6 CRTCs and combo-PHY PLLs. Restrict the CRTC count to 3 and
  remap PLL selection to PLL0/PLL1/external.
- The display-clock call uses a VBIOS command table that Carrizo's VBIOS may implement differently (or not at
  all). Check which ATOM tables the VFCT image contains (`SetDCEClock` index vs `SetPixelClock` revision).
- DCE 11.0 vs 11.2 register offset differences still need a header-by-header diff (`dce_11_0_d.h` vs
  `dce_11_2_d.h`). That's a separate task.
- Linux removed the legacy non-DC `dce_v11_0.c`. It's still in older tags (e.g. v6.12) if we want the simpler
  version.

## 8–10. Memory controller (`gmc_v8_0.c`)

### VRAM size and type (`gmc_v8_0_mc_init`, `gmc_v8_0_sw_init`)
- Size: `CONFIG_MEMSIZE` (MB) on both. On Carrizo it's the UMA carve-out (already logged by our kext). Linux
  masks the upper 16 bits if garbage.
- Type: APU → `AMDGPU_VRAM_TYPE_UNKNOWN`; the VRAM-type register `MC_SEQ_MISC0` is only read on dGPUs.
  **Apple impact:** Apple's VRAM-type detection will read nonsense; force it (DDR4 / "unknown").
- Width: from VBIOS `vram_width`, else `MC_ARB_RAMCFG` / `MC_SHARED_CHMAP`.

### CPU access to VRAM
- Polaris: through BAR0, resized with `amdgpu_device_resize_fb_bar`.
- Carrizo (x86-64, not passthrough): `aper_base = MC_VM_FB_OFFSET << 22`, `aper_size = real_vram_size`. Linux
  maps the carve-out **directly in system RAM** and doesn't use BAR0 or resize it.
  **Apple impact:** If Apple maps VRAM through BAR0 that may still work (BAR0 on the APU decodes to the
  carve-out), but Apple must not try to resize the BAR. Using the direct mapping like Linux is the fallback.

### GPU page tables (most important GMC item)
`gmc_v8_0_sw_init`: on APU, `vm_manager.vram_base_offset = MC_VM_FB_OFFSET << 22`. Every PTE pointing at a
VRAM page adds this offset because the GPU sees the carve-out at its *system physical* address.
**Apple impact:** Apple's VM code will write PTEs with offset 0. Rendering would then hit random system memory,
which shows up as corruption, then a hang, then possibly a kernel memory scribble. Find the PTE builder in
`AMDRadeonX4000` and add the offset, or set an existing "VRAM base" field if Apple has one.

### Other GMC items
- No MC firmware on Carrizo (`gmc_v8_0_init_microcode` returns 0): skip Apple's MC ucode upload
  (`polaris_mc_load_microcode` equivalent).
- GART: 1 GB on Carrizo (UVD/VCE can't use GPUVM) vs 256 MB on Polaris.
- Golden registers: Carrizo only sets `MC_MEM_POWER_LS` = 0x104 (`cz_mgcg_cgcg_init`). The Polaris golden table
  (`golden_settings_polaris11_a11`) must **not** be applied.
- Display can scan out of GTT ("DCE SG support"). `amdgpu_bo_get_preferred_domain` puts VRAM|GTT buffers in
  GTT when the carve-out is small. That matters if the UMA setting can't go above 512 MB.

## 11–17. Graphics (`gfx_v8_0.c`)

### Configuration (`gfx_v8_0_gpu_early_init`)

| | Carrizo | Stoney | Polaris 11 |
|---|---|---|---|
| Shader engines | 1 | 1 | from VBIOS (2) |
| SH per SE | 1 | 1 | VBIOS |
| CU per SH | 8 | 3 | VBIOS |
| Backends (RB) per SE | 2 | 1 | VBIOS |
| Tile pipes | 2 | 2 | VBIOS (4) |
| Texture channel caches | 2 | 2 | VBIOS |
| Max GS threads | 32 | 16 | 32 |
| `GB_ADDR_CONFIG` golden | 0x22010001 | 0x22010001 | 0x22011002 |
| MEC count | 2 | 1 | 2 |
| Always-on CUs | 2 | 2 | all |
| `double_offchip_lds_buf` | 0 | 0 | 1 |

`GB_ADDR_CONFIG` decoded: bits [2:0] `NUM_PIPES` = 1 (2 pipes) vs 2 (4 pipes); bits [13:12]
`NUM_SHADER_ENGINES` = 0 (1 SE) vs 1 (2 SE). `ROW_SIZE` is then fixed up from the DIMM address-mapping fuses
(`MC_FUS_DRAM0/1_BANK_ADDR_MAPPING`) on APUs, not from `MC_ARB_RAMCFG`.

Raster config (`gfx_v8_0_raster_config`): Carrizo `RB_MAP_PKR0(2)` only; Polaris 11 adds `RB_XSEL2`,
`SE_MAP`, `SE_XSEL`, `SE_YSEL`. Golden `PA_SC_RASTER_CONFIG` = 0x00000002 vs 0x16000012.

### Tiling (`gfx_v8_0_tiling_mode_table_init`): highest-risk item
Carrizo's `GB_TILE_MODE*` / `GB_MACROTILE_MODE*` tables use `ADDR_SURF_P2` throughout. Polaris 11/12 use
`ADDR_SURF_P4_16x16` and Polaris 10 uses `P8_32x32_16x16`. **No Polaris uses P2.**

**Apple impact:** the kernel driver programs the tile tables, and Apple's user-space (Metal driver / addrlib)
computes surface layouts from the pipe config it's told about. Every tiled surface will be laid out wrong
unless:
1. the kernel programs Carrizo's tables (copy from Linux; they're MIT), **and**
2. user-space is told it's a 2-pipe part, through whatever property or shared struct carries
   `GB_ADDR_CONFIG` / tile modes, or by patching the user-space bundle.

If (2) turns out to be hard-coded per device ID in user-space, we'll need a user-space patch (Lilu can do that).
This is the main open question for Phase 3. Check it early in Ghidra.

### Golden registers (`gfx_v8_0_init_golden_registers`)
Apply `cz_mgcg_cgcg_init`, `cz_golden_settings_a11`, `cz_golden_common_all` **instead of** the Polaris tables.
Notable differences from Polaris 11: `GB_GPU_ID`, `SQ_RANDOM_WAVE_PRI`, `TCC_EXE_DISABLE`, `TCP_ADDR_CONFIG`
mask (0xF vs 0x3FF), `TCP_CHAN_STEER_LO` 0x1302 vs 0x3210, `TA_CNTL_AUX` 0x10000 vs 0xB0000, `PA_SC_ENHANCE`
0x1 vs 0x20000001. Polaris also sets `CB_HW_CONTROL`, `SQ_CONFIG`, `VGT_RESET_DEBUG`, which Carrizo doesn't.

### Firmware
Files: `carrizo_{ce,pfp,me,mec,mec2,rlc,sdma,sdma1,uvd,vce}.bin` from linux-firmware. Polaris prefers the `_2`
variants; Carrizo has none.

### Firmware loading through SMU8 (`smu8_smumgr.c`: `smu8_request_smu_load_fw`)
Linux load type is `AMDGPU_FW_LOAD_SMU` for both chips, but the mechanism differs:
1. Driver puts each ucode in a GPU buffer and builds a **TOC** (job list) in DRAM:
   `smu8_smu_construct_toc_for_bootup` loads SDMA0, SDMA1, CP_CE, CP_PFP, CP_ME, MEC_JT1, MEC_JT2, RLC_G.
   Also built are RLC ARAM save, power-profiling, vddgfx enter/exit and clock-table jobs.
2. Zero `UcodeLoadStatus` in SMU SRAM.
3. `DriverDramAddrHi/Lo` ← TOC address, `InitJobs`, then `ExecuteJob` for the ARAM, power-profiling and
   bootup entries.
4. Poll `UcodeLoadStatus` for all masks.
5. Point MEC at its ucode directly (`CP_CPC_IC_BASE_LO/HI`, `smu8_load_mec_firmware`).

**Apple impact:** Apple's Polaris SMU7 firmware-load path (or direct CP microcode register writes) won't work.
This is probably the largest piece of new code: a port of the SMU8 TOC builder, called in place of Apple's
loader. The MIT-licensed `smu8_smumgr.c` / `smu8_fusion.h` structs can be used directly.

### RLC (`gfx_v8_0_rlc_init`, `gfx_v8_0_init_pg`, `gfx_v8_0_cp_jump_table_num`)
- Carrizo/Stoney need a **CP table buffer**: `ALIGN(96 * 5 * 4, 2048) + 64 KB` (jump tables + GDS backup),
  programmed into `RLC_JUMP_TABLE_RESTORE`. Carrizo has 5 jump tables, everything else has 4.
- PG init also programs CSB, the save/restore list and `RLC_PG_ALWAYS_ON_CU_MASK`.
- Even with power gating off, expect the RLC to need the jump-table buffer. Treat it as required.

### Interrupts / doorbells
- GUI idle interrupt: not enabled in `gfx_v8_0_rlc_start` on APU ("carrizo do enable cp interrupt after cp
  inited"), and not disabled in `gfx_v8_0_cp_resume`.
- Doorbells: `vi_enable_doorbell_aperture` is a no-op on APU, and `gfx_v8_0_set_cpg_door_bell` skips the
  `CP_RB_DOORBELL_RANGE_*` registers.

### EDC GPR workaround
`gfx_v8_0_do_edc_gpr_workarounds` runs **only on Carrizo**. It dispatches compute shaders to initialise
VGPRs/SGPRs for EDC. Polaris has nothing equivalent, so Apple won't do it either. Probably harmless to skip at
first. Revisit if we see ECC/EDC errors.

## SDMA (`sdma_v3_0.c`)
- Carrizo: 2 instances, firmware `carrizo_sdma.bin`, `carrizo_sdma1.bin`. Stoney: 1 instance.
- Golden: only `cz_mgcg_cgcg_init` (CG). Clock gating via SDMA MGCG/LS, which stays off until Phase 4.
- Ucode is loaded by SMU8 (above), not by direct writes.

## 18–20. Misc SoC (`vi.c`)
- **Reset:** `vi_asic_reset` does nothing on APU. Apple's hang recovery (BACO / PCI config reset) must be
  disabled or turned into a soft reset of individual blocks.
- **Power/clock gating flags** (`vi_common_early_init`): Carrizo CG covers UVD, GFX MGCG/MGLS/RLC/CP/CGTS/CGCG/
  CGLS, BIF LS, HDP, SDMA and VCE. PG covers GFX SMG/pipeline, CP, UVD and VCE, but only if `rev_id != 0` or
  Bristol Ridge (`CZ_REV_BRISTOL`: PCI revision 0xC8–0xCE or 0xE1–0xE6). All off until Phase 4.
- **Revision:** APU reads `ATI_REV_ID_FUSE_MACRO` at SMC 0xC0014044, bits [12:9]. `external_rev_id = rev_id + 1`.
- **xclk:** APU uses the VBIOS SPLL reference (Stoney: hard-coded 100 MHz). Polaris reads `CG_CLKPIN_CNTL*`.
- **ASPM:** skipped (`asic_type < POLARIS10`).
- **PCIe perf counters / SMC running check:** skipped on APU (registers report 0).

## 21. UVD / VCE (Phase 5)
- UVD 6.0, VCE 3.1, neither using GPUVM (they work in GPU physical addresses). Polaris engines use VM.
- Clocks: APU uses DFS registers `GNB_CLK1/2/3_DFS_CNTL` (0xD82200F0/0xD8220110/0xD8220130) through SMC
  indirect, instead of `CG_DCLK/VCLK/ECLK_CNTL`.
- VCE harvest fuse read from SMC on APU. UVD `use_ctx_buf` from firmware ≥ 1.87.11.

---

## What this means for the plan

1. **Spoof target.** No Polaris matches Carrizo's 1 SE / 2-pipe layout. Baffin (0x67EF) is the current default.
   Lexa (Polaris 12, 0x699F) has 8 CUs like Carrizo but still uses 4 pipes. When choosing, look in Ghidra for
   the ASIC with the least hard-coded SE/pipe assumptions.
2. **Hook points we expect to need** (find these first in Ghidra):
   - SMC indirect read/write accessors (#3)
   - SMU message send (#4) and SMC firmware upload / start (#5)
   - VBIOS fetch (#2)
   - VRAM size/type detection (#8)
   - PTE construction / VRAM base (#10)
   - Tile mode and `GB_ADDR_CONFIG` programming, and how they reach user-space (#13)
   - CP/RLC/SDMA firmware loader (#14) and RLC init (#15)
   - Golden register table selection (#13)
   - Display: CRTC count, PLL selection, display-clock VBIOS call (#6–7)
3. **Data we can bring over from Linux (MIT):** Carrizo golden tables, tile/macrotile tables, SMU8 TOC/firmware
   header structs, register offsets (`gfx_8_0_d.h`, `gmc_8_1_d.h`, `smu_8_0_d.h`, `dce_11_0_d.h`).
4. **Things to measure on hardware with `umr`:** final `GB_ADDR_CONFIG`, `PA_SC_RASTER_CONFIG`, tile mode
   registers, `MC_VM_FB_OFFSET`, `MC_VM_FB_LOCATION`, and the SMU firmware version. These confirm the values
   above for the A12-9800 specifically.

## Open questions
- Does Apple's user-space get tiling information from the kernel, or does it hard-code it per ASIC?
- Does the A12-9800's VFCT VBIOS contain `SetDCEClock`, or only `SetPixelClock`?
- Does Apple's X4000 load CP microcode via SMC TOC or by direct register writes? (Changes the size of #14.)
- Does Apple map VRAM through BAR0 and assume BAR0 size == VRAM size? (BAR0 on the APU is usually 256 MB,
  smaller than a 1–2 GB carve-out.)
