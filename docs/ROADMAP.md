# Roadmap

Updated 2026-11-06.

## Done
- Lilu plugin, iGPU detection, VBIOS from ACPI VFCT, device-id spoof to Polaris 11 (`0x67EF`)
- Display: built-in eDP panel through Apple's `AMD9500Controller` (DCE 11.0 pipes, PLLs, DIGs, line buffer, eDP link)
- Acceleration: `AMDRadeonX4000` with an SMU8 microcode loader, VRAM window at 0, IH ring in VRAM, Stoney caps
- Metal: shader fix for Stoney's 16-bank LDS (including Metal's SDMA re-uploads), Stoney compiler back end
- SCLK clock scaling (SMU DPM), on by default
- macOS Catalina through Sequoia; Tahoe with glitches
- Sleep and wake (D509BA Catalina–Monterey, Dell Tahoe): Linux DC's resume steps, a cache flush before S3 that macOS
  leaves out, and the AMD FCH's USB wake quirks; USB 2 and USB 3 keyboards wake it ([SLEEP.md](SLEEP.md))
- Brightness slider and keys (D509BA Monterey, Dell Tahoe), which also gives lid-close sleep on AC
- External HDMI (D509BA, Dell Tahoe), at boot and on hot-plug (README *HDMI*)

## Next, in the order the project set (2026-10-30)
1. **Sleep leftovers:** Sonoma with the cache-flush fix; a wake watchdog that allows for a slow GPU power-up
2. **HDMI audio**
3. **Video encode** (VCE), then decode (UVD needs physical-address translation, [VIDEO.md](VIDEO.md))
4. **GPU power management:** clock scaling that settles lower when idle, power gating; the ~20 s GPU power-up at
   every boot and wake (2 s waits in Apple's engine-control code, [SLEEP.md](SLEEP.md) *Also open*)
5. **Sierra to Mojave** support
6. **Tahoe graphics memory:** Apple's driver placing more allocations in system memory

## Later
- Carrizo / Bristol Ridge testing (code paths written, no hardware)
- GCN 2 APUs (Kaveri, Kabini, Mullins) in CarrizoRed, pure Lilu on the Polaris drivers ([OLDER_APUS.md](OLDER_APUS.md),
  *Plan*): a test machine that boots macOS first (Kabini's boot freeze, or a Kaveri desktop), then detection, DCE 8
  register remap, accelerator remaps, Metal's CI path
