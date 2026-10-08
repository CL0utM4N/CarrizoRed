# Roadmap

Each phase ends with a concrete, testable milestone. Power/clock gating stays **off** until Phase 4.

## Phase 0 — Environment and research
See [PHASE0.md](PHASE0.md).
- [ ] macOS boots on the A12-9800 with OpenCore + AMD_Vanilla (firmware framebuffer, no acceleration)
- [x] Build pipeline works (GitHub Actions macOS runner)
- [ ] Reference data collected: ioreg, VBIOS (VFCT), Linux `umr` register dumps
- [ ] `AMDRadeonX4000`, `AMDRadeonX4000HWServices`, `AMD9500Controller`, `AMDSupport` loaded in Ghidra
- [x] Linux Carrizo-vs-Polaris differences catalogued: [CARRIZO_VS_POLARIS.md](CARRIZO_VS_POLARIS.md)
- [x] DCE 11.0 vs 11.2 register header diff: [DCE11_REGISTER_DIFF.md](DCE11_REGISTER_DIFF.md)

## Phase 1 — Observe (current)
- [x] Lilu plugin skeleton, iGPU detection by config-space ID
- [x] Log PCI info, BARs, UMA carve-out size/location, GFX config registers
- [x] Log when Apple's Polaris kexts load
- [ ] Confirm the above on hardware
- [ ] Spoof `device-id` to `0x67EF` via OpenCore DeviceProperties and record exactly where
      `AMD9500Controller` / `AMDRadeonX4000` fail (panic log, last log lines)

## Phase 2 — Display (`AMD9500Controller` + `AMDSupport`)
- [ ] Make the controller accept the APU: ASIC caps, chip family/revision checks
- [ ] VBIOS from ACPI VFCT instead of PCI ROM
- [ ] VRAM info from `CONFIG_MEMSIZE` / `MC_VM_FB_OFFSET` instead of the dGPU memory controller
- [ ] DCE 11.0 vs 11.2 differences: register offsets, PLL / clock sources, display clock via SMU8
- [ ] **Milestone:** correct native resolution on HDMI/DVI/DP, hotplug works

## Phase 3 — Acceleration (`AMDRadeonX4000` + HWServices)
- [ ] GMC v8 APU setup (system aperture, carve-out as VRAM, GART)
- [ ] Inject Carrizo CP (PFP/ME/CE/MEC/MEC2), RLC and SDMA firmware (linux-firmware `carrizo_*.bin`)
- [ ] Golden registers + GFX config (SE/SH/CU/RB counts) from `gfx_v8_0.c` / `vi.c`
- [ ] Ring tests pass (GFX, SDMA)
- [ ] **Milestone:** Metal device present, WindowServer composites with acceleration

## Phase 4 — Stability and power
- [ ] SMU8 messages for power/clock gating (`smu8_*`)
- [ ] Sleep/wake
- [ ] Hang detection / recovery

## Phase 5 — Extras
- [ ] UVD 6 decode / VCE 3 encode
- [ ] Stoney Ridge (single SDMA, fewer CUs, different golden settings)
