# Test machines

Hardware facts gathered with [`scripts/collect_linux_info.sh`](../scripts/collect_linux_info.sh) from a Linux live
session. VBIOS table revisions come from [`scripts/atom_tables.py`](../scripts/atom_tables.py).

## ASUS VivoBook 15 D509BA (X509BA), A9-9425: Stoney Ridge

Collected 2026-10-09, Ubuntu live, kernel per `uname.txt` in the dump (`carrizored-dump-ubuntu-20261009-0215.tar.gz`,
kept outside the repo in `Desktop/CarrizoRed-RE/`).

### Platform
| | |
|---|---|
| BIOS | `X509BA.301` |
| CPU | AMD A9-9425 (Family 15h model 70h, Stoney, Excavator), 2 cores / 2 threads |
| RAM | 8 GB DDR4 (7.2 GB visible after the 512 MB carve-out) |
| Storage | WD PC SN520 512 GB NVMe (`15b7:5003`), currently Linux (EFI + ext4) |
| Wi-Fi | Qualcomm Atheros QCA9377 (`168c:0042`): **no macOS driver** |
| USB | FCH XHCI `1022:7914`, EHCI `1022:7908` |
| SATA | FCH AHCI `1022:7901` (empty 2.5" bay) |
| Audio | GPU HDMI `1002:15b3`, platform `1022:157a` |
| ACPI | `VFCT` present (64,644 bytes), `CRAT`, `IVRS`, 4 SSDTs. Full set in the dump `acpi/` |

### GPU
| | |
|---|---|
| PCI | `1002:98E4` rev **`0xDA`**, subsystem `1043:151F`, at `00:01.0` |
| Linux IP blocks | vi_common, gmc_v8_0, cz_ih, **gfx_v8_1**, sdma_v3_0 (1 instance), smu8, DCE 11.0, uvd_v6_2, vce_v3_4, acp |
| Carve-out ("VRAM") | **512 MB**, GPU address `0xF4_0000_0000`–`0xF4_1FFF_FFFF` (`MC_VM_FB_LOCATION` is high) |
| BARs | BAR0 **256 MB** at `0xE0000000` (smaller than the 512 MB carve-out), BAR2 8 MB doorbell, BAR4 I/O, BAR5 256 KB MMIO at `0xFEB00000` |
| GTT | 3,692 MB |
| VBIOS | `SWBRT46974.001`, 64,512 bytes, VFCT copy identical to debugfs `amdgpu_vbios`. VFCT image header: `00:01.0 1002:98E4`, revision field 1 |
| Firmware | SMU 26.17.0, ME 0x3A, PFP 0x58, CE 0x32, MEC 0xA1, RLC 0x4F, SDMA0 0x26, UVD 1.43, VCE 52.0 |

### VBIOS command tables that matter for Apple's handlers
| Table | Revision | Apple's handler |
|---|---|---|
| `SetPixelClock` | **1.6** | `AtiSetPixelClock_V1_V6` exists (dividers from `hwCalculateBasicClockParameters`) |
| `AdjustDisplayPll` | **1.3** | `AtiAdjustDisplayPll_V3` exists |
| `EnableSpreadSpectrumOnPPLL` | **1.3** | `AtiEnableSpreadSpectrumOnPll_V3` exists |
| `DIG1TransmitterControl` | **1.5** | `AtiTransmitterControl_V1_V5` exists |
| `SetDCEClock` | **absent** | DCE 11.0 has no `SetDCEClock` (it was added in DCE 11.1+). Check what `BaffinDisplayEngineClock::SetDisplayEngineClock` calls: if it needs `SetDCEClock`, the display engine clock must go through `SetPixelClock` V6 (`AtiSetPixelClock_V1_V6::setDisplayClock(uchar, uint)` exists) |
| `DIGxEncoderControl` / `DIG2EncoderControl` | 1.4 / 1.1 | to check |
| `SelectCRTC_Source` | 1.3 | to check |
| `EnableGraphSurfaces` / `SetCRTC_UsingDTDTiming` | 1.4 / 2.3 | to check |
| Data: `FirmwareInfo` 2.2, `Object_Header` 1.3, `IntegratedSystemInfo` 1.9, `PowerPlayInfo` 6.1, `ASIC_InternalSS_Info` 3.1 | | |

### macOS install log
- **2026-10-09, attempts 1–2 (MacPro7,1 SMBIOS):** recovery (24E263) boots to userspace, keyboard works, then
  panics ~3.5 min in: page fault `CR2 0x8` in `AppleIntelMCEReporter::start → AppleIntelMCE::Controller::controller`.
  That kext matches the board-ids of MacPro6,1 / iMacPro1,1 / MacPro7,1 and crashes on AMD CPUs. It's in the system
  KC, so the fix is a different SMBIOS: **iMac19,1**. The panic text was read back from NVRAM (`AAPL,PanicInfo*`) via
  `dmpstore -all -s FS0:\nvram.bin` in OpenShell, then unpacked from Apple's 7-bit format.
- **Attempt 3 (iMac19,1):** reaches the macOS Recovery UI, but the PS/2 keyboard and all USB ports are dead. The
  DSDT's `OSFL()` sets `OSVR` from `_OSI("Windows …")`; macOS answers only `Darwin`, so `OSVR = 3` and the HPET's
  `_STA` reports it disabled (needs `OSVR >= 0x0C`) and clears `HPEN`. Fix: `SSDT-XOSI` + `_OSI → XOSI` rename
  (DSDT, 24 hits) so the firmware sees Windows 10 under macOS. HPET `_CRS` already holds IRQ 0/8 and RTC drops its IRQ
  when `HPEN` = 1, so no IRQ patching is needed.
- The `shared_region … vm_shared_region_start_address() failed` and launchd `error = 37` lines in verbose recovery
  are noise; processes keep running.

### Consequences
- The CarrizoRed VFCT injector will match this image (vendor/device/slot/function `1002:98E4` `00:01.0`).
- The pixel-clock path Apple uses for DCE 11.0 VBIOSes is the one this VBIOS needs.
- **New item:** no `SetDCEClock` table, so check Apple's display-engine-clock path.
- **Phase 3 note:** BAR0 (256 MB) is smaller than the carve-out (512 MB). Apple's accelerator must not assume the
  whole "VRAM" is CPU-visible through BAR0 (Linux maps the carve-out directly at `MC_VM_FB_OFFSET << 22`).
- For macOS: no Wi-Fi (QCA9377), so use a USB Ethernet/Wi-Fi adapter or iPhone USB tethering if needed. Use a USB mouse
  (the I²C trackpad is unlikely to work on AMD without extra work).
