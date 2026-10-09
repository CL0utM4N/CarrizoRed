#!/bin/bash
#
#  Collect reference data for CarrizoRed from a Linux live session on the test machine.
#  Read-only: nothing on the machine is changed.
#
#    sudo bash collect_linux_info.sh
#
#  Produces carrizored-dump-<host>-<date>.tar.gz next to this script (or in $HOME if that
#  location is read-only), containing:
#    - hardware: lspci (incl. the GPU's full config space), dmidecode, CPU, memory, USB
#    - ACPI tables (DSDT, SSDTs, VFCT, ...), also used to build the OpenCore EFI
#    - GPU: VBIOS, amdgpu boot log, firmware versions, VRAM/GTT sizes, and selected registers via umr
#
#  For the debugfs files (VBIOS, firmware info) Secure Boot must be disabled in the firmware
#  setup, otherwise kernel lockdown hides them.
#
#  Optional, for the register dump: Fedora `sudo dnf install -y umr`, Ubuntu `sudo apt install umr`.
#  Works from Fedora and Ubuntu live sessions (and installed systems).
#

set -u

if [ "$(id -u)" -ne 0 ]; then
  echo "Run as root: sudo bash $0"
  exit 1
fi

here="$(cd "$(dirname "$0")" && pwd)"
# Live USBs are mounted read-only (Ubuntu: /cdrom, Fedora: /run/initramfs/live). If the script
# runs from there, try to make that mount writable so the output lands on the stick.
if [ ! -w "$here" ]; then
  mp=$(findmnt -n -o TARGET --target "$here" 2>/dev/null)
  [ -n "$mp" ] && [ "$mp" != "/" ] && mount -o remount,rw "$mp" 2>/dev/null
fi
base="$here"
[ -w "$base" ] || base="${SUDO_USER:+/home/$SUDO_USER}"
[ -n "$base" ] && [ -w "$base" ] || base="/root"

name="carrizored-dump-$(hostname)-$(date +%Y%m%d-%H%M)"
out="$base/$name"
mkdir -p "$out"/{acpi,gpu}
echo "Collecting into $out"

run() {  # run <outfile> <command...>
  local file="$1"; shift
  { echo "\$ $*"; "$@"; } >"$out/$file" 2>&1
}

# --- System -------------------------------------------------------------------------------
run uname.txt uname -a
run os-release.txt cat /etc/os-release
run cpuinfo.txt cat /proc/cpuinfo
run meminfo.txt cat /proc/meminfo
run cmdline.txt cat /proc/cmdline
run lspci-nn.txt lspci -nn
run lspci-full.txt lspci -nnvvvk
run lsusb.txt lsusb
run lsblk.txt lsblk -o NAME,SIZE,TYPE,TRAN,MODEL,FSTYPE,MOUNTPOINT
command -v dmidecode >/dev/null && run dmidecode.txt dmidecode
run dmesg.txt dmesg
run mokutil.txt mokutil --sb-state

