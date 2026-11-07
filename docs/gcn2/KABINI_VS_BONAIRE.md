# Kabini/Mullins/Kaveri vs Bonaire: what Apple's GCN 2 drivers will get wrong

Apple's Monterey drivers support GCN 2 (Sea Islands, "CI") **dGPUs**: Bonaire and Hawaii. KaberiRed spoofs the APU's
`device-id` to a Bonaire so those drivers match, so every Bonaire assumption that doesn't hold on the APU has to be
patched. This page lists the differences in Linux's drivers (MIT): `amdgpu` (`gfx_v7_0.c`, `gmc_v7_0.c`, `cik_ih.c`,
`cik_sdma.c`, `dce_v8_0.c`, `cik.c`) and `radeon` (`cik.c`, `kv_dpm.c`). Line references are to Linux master, October
2026.

Kabini/Mullins come first; Kaveri is listed where it differs.

## Summary

| Area | Bonaire (spoof target) | Kabini / Mullins | Kaveri | Patch needed on Kabini? |
|---|---|---|---|---|
| Shader engines / SH per SE | 2 / 1 | 1 / 1 | 1 / 1 | **yes**, if Apple hard-codes them |
| CUs per SH | 7 | 2 | 8 | **yes**, same |
| Render backends per SE | 2 | 1 | 2 | likely (raster config) |
| Tile pipes (`max_tile_pipes`) | 4 | 2 | 4 | **yes**: tiling (see below) |
| Texture channel caches | 4 | 2 | 4 | check |
| Max GS threads | 32 | 16 | 16 | check |
| `GB_ADDR_CONFIG` golden | `0x12010001` | `0x12010001` (+ row size) | `0x12010001` | row size only |
| Tile mode table | `ADDR_SURF_P4_16x16` | `ADDR_SURF_P2` | amdgpu P2, radeon P4 | **yes** (CAIL programs it; user space must agree) |
| `PA_SC_RASTER_CONFIG` | `RB_MAP_PKR0(2) RB_XSEL2(1) SE_MAP(2) SE_XSEL(1) SE_YSEL(1)` | `0` | `RB_MAP_PKR0(2)` | yes, if Apple writes its own |
| Compute (MEC) | 1 MEC × 4 pipes | 1 × 4 | **2** × 4 | no (Kaveri: maybe) |
| SDMA engines | 2 | **2** | 2 | **no** (unlike Stoney) |
| Interrupt handler | CIK IH | CIK IH | CIK IH | no (same block) |
| MC firmware / VRAM type | `bonaire_mc.bin`, GDDR5 from `MC_SEQ_MISC0` | none, VRAM type unknown | none | **yes**: skip MC microcode and GDDR5 setup |
| VRAM | dedicated, at MC 0 | UMA carve-out at `MC_VM_FB_LOCATION`, physical `MC_VM_FB_OFFSET << 22` | same | **yes** (as CarrizoRed) |
| VM page tables (local entries) | FB offset | carve-out physical address (`vram_base_offset`) | same | **yes** (as CarrizoRed) |
| UVD / VCE GPUVM | yes | **no** (UVD and VCE can't use GPUVM) | no | later (video) |
| Power management | `ci_dpm`, SMC firmware `bonaire_smc.bin` | `kv_dpm`, SMU in the APU, no driver firmware | `kv_dpm` | **yes**: expect a Dummy back end |
| PCIe gen 2/3, ASPM | programmed | skipped on IGPs | skipped | **yes**, if Apple programs them |
| Display CRTCs | 6 | **2** | 4 | **yes** |
| DIG encoders / HPD pins | 6 / 6 | 6 / 6 | 7 / 6 | probably not |
| Pixel PLLs | PPLL0, PPLL1, PPLL2 | **PPLL1, PPLL2 only** | PPLL0-2 | **yes**: never hand out PPLL0 |
| DMIF buffers per pipe (≥ 2560 wide) | 4 | 2 | 2 | check |
| Audio endpoints | 7 | 3 | 7 | later (HDMI audio) |
| GFX microcode | PFP, ME, CE, MEC, RLC, SDMA | PFP, ME, CE, MEC, RLC, SDMA | + MEC2 | no: CAIL embeds the APU's own |

## GFX (gfx_v7_0.c)

`gfx_v7_0_gpu_early_init` (around line 4160) sets the shape of each chip:

```
             SE  tile_pipes  cu/sh  sh/se  rb/se  tcc  gprs  gs_threads  hw_contexts
BONAIRE       2      4         7      1      2     4   256      32           8
HAWAII        4     16        11      1      4    16   256      32           8
KAVERI        1      4         8      1      2     4   256      16           8
KABINI/ML     1      2         2      1      1     2   256      16           8
```

The FIFO sizes (`sc_prim_fifo_size_frontend` 0x20, backend 0x100, HiZ 0x30, early Z 0x130) are the same on all four.
`gb_addr_config` starts as `BONAIRE_GB_ADDR_CONFIG_GOLDEN` (`0x12010001`) for Bonaire, Kaveri and Kabini; only the
`ROW_SIZE` field changes. On an APU it comes from the DIMM bank-address mapping (`MC_FUS_DRAM0/1_BANK_ADDR_MAPPING`:
2 KB if any DIMM uses map 11, else 1 KB); on a dGPU from `MC_ARB_RAMCFG.NOOFCOLS` (capped at 4 KB). The first boot's
report records `GB_ADDR_CONFIG` as the firmware left it.

**Tiling.** `gfx_v7_0_tiling_mode_table_init` gives Bonaire a 4-pipe table (`PIPE_CONFIG(ADDR_SURF_P4_16x16)`) and
Kabini/Kaveri/Mullins the 2-pipe one (`ADDR_SURF_P2`). radeon instead picks by `max_tile_pipes`, which gives Kaveri
P4. On Stoney (CarrizoRed) CAIL programmed the APU's own tiling, the kernel's AddrLib followed the hardware, and the
open question was what user space assumes; the same question applies here: Apple's Metal/GL drivers will compute
texture layouts for a Bonaire unless they read the pipe count from the kernel.

**Raster config** (`gfx_v7_0_raster_config`): Bonaire `RB_MAP_PKR0(2) | RB_XSEL2(1) | SE_MAP(2) | SE_XSEL(1) |
SE_YSEL(1)`, Kaveri `RB_MAP_PKR0(2)`, Kabini/Mullins 0. Harvested (fused-off) RBs are handled by
`gfx_v7_0_write_harvested_raster_configs`.

**Compute**: `num_mec` is 2 on Kaveri, 1 on everything else, with 4 pipes per MEC (`gfx_v7_0_mec_init`).

**Microcode**: PFP, ME, CE, MEC and RLC for every chip, plus MEC2 on Kaveri (`gfx_v7_0_init_microcode`). GCN 2 loads
them directly through the `*_UCODE_ADDR/DATA` registers (`gfx_v7_0_cp_gfx_load_microcode`,
`gfx_v7_0_rlc_resume`): no SMU loader, unlike Carrizo/Stoney. CAIL in Apple's HWLibs embeds the Kalindi and Spectre
images (see CarrizoRed `docs/OLDER_APUS.md`).

## Memory controller (gmc_v7_0.c, radeon cik.c)

- **MC microcode**: `gmc_v7_0_init_microcode` loads `bonaire_mc.bin` / `hawaii_mc.bin` and returns 0 for
  Kaveri/Kabini/Mullins: APUs have no MC sequencer to program. Apple's Bonaire path will try to load MC microcode and
  set up GDDR5 (`MC_SEQ_*`), which must be skipped. CAIL picks its own path from the real device ID, so its side
  should already be right; the X4000 accelerator's own `powerUp` is the place to check (on Stoney, CarrizoRed had to
  zero Polaris GDDR5 bits in `MC_ARB_MISC2`).
