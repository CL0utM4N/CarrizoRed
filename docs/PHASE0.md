# Phase 0 checklist

## Hardware

- [ ] **Motherboard:** A320 / B350 / X370 with the A12-9800 on its CPU support list *for the BIOS version
      installed*. Many B450 boards dropped Bristol Ridge in later BIOSes; B550/X570 never supported it.
- [ ] Board has HDMI + DVI and/or DisplayPort outputs.
- [ ] Dual-channel DDR4.
- [ ] Optional: COM header (serial kernel logging), cheap Polaris dGPU (RX 460/560/570) as a known-good
      reference for Apple's drivers.

## Firmware (BIOS) settings

- [ ] UMA Frame Buffer Size: maximum (1–2 GB)
- [ ] Primary display: IGD / iGPU
- [ ] CSM off, Secure Boot off, Above 4G decoding on if available
- [ ] Note the BIOS/AGESA version here: ________

## Linux (dual boot) — reference data

Boot a recent Linux live USB/install with `amdgpu` working on the iGPU, then save:

- [ ] `lspci -nnvvv -s 00:01.0` (confirm device ID and revision)
- [ ] `sudo dmesg | grep -i amdgpu` (firmware versions, VRAM size, CU count)
- [ ] VBIOS: `sudo cat /sys/kernel/debug/dri/0/amdgpu_vbios > vbios.rom` (or dump the ACPI VFCT table:
      `sudo cat /sys/firmware/acpi/tables/VFCT > vfct.bin`)
- [ ] Firmware versions: `sudo cat /sys/kernel/debug/dri/0/amdgpu_firmware_info`
- [ ] Register dump with [umr](https://gitlab.freedesktop.org/tomstdenis/umr) after amdgpu init:
      `sudo umr -O bits -r *.gfx80.*` and `sudo umr -O bits -r *.dce110.*` (adjust names to what
      `umr -lb` lists for the ASIC)

## macOS

- [ ] OpenCore with [AMD_Vanilla](https://github.com/AMD-OSX/AMD_Vanilla) patches (15h/16h set, core
      count patched for 4 cores), booting to desktop
- [ ] `ioreg -lw0 > ioreg.txt` and an IORegistryExplorer save
- [ ] Copy out for reverse engineering (from the exact macOS build you will test on):
  - `/System/Library/Extensions/AMDRadeonX4000.kext`
  - `/System/Library/Extensions/AMDRadeonX4000HWServices.kext`
  - `/System/Library/Extensions/AMD9500Controller.kext`
  - `/System/Library/Extensions/AMDSupport.kext`
  - On Big Sur+ these binaries live in the kernel collection; extract with
    `kmutil` / `ipsw kernel extract` or grab them from the KDK.
- [ ] Install the matching KDK (Kernel Debug Kit) for symbolised panic logs

## Reverse engineering — first targets

In Ghidra (or IDA), for each kext note the function addresses and what they do:

`AMD9500Controller` / `AMDSupport`:
- [ ] Device-ID → ASIC table (where `0x67EF` etc. map to "Baffin"/"Ellesmere")
- [ ] ASIC capability struct and how family/revision IDs are chosen
- [ ] VBIOS acquisition (`getVBIOS`-style routines, PCI ROM vs ACPI)
- [ ] VRAM size/type detection
- [ ] DCE register access helpers and the display-clock / PLL setup

`AMDRadeonX4000` / `AMDRadeonX4000HWServices`:
- [ ] Accelerator `start()` path and the per-ASIC subclass selection
- [ ] Firmware blob tables and the upload code (CP, RLC, SDMA, MEC)
- [ ] GMC / VM setup and VRAM manager initialisation
- [ ] Golden register tables
- [ ] Shader engine / CU configuration reads

## First test boots

1. Boot with `CarrizoRed.kext` (debug) + Lilu (debug), **no** device-id spoof. Collect the log lines and
   `ioreg` properties (`CarrizoRed,vram-mb` etc.) and send them over.
2. Add DeviceProperties for `PciRoot(0x0)/Pci(0x1,0x0)`: `device-id` = `<EF 67 00 00>`. Expect a panic or
   hang in Apple's kexts — that failure point is the starting point for Phase 2. Keep a USB with the
   unspoofed config to recover.
