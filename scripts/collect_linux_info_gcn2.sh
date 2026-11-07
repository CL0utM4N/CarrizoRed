#!/bin/bash
#
#  Collect reference data for UnifiedRed (GCN 2 APUs: Kabini, Mullins, Kaveri) from a Linux live session on the test machine.
#  Read-only: nothing on the machine is changed.
#
#    sudo bash collect_linux_info.sh
#
#  Produces unifiedred-gcn2-dump-<host>-<date>.tar.gz next to this script (or in $HOME if that
#  location is read-only), containing:
#    - hardware: lspci (incl. the GPU's full config space), dmidecode, CPU, memory, USB
#    - ACPI tables (DSDT, SSDTs, VFCT, ...), also used to build the OpenCore EFI
#    - GPU: VBIOS, kernel driver log, firmware versions, VRAM/GTT sizes, and selected registers
#
#  Linux drives Kabini/Mullins/Kaveri with the old `radeon` driver unless told otherwise. For the most data
#  (firmware versions, the register file), boot the live session with these kernel parameters:
#      radeon.cik_support=0 amdgpu.cik_support=1
#  For the debugfs files Secure Boot must be disabled in the firmware setup, otherwise kernel lockdown hides
#  them. The VBIOS is in the ACPI VFCT table either way.
#
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

name="unifiedred-gcn2-dump-$(hostname)-$(date +%Y%m%d-%H%M)"
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
dmesg | grep -iE "amdgpu|\[drm\]|VFCT|radeon" >"$out/gpu/dmesg-gpu.txt" 2>&1
[ -n "$gpu" ] && basename "$(readlink -f "/sys/bus/pci/devices/$gpu/driver")" >"$out/gpu/driver.txt" 2>&1
grep -o -w -E 'avx2|bmi2|fma|avx|sse4_2|movbe' /proc/cpuinfo | sort | uniq -c >"$out/cpu-flags.txt"
[ -s "$out/gpu/vbios.rom" ] || echo "No debugfs VBIOS: is Secure Boot disabled? (VFCT is in acpi/ anyway)" \
  >"$out/gpu/vbios-missing.txt"

# Registers (read-only), through amdgpu's register file: dword index * 4 = byte offset. CIK offsets (gfx_7_2_d.h,
# gmc_7_1_d.h, bif_4_1_d.h, dce_8_0_d.h, cikd.h).
if [ -n "${regs:-}" ] && [ -r "$regs" ]; then
  rd() {  # rd <name> <dword index>
    local v
    v=$(dd if="$regs" bs=4 skip=$(($2)) count=1 2>/dev/null | od -An -tx4 | tr -d ' ')
    printf '%-34s 0x%04X  %s
' "$1" $(($2)) "${v:-<read failed>}" >>"$out/gpu/registers.txt"
  }
  echo "# name                               index   value (amdgpu_regs, $(date))" >"$out/gpu/registers.txt"
  rd GRBM_STATUS 0x2004
  rd SRBM_STATUS 0x394
  rd GB_ADDR_CONFIG 0x263E
  rd CC_RB_BACKEND_DISABLE 0x263D
  rd GC_USER_RB_BACKEND_DISABLE 0x26DF
  rd CC_GC_SHADER_ARRAY_CONFIG 0x226F
  rd GC_USER_SHADER_ARRAY_CONFIG 0x2270
  rd PA_SC_RASTER_CONFIG 0xA0D4
  rd PA_SC_RASTER_CONFIG_1 0xA0D5
  for i in $(seq 0 31); do rd "GB_TILE_MODE$i" $((0x2644 + i)); done
  for i in $(seq 0 15); do rd "GB_MACROTILE_MODE$i" $((0x2664 + i)); done
  rd CONFIG_MEMSIZE 0x150A
  rd MC_VM_FB_LOCATION 0x809
  rd MC_VM_FB_OFFSET 0x81A
  rd MC_VM_SYSTEM_APERTURE_LOW_ADDR 0x80D
  rd MC_VM_SYSTEM_APERTURE_HIGH_ADDR 0x80E
  rd MC_SHARED_CHMAP 0x801
  rd MC_ARB_RAMCFG 0x9D8
  rd MC_FUS_DRAM0_BANK_ADDR_MAPPING 0xA11
  rd MC_FUS_DRAM1_BANK_ADDR_MAPPING 0xA12
  rd DMIF_ADDR_CONFIG 0x2F5
  rd DMIF_ADDR_CALC 0x300
  rd PIPE0_DMIF_BUFFER_CONTROL 0x328
  rd PIPE1_DMIF_BUFFER_CONTROL 0x330
  for p in 0 1; do
    o=$((p * 0x300))
    rd "CRTC${p}_CRTC_CONTROL" $((0x1B9C + o))
    rd "CRTC${p}_CRTC_H_TOTAL" $((0x1B80 + o))
    rd "CRTC${p}_CRTC_V_TOTAL" $((0x1B87 + o))
    rd "GRPH${p}_ENABLE" $((0x1A00 + o))
    rd "GRPH${p}_CONTROL" $((0x1A01 + o))
    rd "GRPH${p}_PRIMARY_SURFACE_ADDRESS" $((0x1A04 + o))
    rd "GRPH${p}_PRIMARY_SURFACE_ADDRESS_HIGH" $((0x1A07 + o))
    rd "GRPH${p}_PITCH" $((0x1A06 + o))
    rd "LB${p}_LB_MEMORY_CTRL" $((0x1AC1 + o))
  done
  rd LVTMA_PWRSEQ_CNTL 0x1919
  rd LVTMA_PWRSEQ_STATE 0x191A
  rd BL_PWM_CNTL 0x191E
else
  echo "No amdgpu register file (driver: $(cat "$out/gpu/driver.txt" 2>/dev/null))." \
    "Boot with radeon.cik_support=0 amdgpu.cik_support=1 for the register dump." >"$out/gpu/registers-missing.txt"
fi

# --- Pack ---------------------------------------------------------------------------------
tar -C "$base" -czf "$base/$name.tar.gz" "$name" && rm -rf "$out"
sync
echo
echo "Done: $base/$name.tar.gz"
echo "Copy it to the Windows PC (e.g. into Desktop\KaberiRed-RE\) and tell Claude where it is."
