//
//  kern_vbios.cpp
//  CarrizoRed
//
//  See kern_vbios.hpp. Matching and validation follow Linux amdgpu_bios.c (MIT).
//

#include "kern_vbios.hpp"

#include <Headers/kern_util.hpp>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <IOKit/IORegistryEntry.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <string.h>

// Why the last inject() failed, published as "CarrizoRed,vbios-status" so it shows in ioreg without the kernel log.
static void setStatus(IOPCIDevice *pci, const char *status) {
	pci->setProperty("CarrizoRed,vbios-status", status);
	SYSLOG("vbios", "%s", status);
}

// The firmware's copy of an ACPI table. The platform expert has no "ACPI Tables" property this early in boot and its
// getACPITableData() is protected, but any ACPI device node can return a table: use the iGPU's own node ("acpi-device",
// set by IOPCIFamily), else \_SB.
static const OSData *copyAcpiTable(IOPCIDevice *pci, const char *signature) {
	IOACPIPlatformDevice *acpi = OSDynamicCast(IOACPIPlatformDevice, pci->getProperty("acpi-device"));
	IORegistryEntry *sb = nullptr;
	if (!acpi) {
		sb = IORegistryEntry::fromPath("IOACPIPlane:/_SB");
		acpi = OSDynamicCast(IOACPIPlatformDevice, sb);
	}
	if (!acpi) {
		setStatus(pci, "no ACPI device to read tables through");
		OSSafeReleaseNULL(sb);
		return nullptr;
	}
	auto *table = acpi->getACPITableData(signature, 0);
	if (table)
		table->retain();
	else
		setStatus(pci, "getACPITableData(VFCT) returned nothing");
	OSSafeReleaseNULL(sb);
	return table;
}

// check_atom_bios(): 55 AA signature, header pointer at 0x48, "ATOM" (or "MOTA") 4 bytes into the header.
static bool isAtomBios(const uint8_t *image, uint32_t size) {
	if (size < 0x4A || image[0] != 0x55 || image[1] != 0xAA)
		return false;
	uint32_t header = static_cast<uint32_t>(image[0x48]) | (static_cast<uint32_t>(image[0x49]) << 8);
	if (!header || header + 8 > size)
		return false;
	auto *magic = image + header + 4;
	return !memcmp(magic, "ATOM", 4) || !memcmp(magic, "MOTA", 4);
}

bool VBIOS::inject(IOPCIDevice *pci, uint16_t vendorId, uint16_t deviceId) {
	if (auto *existing = OSDynamicCast(OSData, pci->getProperty(ImageProperty))) {
		SYSLOG("vbios", "%s already present (%u bytes), leaving it alone", ImageProperty, existing->getLength());
		return true;
	}

	auto *table = copyAcpiTable(pci, "VFCT");
	if (!table)
		return false;

	auto *base = static_cast<const uint8_t *>(const_cast<OSData *>(table)->getBytesNoCopy());
	uint32_t tableSize = table->getLength();
	bool injected = false;

	if (tableSize < sizeof(Vfct) || reinterpret_cast<const AcpiHeader *>(base)->length > tableSize) {
		setStatus(pci, "VFCT table too short");
	} else {
		tableSize = reinterpret_cast<const AcpiHeader *>(base)->length;
		uint32_t offset = reinterpret_cast<const Vfct *>(base)->vbiosImageOffset;
		uint8_t bus = pci->getBusNumber(), dev = pci->getDeviceNumber(), func = pci->getFunctionNumber();

		while (offset + sizeof(VfctImageHeader) <= tableSize) {
			auto *image = reinterpret_cast<const VfctImageHeader *>(base + offset);
			uint32_t dataOffset = offset + sizeof(VfctImageHeader);
			if (image->imageLength == 0 || image->imageLength > tableSize - dataOffset) {
				setStatus(pci, "VFCT image truncated");
				break;
			}
			offset = dataOffset + image->imageLength;

			DBGLOG("vbios", "VFCT image %02X:%02X.%X %04X:%04X rev 0x%X, %u bytes", image->pciBus, image->pciDevice,
				image->pciFunction, image->vendorId, image->deviceId, image->revision, image->imageLength);

			// Same rule as amdgpu_acpi_vfct_match(): IDs, slot and function must match; the bus may have been
			// renumbered since POST, so a bus mismatch is only logged.
			if (image->vendorId != vendorId || image->deviceId != deviceId || image->pciDevice != dev ||
				image->pciFunction != func)
				continue;
			if (image->pciBus != bus)
				SYSLOG("vbios", "VFCT bus %u differs from runtime bus %u, matching by device identity",
					image->pciBus, bus);

			auto *data = base + dataOffset;
			if (!isAtomBios(data, image->imageLength)) {
				setStatus(pci, "VFCT image is not an ATOM BIOS");
				break;
			}
			if (image->imageLength > MaxAppleImageSize)
				SYSLOG("vbios", "VBIOS is %u bytes, AMDSupport only reads the first %u", image->imageLength,
					MaxAppleImageSize);

			pci->setProperty(ImageProperty, const_cast<uint8_t *>(data), image->imageLength);
			SYSLOG("vbios", "injected %s from VFCT (%u bytes)", ImageProperty, image->imageLength);
			pci->setProperty("CarrizoRed,vbios-status", "injected from VFCT");
			injected = true;
			break;
		}
		if (!injected && !pci->getProperty("CarrizoRed,vbios-status"))
			setStatus(pci, "no VFCT image matches this device/slot");
	}

	table->release();
	return injected;
}
