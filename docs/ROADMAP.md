# Roadmap

Updated 2026-11-03.

## Done
- Lilu plugin, iGPU detection, VBIOS from ACPI VFCT, device-id spoof to Polaris 11 (`0x67EF`)
- Display: built-in eDP panel through Apple's `AMD9500Controller` (DCE 11.0 pipes, PLLs, DIGs, line buffer, eDP link)
- Acceleration: `AMDRadeonX4000` with an SMU8 microcode loader, VRAM window at 0, IH ring in VRAM, Stoney caps
- Metal: shader fix for Stoney's 16-bank LDS (including Metal's SDMA re-uploads), Stoney compiler back end
- SCLK clock scaling (SMU DPM), on by default
- macOS Catalina through Sequoia; Tahoe with glitches
- Sleep and wake on Catalina, Big Sur and Monterey (D509BA): Linux DC's resume steps, and a cache flush before S3
  that macOS leaves out ([SLEEP.md](SLEEP.md))

## Next, in the order the project set (2026-10-30)
1. **Sleep:** Sonoma with the cache-flush fix (Monterey fixed 2026-11-03, [SLEEP.md](SLEEP.md)); a wake watchdog
   that allows for a slow GPU power-up; lid sleep; the other machines and macOS versions
2. **Video encode** (VCE), then decode (UVD needs physical-address translation, [VIDEO.md](VIDEO.md))
3. **Brightness keys:** the eDP panel as an `AppleBacklightDisplay` (also what lid sleep needs)
4. **GPU power management:** clock scaling that settles lower when idle, power gating; the ~20 s GPU power-up at
   every boot and wake (2 s waits in Apple's engine-control code, [SLEEP.md](SLEEP.md) *Also open*)
5. **Sierra to Mojave** support
6. **Tahoe graphics memory:** Apple's driver placing more allocations in system memory

## Later
- Carrizo / Bristol Ridge testing (code paths written, no hardware)
- External displays (HDMI), HDMI audio
- GCN 2 APUs ([OLDER_APUS.md](OLDER_APUS.md))
