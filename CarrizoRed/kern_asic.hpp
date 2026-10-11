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

// Pixel PLLs (Linux DC num_pll: PLL0, PLL1) and DIG stream encoders (num_stream_encoder) on both families. Apple's
// assigner is created with 7 PLLs and BaffinSharedController::getDigLimit() returns 6.
static constexpr uint8_t DisplayPlls = 2;
static constexpr uint32_t DisplayDigs = 3;

// DCE 11.0 per-pipe register blocks (DCP, LB, DCFE, DMIF_PG, SCL, BLND, CRTC, FMT): 0x200 dwords each, pipes 0-2 from
// 0x1A00, pipes 3-5 from 0x4000 (dce_11_0_d.h). Returns -1 for a register outside them.
inline int pipeOfRegister(uint32_t dword) {
	if (dword >= 0x1A00 && dword < 0x2000)
		return static_cast<int>((dword - 0x1A00) / 0x200);
	if (dword >= 0x4000 && dword < 0x4600)
		return 3 + static_cast<int>((dword - 0x4000) / 0x200);
	return -1;
}

inline uint32_t pipeRegisterBase(uint32_t pipe) { return pipe < 3 ? 0x200 * pipe : 0x2600 + 0x200 * (pipe - 3); }

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
	static constexpr uint32_t MC_VM_SYSTEM_APERTURE_LOW_ADDR = 0x80D;
	static constexpr uint32_t MC_VM_SYSTEM_APERTURE_HIGH_ADDR = 0x80E;
	// oss_3_0_1_d.h
	static constexpr uint32_t HDP_NONSURFACE_BASE = 0xB01;
	static constexpr uint32_t HDP_NONSURFACE_INFO = 0xB02;
	static constexpr uint32_t HDP_NONSURFACE_SIZE = 0xB03;
	// dce_11_0_d.h, display-wide
	static constexpr uint32_t PIXCLK1_RESYNC_CNTL = 0x138;
	static constexpr uint32_t PIXCLK0_RESYNC_CNTL = 0x13A;
	static constexpr uint32_t PHYPLL_PIXCLK_CNTL = 0x13E;
	static constexpr uint32_t CRTC0_PIXEL_RATE_CNTL = 0x140;  // CRTCn at +4n; [1:0] PIXEL_RATE_SOURCE, [4] DP_DTO0_ENABLE
	static constexpr uint32_t PIPE0_PG_STATUS = 0x2C2;  // PIPEn at +3n; [31:30] PGFSM_PWR_STATUS, 0 = powered
	static constexpr uint32_t DMIF_ADDR_CONFIG = 0x2F5;  // DCE 11.0 only (DMIFV_STATUS on 11.2)
	static constexpr uint32_t DMIF_CONTROL = 0x2F6;
	static constexpr uint32_t DMIF_STATUS = 0x2F7;
	static constexpr uint32_t DMIF_ARBITRATION_CONTROL = 0x2F9;
	static constexpr uint32_t DMIF_ADDR_CALC = 0x303;
	static constexpr uint32_t MCIF_CONTROL = 0x30C;
	static constexpr uint32_t DCI_MEM_PWR_STATUS = 0x317;
	static constexpr uint32_t DCI_CLK_CNTL = 0x319;
	// dce_11_0_d.h, pipe 0 (add pipeRegisterBase(pipe))
	static constexpr uint32_t GRPH_ENABLE = 0x1A00;
	static constexpr uint32_t GRPH_CONTROL = 0x1A01;
	static constexpr uint32_t GRPH_PRIMARY_SURFACE_ADDRESS = 0x1A04;
	static constexpr uint32_t GRPH_PRIMARY_SURFACE_ADDRESS_HIGH = 0x1A07;
	static constexpr uint32_t DPG_PIPE_ARBITRATION_CONTROL1 = 0x1B30;
	static constexpr uint32_t DPG_PIPE_ARBITRATION_CONTROL2 = 0x1B31;
	static constexpr uint32_t DPG_WATERMARK_MASK_CONTROL = 0x1B32;
	static constexpr uint32_t DPG_PIPE_URGENCY_CONTROL = 0x1B33;
	static constexpr uint32_t DPG_PIPE_DPM_CONTROL = 0x1B34;
	static constexpr uint32_t DPG_PIPE_STUTTER_CONTROL = 0x1B35;
	static constexpr uint32_t DPG_PIPE_NB_PSTATE_CHANGE_CONTROL = 0x1B36;
	static constexpr uint32_t DPG_PIPE_STUTTER_CONTROL_NONLPTCH = 0x1B37;
	static constexpr uint32_t CRTC_CONTROL = 0x1B9C;
}

// Apple's Polaris (Baffin, RX 460/560) device ID, the closest supported personality for
// AMD9500Controller/AMDRadeonX4000. Spoofing is done via OpenCore DeviceProperties for now.
static constexpr uint16_t PolarisSpoofDeviceId = 0x67EF;

#endif /* kern_asic_hpp */
