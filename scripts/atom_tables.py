#!/usr/bin/env python3
"""
List the command and data tables in an AMD ATOM BIOS image (VBIOS), with their revisions.

    python scripts/atom_tables.py vbios.rom          # a VBIOS image (e.g. from amdgpu_vbios)
    python scripts/atom_tables.py VFCT.aml           # or the ACPI VFCT table: every image inside is listed

Apple's AMDSupport picks its command-table handler classes from these revisions (for example
SetPixelClock content revision 5/6/7 → AtiSetPixelClock_V1_V5/V6/V7), so this tells us which code paths a
given VBIOS will use. Table order and header layout follow Linux drivers/gpu/drm/amd/include/atombios.h (MIT):
ATOM_ROM_HEADER, ATOM_MASTER_LIST_OF_COMMAND_TABLES, ATOM_MASTER_LIST_OF_DATA_TABLES, ATOM_COMMON_TABLE_HEADER.
"""

import struct
import sys
from pathlib import Path

COMMAND_TABLES = """ASIC_Init GetDisplaySurfaceSize ASIC_RegistersInit VRAM_BlockVenderDetection DIGxEncoderControl
MemoryControllerInit EnableCRTCMemReq MemoryParamAdjust DVOEncoderControl GPIOPinControl SetEngineClock SetMemoryClock
SetPixelClock EnableDispPowerGating ResetMemoryDLL ResetMemoryDevice MemoryPLLInit AdjustDisplayPll
AdjustMemoryController EnableASIC_StaticPwrMgt SetUniphyInstance DAC_LoadDetection LVTMAEncoderControl
HW_Misc_Operation DAC1EncoderControl DAC2EncoderControl DVOOutputControl CV1OutputControl GetConditionalGoldenSetting
SMC_Init PatchMCSetting MC_SEQ_Control Gfx_Harvesting EnableScaler BlankCRTC EnableCRTC GetPixelClock
EnableVGA_Render GetSCLKOverMCLKRatio SetCRTC_Timing SetCRTC_OverScan GetSMUClockInfo SelectCRTC_Source
EnableGraphSurfaces UpdateCRTC_DoubleBufferRegisters LUT_AutoFill SetDCEClock GetMemoryClock GetEngineClock
SetCRTC_UsingDTDTiming ExternalEncoderControl LVTMAOutputControl VRAM_BlockDetectionByStrap MemoryCleanUp
ProcessI2cChannelTransaction WriteOneByteToHWAssistedI2C ReadHWAssistedI2CStatus SpeedFanControl
PowerConnectorDetection MC_Synchronization ComputeMemoryEnginePLL Gfx_Init VRAM_GetCurrentInfoBlock
DynamicMemorySettings MemoryTraining EnableSpreadSpectrumOnPPLL TMDSAOutputControl SetVoltage DAC1OutputControl
ReadEfuseValue ComputeMemoryClockParam ClockSource MemoryDeviceInit GetDispObjectInfo DIG1EncoderControl
DIG2EncoderControl DIG1TransmitterControl DIG2TransmitterControl ProcessAuxChannelTransaction DPEncoderService
GetVoltageInfo""".split()

DATA_TABLES = """UtilityPipeLine MultimediaCapabilityInfo MultimediaConfigInfo StandardVESA_Timing FirmwareInfo PaletteData
LCD_Info DIGTransmitterInfo SMU_Info SupportedDevicesInfo GPIO_I2C_Info VRAM_UsageByFirmware GPIO_Pin_LUT
VESA_ToInternalModeLUT GFX_Info PowerPlayInfo GPUVirtualizationInfo SaveRestoreInfo PPLL_SS_Info OemInfo XTMDS_Info
MclkSS_Info Object_Header IndirectIOAccess MC_InitParameter ASIC_VDDC_Info ASIC_InternalSS_Info TV_VideoMode
VRAM_Info MemoryTrainingInfo IntegratedSystemInfo ASIC_ProfilingInfo VoltageObjectInfo PowerSourceInfo
ServiceInfo""".split()

# Tables Apple's AMDSupport has revision-specific handlers for, and the revisions seen so far (see docs/APPLE_KEXTS.md).
APPLE_HANDLERS = {"SetPixelClock": {5, 6, 7}}


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def table_header(b, off):
    """ATOM_COMMON_TABLE_HEADER: usStructureSize, ucTableFormatRevision, ucTableContentRevision."""
    if off == 0 or off + 4 > len(b):
        return None
    size, frev, crev = struct.unpack_from("<HBB", b, off)
    return size, frev, crev


def dump_image(b, label):
    if len(b) < 0x4A or b[0] != 0x55 or b[1] != 0xAA:
        print(f"{label}: not a VBIOS image (no 55 AA)")
        return
    rom = u16(b, 0x48)
    if b[rom + 4:rom + 8] not in (b"ATOM", b"MOTA"):
        print(f"{label}: no ATOM signature at ROM header 0x{rom:X}")
        return
    sub_vendor, sub_id = u16(b, rom + 0x18), u16(b, rom + 0x1A)
    cmd_master, data_master = u16(b, rom + 0x1E), u16(b, rom + 0x20)
    rom_size = b[2] * 512
    # Part number / build string: printable run right after the ROM header area is not fixed, so search near the start.
    print(f"== {label}: {len(b)} bytes (ROM declares {rom_size}), ROM header 0x{rom:X}, "
          f"subsystem {sub_vendor:04X}:{sub_id:04X}")

    print(f"\nCommand tables (master list at 0x{cmd_master:X}):")
    print(f"  {'name':34s} {'offset':>8} {'size':>6}  rev")
    for i, name in enumerate(COMMAND_TABLES):
        off = u16(b, cmd_master + 4 + 2 * i)
        hdr = table_header(b, off)
        if not hdr:
            continue
        size, frev, crev = hdr
        note = ""
        if name in APPLE_HANDLERS:
            note = "  <- Apple handler exists" if crev in APPLE_HANDLERS[name] else "  <- NO Apple handler for this revision"
        print(f"  {name:34s} 0x{off:06X} {size:6d}  {frev}.{crev}{note}")

    print(f"\nData tables (master list at 0x{data_master:X}):")
    print(f"  {'name':34s} {'offset':>8} {'size':>6}  rev")
    for i, name in enumerate(DATA_TABLES):
        off = u16(b, data_master + 4 + 2 * i)
        hdr = table_header(b, off)
        if not hdr:
            continue
        size, frev, crev = hdr
        print(f"  {name:34s} 0x{off:06X} {size:6d}  {frev}.{crev}")
    print()


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    data = Path(sys.argv[1]).read_bytes()
    if data[:4] == b"VFCT":
        # UEFI_ACPI_VFCT: VBIOSImageOffset at 0x34; each image is a 0x1C-byte VFCT_IMAGE_HEADER + data.
        off, length = u16(data, 0x34) | (u16(data, 0x36) << 16), struct.unpack_from("<I", data, 4)[0]
        while off + 0x1C <= length:
            bus, dev, fn, ven, did, ssv, ssid, rev, ilen = struct.unpack_from("<IIIHHHHII", data, off)
            if ilen == 0 or off + 0x1C + ilen > length:
                break
            dump_image(data[off + 0x1C:off + 0x1C + ilen],
                       f"VFCT image {bus:02X}:{dev:02X}.{fn} {ven:04X}:{did:04X} ({ssv:04X}:{ssid:04X}) rev 0x{rev:X}")
            off += 0x1C + ilen
    else:
        dump_image(data, sys.argv[1])


if __name__ == "__main__":
    main()
