//
//  kern_vbios.cpp
//  CarrizoRed
//
//  See kern_vbios.hpp. Matching and validation follow Linux amdgpu_bios.c (MIT).
//

#include "kern_vbios.hpp"

#include <Headers/kern_util.hpp>
#include <IOKit/IORegistryEntry.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <string.h>

// The ACPI tables the firmware provided, as published by AppleACPIPlatformExpert ("ACPI Tables" → "VFCT").
static OSData *copyAcpiTable(const char *signature) {
	auto *expert = IORegistryEntry::fromPath("IOService:/AppleACPIPlatformExpert");
	if (!expert) {
		SYSLOG("vbios", "AppleACPIPlatformExpert not found");
		return nullptr;
	}

	OSData *table = nullptr;
	if (auto *tables = OSDynamicCast(OSDictionary, expert->getProperty("ACPI Tables"))) {
		table = OSDynamicCast(OSData, tables->getObject(signature));
		if (table)
			table->retain();
	} else {
		SYSLOG("vbios", "AppleACPIPlatformExpert has no \"ACPI Tables\"");
	}
	expert->release();
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

	auto *table = copyAcpiTable("VFCT");
	if (!table) {
		SYSLOG("vbios", "no ACPI VFCT table, AMDSupport will not find a VBIOS");
		return false;
	}

	auto *base = static_cast<const uint8_t *>(table->getBytesNoCopy());
	uint32_t tableSize = table->getLength();
	bool injected = false;

	if (tableSize < sizeof(Vfct) || reinterpret_cast<const AcpiHeader *>(base)->length > tableSize) {
		SYSLOG("vbios", "VFCT table too short (%u bytes)", tableSize);
	} else {
		tableSize = reinterpret_cast<const AcpiHeader *>(base)->length;
		uint32_t offset = reinterpret_cast<const Vfct *>(base)->vbiosImageOffset;
		uint8_t bus = pci->getBusNumber(), dev = pci->getDeviceNumber(), func = pci->getFunctionNumber();

		while (offset + sizeof(VfctImageHeader) <= tableSize) {
			auto *image = reinterpret_cast<const VfctImageHeader *>(base + offset);
			uint32_t dataOffset = offset + sizeof(VfctImageHeader);
			if (image->imageLength == 0 || image->imageLength > tableSize - dataOffset) {
				SYSLOG("vbios", "VFCT image at 0x%X truncated (length %u)", offset, image->imageLength);
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
				SYSLOG("vbios", "VFCT image for %04X:%04X is not an ATOM BIOS", vendorId, deviceId);
				break;
			}
			if (image->imageLength > MaxAppleImageSize)
				SYSLOG("vbios", "VBIOS is %u bytes, AMDSupport only reads the first %u", image->imageLength,
					MaxAppleImageSize);

			pci->setProperty(ImageProperty, const_cast<uint8_t *>(data), image->imageLength);
			SYSLOG("vbios", "injected %s from VFCT (%u bytes)", ImageProperty, image->imageLength);
			injected = true;
			break;
		}
		if (!injected)
			SYSLOG("vbios", "no usable VFCT image for %04X:%04X at %02X:%02X.%X", vendorId, deviceId, bus, dev, func);
	}

	table->release();
	return injected;
}
