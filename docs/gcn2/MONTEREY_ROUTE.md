# Roadmap

**2026-11-03: GCN 2 support moved to CarrizoRed.** It will adapt Apple's current Polaris drivers with Lilu, as for
Stoney, instead of Monterey's `AMD8000Controller`. The plan is in CarrizoRed's `docs/OLDER_APUS.md`. Phase 1's
"macOS boots on the test laptop" still applies there: the Kabini boot freeze ([TEST_MACHINE.md](TEST_MACHINE.md))
blocks either route. Phases 2–4 below are kept as a record of the Monterey route.

Each phase ends with a testable milestone. Test boots cost the tester real time: a visual test is only requested once
a counter in the report proves the patch in question landed, and one test EFI is rebuilt in place.

## Phase 0: environment and research (current)

- [x] Repository skeleton from CarrizoRed: Makefile, Info.plist template, Lilu entry, CI, black box
- [x] Monterey's AMD binaries on Windows without a KDK (`scripts/fetch_macos.py`, Apple's installer, 12.7.6 21H1320)
- [x] Inventory of `AMD8000Controller`, `AMDSupport`, `AMDRadeonX4000`, HWServices/HWLibs: GCN 2 classes, device-ID
      tables, CAIL Kalindi/Spectre rows and microcode ([APPLE_KEXTS.md](APPLE_KEXTS.md))
- [x] Linux differences Bonaire vs Kabini/Kaveri ([KABINI_VS_BONAIRE.md](KABINI_VS_BONAIRE.md))
- [x] Spoof target: `0x665C` (provisional; any of the eight Bonaire IDs matches all three kexts)
- [x] Metal has a GFX7 code generator: `libSC` gives both CI and KV families the Bonaire back end. Its code has the
      16-bank interpolation hazard (33 of the Metal driver's 435 built-in GFX6/7 programs), so Kabini/Mullins need the
      interpolation fixer; Kaveri doesn't
- [x] CAIL's Kalindi code paths in the stripped `AMDRadeonX4050HWLibs`: Bonaire table + 15 Spectre + 3 Kalindi
      overrides, direct microcode load ([APPLE_KEXTS.md](APPLE_KEXTS.md))
- [x] First CI build (GitHub Actions, 2026-10-17)
- [x] Reference data from the HP A4-5000 ([TEST_MACHINE.md](TEST_MACHINE.md)): IDs, ACPI tables, VBIOS (no VFCT), memory map; registers come from KaberiRed's first report

## Phase 1: observe

- [ ] macOS Monterey boots on the A4-5000 with the firmware framebuffer (OpenCore, no KaberiRed)
- [ ] KaberiRed loaded, no spoof: report (`krpanicat=60`) shows the iGPU, carve-out, `GB_ADDR_CONFIG`, VBIOS injected
- [ ] Spoof `device-id` with `-krnoaccel`: which of Apple's drivers attach, and where the controller stops

## Phase 2: display (`AMD8000Controller` + `AMDSupport`)

- [ ] Controller accepts the APU: ASIC caps, family/revision checks, VBIOS from VFCT, VRAM from `CONFIG_MEMSIZE`
- [ ] DCE 8.3 limits: `getDisplayLimit`/`getNumLineBuffers` 6 → 2, `hwMiscSetup`'s hard-coded 6-pipe loop, eDP link
      (Apple's PLL assigner already uses 2 PLLs, which map to PPLL1/PPLL2)
- [ ] **Milestone:** native resolution on the internal panel, brightness, then HDMI/VGA

## Phase 3: acceleration (`AMDRadeonX4000`)

- [ ] CAIL runs its own Kalindi path (it reads the real device ID) and loads its embedded microcode
- [ ] VRAM = carve-out: FB window, VM page-table entries, interrupt ring placement
- [ ] Shader engine / CU / RB / tiling configuration for 1 SE, 2 CUs, 1 RB, 2 pipes
- [ ] PowerPlay Dummy back end
- [ ] **Milestone:** Metal device present, WindowServer composites with acceleration
- [ ] 16-bank LDS hazard (`v_interp_p1_f32` writing its own I source): check what Apple's GFX7 compiler emits; if
      needed, port CarrizoRed's interpolation fixer with a GFX7 decoder, an offline harness and a fuzzer first

## Phase 4: Kaveri, then newer macOS

- [ ] Kaveri (DCE 8.1, 4 CRTCs, PPLL0-2, up to 8 CUs, 2 MECs)
- [ ] ~~Ventura and later with OpenCore Legacy Patcher's legacy-GCN root patches~~ Dropped: no OCLP. OpenCore can't
      inject Monterey's GCN 2 kexts on Big Sur and later, because their dependencies are only in the System KC. Newer
      macOS comes through CarrizoRed's Polaris route instead
