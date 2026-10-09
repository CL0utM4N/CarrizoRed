//
//  kern_asic.hpp
//  CarrizoRed
//
//  Hardware constants for Carrizo-family (GFX8 / VI) APUs.
//  Register offsets are dword indices into the MMIO BAR (BAR5), as in Linux's
//  drivers/gpu/drm/amd/include/asic_reg headers (MIT licensed).
//

#ifndef kern_asic_hpp
#define kern_asic_hpp

#include <stdint.h>
#include <stddef.h>

enum class ChipFamily {
	Unknown,
	Carrizo,  // Carrizo and Bristol Ridge: gfx801, DCE 11.0, 8 CUs max
	Stoney,   // Stoney Ridge: gfx810, DCE 11.0, 3 CUs, single pipe-set
};

struct SupportedDevice {
	uint16_t deviceId;
	ChipFamily family;
};

// From amdgpu_drv.c (CHIP_CARRIZO / CHIP_STONEY, AMD_IS_APU).
// Bristol Ridge (e.g. A12-9800) reports 0x9874 and is driven as Carrizo.
static constexpr SupportedDevice SupportedDevices[] {
	{0x9870, ChipFamily::Carrizo},
	{0x9874, ChipFamily::Carrizo},
	{0x9875, ChipFamily::Carrizo},
	{0x9876, ChipFamily::Carrizo},
	{0x9877, ChipFamily::Carrizo},
	{0x98E4, ChipFamily::Stoney},
};

inline ChipFamily familyForDevice(uint16_t deviceId) {
	for (auto &dev : SupportedDevices)
		if (dev.deviceId == deviceId)
			return dev.family;
	return ChipFamily::Unknown;
}

// Display pipes (CRTC + line buffer each) per family, from Linux DC carrizo_resource_cap / stoney_resource_cap.
// Apple's BaffinSharedController assumes 6 (getDisplayLimit, getNumLineBuffers).
inline uint8_t displayPipes(ChipFamily family) {
	switch (family) {
		case ChipFamily::Carrizo:
			return 3;
		case ChipFamily::Stoney:
			return 2;
		default:
			return 0;
	}
}

inline const char *familyName(ChipFamily family) {
	switch (family) {
		case ChipFamily::Carrizo:
			return "Carrizo/Bristol Ridge";
		case ChipFamily::Stoney:
			return "Stoney Ridge";
		default:
			return "Unknown";
	}
}

namespace Reg {
	// bif_5_0_d.h
	static constexpr uint32_t CONFIG_MEMSIZE = 0x150A;  // Carve-out ("VRAM") size in MB
	// gmc_8_1_d.h
	static constexpr uint32_t MC_VM_FB_LOCATION = 0x809;  // [15:0] FB base >> 24, [31:16] FB top >> 24
	static constexpr uint32_t MC_VM_FB_OFFSET = 0x81A;    // System physical address of carve-out >> 22 (APU only)
	// gfx_8_0_d.h
	static constexpr uint32_t GRBM_STATUS = 0x2004;
	static constexpr uint32_t GB_ADDR_CONFIG = 0x263E;
	static constexpr uint32_t CC_GC_SHADER_ARRAY_CONFIG = 0x226F;
	static constexpr uint32_t CC_RB_BACKEND_DISABLE = 0x263D;
}

// Apple's Polaris (Baffin, RX 460/560) device ID, the closest supported personality for
// AMD9500Controller/AMDRadeonX4000. Spoofing is done via OpenCore DeviceProperties for now.
static constexpr uint16_t PolarisSpoofDeviceId = 0x67EF;

#endif /* kern_asic_hpp */