# --- ACPI tables --------------------------------------------------------------------------
for t in /sys/firmware/acpi/tables/*; do
  [ -f "$t" ] && cp "$t" "$out/acpi/$(basename "$t").aml" 2>/dev/null
done
for t in /sys/firmware/acpi/tables/dynamic/*; do
  [ -f "$t" ] && cp "$t" "$out/acpi/dynamic-$(basename "$t").aml" 2>/dev/null
done

# --- GPU ----------------------------------------------------------------------------------
gpu=$(lspci -D -d 1002: -n 2>/dev/null | awk '$2 ~ /^03/ {print $1; exit}')
if [ -n "$gpu" ]; then
  echo "AMD GPU at $gpu"
  echo "$gpu" >"$out/gpu/pci-address.txt"
  run gpu/config-space.txt lspci -xxxx -s "$gpu"
  dev="/sys/bus/pci/devices/$gpu"
  for f in vendor device revision subsystem_vendor subsystem_device class resource \
           mem_info_vram_total mem_info_vis_vram_total mem_info_gtt_total vbios_version \
           current_link_speed current_link_width; do
    [ -r "$dev/$f" ] && echo "$f: $(cat "$dev/$f" 2>&1)" >>"$out/gpu/sysfs.txt"
  done
  # Also keeps the ROM-sysfs route for machines where debugfs is locked down.
  if [ -e "$dev/rom" ]; then
    echo 1 >"$dev/rom" 2>/dev/null && cat "$dev/rom" >"$out/gpu/pci-rom.bin" 2>/dev/null
    echo 0 >"$dev/rom" 2>/dev/null
    [ -s "$out/gpu/pci-rom.bin" ] || rm -f "$out/gpu/pci-rom.bin"
  fi
fi

mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug 2>/dev/null
for d in /sys/kernel/debug/dri/*; do
  [ -e "$d/amdgpu_firmware_info" ] || continue
  cp "$d/amdgpu_vbios" "$out/gpu/vbios.rom" 2>/dev/null
  cat "$d/amdgpu_firmware_info" >"$out/gpu/firmware-info.txt" 2>&1
  cat "$d/amdgpu_vram_mm" >"$out/gpu/vram-mm.txt" 2>/dev/null
  cat "$d/amdgpu_gtt_mm" >"$out/gpu/gtt-mm.txt" 2>/dev/null
  cat "$d/amdgpu_pm_info" >"$out/gpu/pm-info.txt" 2>/dev/null
  regs="$d/amdgpu_regs"
  break
done
dmesg | grep -iE "amdgpu|\[drm\]|VFCT|radeon" >"$out/gpu/dmesg-amdgpu.txt" 2>&1
[ -s "$out/gpu/vbios.rom" ] || echo "No debugfs VBIOS: is Secure Boot disabled? (VFCT is in acpi/ anyway)" \
  >"$out/gpu/vbios-missing.txt"

# umr (read-only register reads). Install with: sudo dnf install umr / sudo apt install umr
if command -v umr >/dev/null; then
  run gpu/umr-blocks.txt umr -lb
  for r in mmGB_ADDR_CONFIG mmGB_TILE_MODE0 mmGB_MACROTILE_MODE0 mmPA_SC_RASTER_CONFIG \
           mmPA_SC_RASTER_CONFIG_1 mmCC_RB_BACKEND_DISABLE mmGC_USER_RB_BACKEND_DISABLE \
           mmCC_GC_SHADER_ARRAY_CONFIG mmGC_USER_SHADER_ARRAY_CONFIG mmMC_VM_FB_LOCATION \
           mmMC_VM_FB_OFFSET mmCONFIG_MEMSIZE mmMC_ARB_RAMCFG mmMC_VM_SYSTEM_APERTURE_LOW_ADDR \
           mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR mmDMIF_ADDR_CONFIG mmLB_MEMORY_CTRL \
           mmPIXCLK0_RESYNC_CNTL mmPIXCLK1_RESYNC_CNTL mmPHYPLL_PIXCLK_CNTL mmCRTC_CONTROL \
           mmCRTC1_CRTC_CONTROL mmCRTC2_CRTC_CONTROL; do
    echo "== $r" >>"$out/gpu/umr-registers.txt"
    umr -O bits -r "*.*.$r" >>"$out/gpu/umr-registers.txt" 2>&1
  done
elif [ -n "${regs:-}" ] && [ -r "$regs" ]; then
  # Fallback without umr: amdgpu_regs returns the MMIO register at byte offset (dword index * 4).
  # Offsets are VI/Carrizo-family (gfx_8_0_d.h, gmc_8_1_d.h, bif_5_0_d.h, dce_11_0_d.h); read-only.
  rd() {  # rd <name> <dword index>
    local v
    v=$(dd if="$regs" bs=4 skip=$(($2)) count=1 2>/dev/null | od -An -tx4 | tr -d ' ')
    printf '%-34s 0x%04X  %s\n' "$1" $(($2)) "${v:-<read failed>}" >>"$out/gpu/registers.txt"
  }
  echo "# name                               index   value (amdgpu_regs, $(date))" >"$out/gpu/registers.txt"
  rd GRBM_STATUS 0x2004
  rd GB_ADDR_CONFIG 0x263E
  rd CC_RB_BACKEND_DISABLE 0x263D
  rd CC_GC_SHADER_ARRAY_CONFIG 0x226F
  for i in $(seq 0 31); do rd "GB_TILE_MODE$i" $((0x2644 + i)); done
  for i in $(seq 0 15); do rd "GB_MACROTILE_MODE$i" $((0x2664 + i)); done
  rd CONFIG_MEMSIZE 0x150A
  rd MC_VM_FB_LOCATION 0x809
  rd MC_VM_FB_OFFSET 0x81A
  rd MC_VM_SYSTEM_APERTURE_LOW_ADDR 0x80D
  rd MC_VM_SYSTEM_APERTURE_HIGH_ADDR 0x80E
  rd MC_ARB_RAMCFG 0x9D8
  rd DMIF_ADDR_CONFIG 0x2F5
  rd LB0_LB_MEMORY_CTRL 0x1AC1
  rd LB1_LB_MEMORY_CTRL 0x1CC1
  rd LB2_LB_MEMORY_CTRL 0x1EC1
  rd CRTC0_CRTC_CONTROL 0x1B9C
  rd CRTC1_CRTC_CONTROL 0x1D9C
  rd CRTC2_CRTC_CONTROL 0x1F9C
  rd GRPH0_CONTROL 0x1A01
  rd GRPH0_PRIMARY_SURFACE_ADDRESS 0x1A04
  rd PIXCLK0_RESYNC_CNTL 0x13A
  rd PIXCLK1_RESYNC_CNTL 0x138
  rd PIXCLK2_RESYNC_CNTL 0x139
  rd PHYPLL_PIXCLK_CNTL 0x13E
  for i in $(seq 0 17); do rd "PLL0_REG+$i" $((0x1700 + i)); done
else
  echo "Neither umr nor amdgpu_regs available, register dump skipped" >"$out/gpu/registers-missing.txt"
fi

# --- Pack ---------------------------------------------------------------------------------
tar -C "$base" -czf "$base/$name.tar.gz" "$name" && rm -rf "$out"
sync
echo
echo "Done: $base/$name.tar.gz"
echo "Copy it to the Windows PC (e.g. into Desktop\\CarrizoRed-RE\\) and tell Claude where it is."