- **VRAM type**: dGPU reads `MC_SEQ_MISC0.MT`; APU: unknown (`gmc_v7_0_sw_init`).
- **VRAM size and location**: `CONFIG_MEMSIZE` (MB), base `MC_VM_FB_LOCATION[15:0] << 24` as the firmware set it
  (`gmc_v7_0_vram_gtt_location`). If the carve-out is larger than BAR0, Linux uses the carve-out's physical address
  as the CPU aperture (`MC_VM_FB_OFFSET << 22`, `gmc_v7_0_mc_init`).
- **VM**: `vm_manager.vram_base_offset = MC_VM_FB_OFFSET << 22` on APUs, 0 on dGPUs (`gmc_v7_0_sw_init`; radeon
  `cik_vm_init`). Local page-table entries hold that physical address. CarrizoRed's VM fix (convert PTEs/PDEs) carries
  over, as does moving the FB window to MC 0 if the accelerator assumes VRAM at 0.
- **UVD/VCE** can't use GPUVM on Kaveri/Kabini/Mullins (`gmc_v7_0_mc_init`, comment "UVD, VCE do not support GPUVM").

## Interrupts (cik_ih.c) and SDMA (cik_sdma.c)

- The IH block is the same CIK IH on Bonaire and the APUs: read pointer acknowledged through `IH_RB_RPTR`, write-back
  enabled, no doorbell. The Polaris-vs-Carrizo IH difference CarrizoRed fixed doesn't exist here. The ring's
  *location* still matters: AMD9500Controller put it in system RAM by physical address, reached through the system
  aperture, which broke once VRAM moved to MC 0 (CarrizoRed bring-up part 2). Check where AMD8000Controller puts it.
