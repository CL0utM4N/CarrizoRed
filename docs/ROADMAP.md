# Roadmap

Updated 2026-10-31, at the 0.1.0 pre-release.

## Done
- Lilu plugin, iGPU detection, VBIOS from ACPI VFCT, device-id spoof to Polaris 11 (`0x67EF`)
- Display: built-in eDP panel through Apple's `AMD9500Controller` (DCE 11.0 pipes, PLLs, DIGs, line buffer, eDP link)
- Acceleration: `AMDRadeonX4000` with an SMU8 microcode loader, VRAM window at 0, IH ring in VRAM, Stoney caps
- Metal: shader fix for Stoney's 16-bank LDS (including Metal's SDMA re-uploads), Stoney compiler back end
- SCLK clock scaling (SMU DPM), on by default
- macOS Catalina through Sequoia; Tahoe with glitches
- Sleep and wake on Catalina (D509BA): Linux DC's resume steps ([SLEEP.md](SLEEP.md))

## Next, in the order the project set (2026-10-30)
1. **Sleep:** Sonoma's wake hang (on hold, next steps in [SLEEP.md](SLEEP.md)); lid sleep; the other machines and
   macOS versions
2. **Video encode** (VCE), then decode (UVD needs physical-address translation, [VIDEO.md](VIDEO.md))
3. **Brightness keys:** the eDP panel as an `AppleBacklightDisplay` (also what lid sleep needs)
4. **GPU power management:** clock scaling that settles lower when idle, power gating
5. **Sierra to Mojave** support
6. **Tahoe graphics memory:** Apple's driver placing more allocations in system memory

## Later
- Carrizo / Bristol Ridge testing (code paths written, no hardware)
- External displays (HDMI), HDMI audio
- GCN 2 APUs ([OLDER_APUS.md](OLDER_APUS.md))
