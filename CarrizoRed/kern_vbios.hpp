//
//  kern_vbios.hpp
//  CarrizoRed
//
//  APUs have no PCI expansion ROM: the VBIOS is handed over by the system firmware in the ACPI VFCT table.
//  AMDSupport's AtomBiosProxy::readAtomBios() reads the "ATY,bin_image" property of the GPU first and only falls
//  back to the PCI ROM, so we copy the VFCT image into that property before AMDSupport starts.
//
//  VFCT layout from Linux drivers/gpu/drm/amd/include/atombios.h, matching and validation rules from
//  amdgpu_bios.c (amdgpu_acpi_vfct_bios / check_atom_bios). Both MIT licensed:
//    Copyright 2006-2007 Advanced Micro Devices, Inc.; Copyright 2008 Red Hat Inc.; Copyright 2009 Jerome Glisse.
//

#ifndef kern_vbios_hpp
#define kern_vbios_hpp

#include <IOKit/pci/IOPCIDevice.h>
#include <stdint.h>

namespace VBIOS {
	// AtomBiosProxy::readEfiAtomBiosImage copies at most this many bytes of "ATY,bin_image".
	static constexpr uint32_t MaxAppleImageSize = 0x10000;

	static constexpr const char *ImageProperty = "ATY,bin_image";

#pragma pack(push, 1)
	struct AcpiHeader {
		char signature[4];
		uint32_t length;
		uint8_t revision;
		uint8_t checksum;
		char oemId[6];
		uint64_t oemTableId;
		uint32_t oemRevision;
		uint32_t creatorId;
		uint32_t creatorRevision;
	};

	// UEFI_ACPI_VFCT
	struct Vfct {
		AcpiHeader header;
		uint8_t tableUUID[16];
		uint32_t vbiosImageOffset;  // Offset of the first image (VFCT_IMAGE_HEADER + data) from the table start
		uint32_t lib1ImageOffset;
		uint32_t reserved[4];
	};

	// VFCT_IMAGE_HEADER, followed directly by imageLength bytes of VBIOS.
	struct VfctImageHeader {
		uint32_t pciBus;
		uint32_t pciDevice;
		uint32_t pciFunction;
		uint16_t vendorId;
		uint16_t deviceId;
		uint16_t subsystemVendorId;
		uint16_t subsystemId;
		uint32_t revision;
		uint32_t imageLength;
	};
#pragma pack(pop)

	static_assert(sizeof(AcpiHeader) == 0x24, "ACPI header size");
	static_assert(sizeof(Vfct) == 0x4C, "UEFI_ACPI_VFCT size");
	static_assert(sizeof(VfctImageHeader) == 0x1C, "VFCT_IMAGE_HEADER size");

	/**
	 *  Find the VBIOS for `pci` in the ACPI VFCT table and publish it as "ATY,bin_image".
	 *  Does nothing if the property already exists (e.g. injected through OpenCore DeviceProperties).
	 *
	 *  @return true if the property is present afterwards
	 */
	bool inject(IOPCIDevice *pci, uint16_t vendorId, uint16_t deviceId);
}

#endif /* kern_vbios_hpp */