- `cik_sdma_early_init` sets `num_instances = SDMA_MAX_INSTANCE` (2) on every CIK chip: Kabini has **two** SDMA
  engines, like Bonaire. No single-SDMA patch (CarrizoRed's for Stoney) should be needed.

## Display (dce_v8_0.c, radeon cik.c)

- **CRTCs** (`dce_v8_0_early_init`): Bonaire/Hawaii 6, Kaveri 4, Kabini/Mullins 2. Register blocks:
  `CRTC0_REGISTER_OFFSET` 0, CRTC1 `+0x300`, CRTC2-5 `0x2600 + 0x300 × (n − 2)` (`cikd.h`). Apple's controller
  creates 6 pipes' worth of objects for a Bonaire (`CISharedController::getDisplayLimit` / `getNumLineBuffers` = 6) and
  `AMD8000Controller::hwMiscSetup` loops over 6 pipes on its own; see [APPLE_KEXTS.md](APPLE_KEXTS.md).
- **PLLs** (`dce_v8_0_pick_pll`): "KB/ML has PPLL1 and PPLL2"; CI and KV have PPLL0, PPLL1 and PPLL2. DP can use the
  external clock (`dp_extclk`) and skip the PPLL. Apple's CI PLL assigner is created with 2 PLLs whose indexes 0/1 are
  ATOM PPLL1/PPLL2, so it should never hand out PPLL0.
- **DIGs / HPD** (`dce_v8_0_sw_init`): Bonaire/Hawaii 6/6, Kaveri 7/6, Kabini/Mullins 6/6 (marked "?" in Linux).
- **Line buffer** (`dce8_line_buffer_adjust`): `LB_MEMORY_SIZE` 0x6B0 everywhere; DMIF buffers allocated per pipe 2,
  or 4 on dGPUs for modes 2560 pixels wide and above (2 on IGPs). Kabini's eDP panel is 1366×768, so first light is
  unaffected.
- **Watermarks/bandwidth** (`dce8_program_watermarks`, `dce8_bandwidth`): use DRAM channel count and the memory and
  engine clocks from DPM (`radeon_dpm_get_mclk/sclk`); on an APU those come from `kv_dpm`, not GDDR5. Wrong values give
  underflow (flicker) rather than no picture.
- **Audio**: 7 endpoints on Bonaire/Kaveri, 3 on Kabini/Mullins.

## Power (kv_dpm.c vs ci_dpm.c)

Bonaire's PowerPlay is `ci_dpm` with SMC firmware loaded by the driver (`bonaire_smc.bin`); Kabini/Kaveri use
`kv_dpm`, talking to the SMU inside the APU through messages (`kv_smc.c`), with no driver-loaded firmware. Apple's
Bonaire PowerPlay back end would drive an SMC that isn't there. As on Stoney, the first step is PowerPlay's Dummy back
end (clocks left to the SMU firmware), then, if it's worth it, `kv_dpm`'s messages.

## PCIe

radeon `cik_pcie_gen3_enable` and `cik_program_aspm` return early on IGPs: the APU's GPU sits on the internal bus.
Apple's controller or PowerPlay may try link-speed changes for a Bonaire; trace them on the first spoof boot.
