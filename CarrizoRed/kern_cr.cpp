//
//  kern_cr.cpp
//  CarrizoRed
//
//  Phase 1: observe only. Finds the iGPU, logs what the hardware reports, and
//  watches Apple's Polaris kexts load. Nothing is patched yet.
//

#include "kern_cr.hpp"
#include "kern_vbios.hpp"

#include <Headers/kern_api.hpp>
#include <Headers/kern_devinfo.hpp>
#include <Headers/kern_iokit.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IODeviceTreeSupport.h>

static const char *pathRadeonX4000[] {"/System/Library/Extensions/AMDRadeonX4000.kext/Contents/MacOS/AMDRadeonX4000"};
static const char *pathRadeonX4000HWServices[] {
	"/System/Library/Extensions/AMDRadeonX4000HWServices.kext/Contents/MacOS/AMDRadeonX4000HWServices"};
static const char *pathPolarisController[] {
	"/System/Library/Extensions/AMD9500Controller.kext/Contents/MacOS/AMD9500Controller"};
static const char *pathSupport[] {"/System/Library/Extensions/AMDSupport.kext/Contents/MacOS/AMDSupport"};

enum KextIndex : size_t {
	KextRadeonX4000,
	KextRadeonX4000HWServices,
	KextPolarisController,
	KextSupport,
	KextCount,
};

static KernelPatcher::KextInfo kexts[KextCount] {
	[KextRadeonX4000] = {"com.apple.kext.AMDRadeonX4000", pathRadeonX4000, arrsize(pathRadeonX4000), {}, {},
		KernelPatcher::KextInfo::Unloaded},
	[KextRadeonX4000HWServices] = {"com.apple.kext.AMDRadeonX4000HWServices", pathRadeonX4000HWServices,
		arrsize(pathRadeonX4000HWServices), {}, {}, KernelPatcher::KextInfo::Unloaded},
	[KextPolarisController] = {"com.apple.kext.AMD9500Controller", pathPolarisController,
		arrsize(pathPolarisController), {}, {}, KernelPatcher::KextInfo::Unloaded},
	[KextSupport] = {"com.apple.kext.AMDSupport", pathSupport, arrsize(pathSupport), {}, {},
		KernelPatcher::KextInfo::Unloaded},
};

CRDriver *CRDriver::callback = nullptr;

void CRDriver::init() {
	callback = this;
	SYSLOG("cr", "CarrizoRed starting on kernel %d.%d", getKernelVersion(), getKernelMinorVersion());

	lilu.onPatcherLoadForce(
		[](void *user, KernelPatcher &patcher) { static_cast<CRDriver *>(user)->processPatcher(patcher); }, this);
	lilu.onKextLoadForce(
		kexts, arrsize(kexts),
		[](void *user, KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size) {
			static_cast<CRDriver *>(user)->processKext(patcher, index, slide, size);
		},
		this);
}

void CRDriver::processPatcher(KernelPatcher &) {
	auto *devInfo = DeviceInfo::create();
	if (!devInfo) {
		SYSLOG("cr", "failed to create DeviceInfo");
		disableKextCallbacks();
		return;
	}

	// Lilu classifies the iGPU by its device-id property, so once we spoof a Polaris ID
	// via DeviceProperties it lands in videoExternal instead. Match on config space.
	IORegistryEntry *candidates[1 + 8] {devInfo->videoBuiltin};
	size_t candidateNum = 1;
	for (size_t i = 0; i < devInfo->videoExternal.size() && candidateNum < arrsize(candidates); i++)
		candidates[candidateNum++] = devInfo->videoExternal[i].video;

	for (size_t i = 0; i < candidateNum && !iGPU; i++) {
		auto *pci = OSDynamicCast(IOPCIDevice, candidates[i]);
		if (!pci)
			continue;
		auto vendor = WIOKit::readPCIConfigValue(pci, WIOKit::kIOPCIConfigVendorID);
		auto device = static_cast<uint16_t>(WIOKit::readPCIConfigValue(pci, WIOKit::kIOPCIConfigDeviceID));
		auto fam = familyForDevice(device);
		DBGLOG("cr", "candidate %s vendor 0x%04X device 0x%04X", safeString(pci->getName()), vendor, device);
		if (vendor != WIOKit::VendorID::ATIAMD || fam == ChipFamily::Unknown)
			continue;

		iGPU = pci;
		iGPU->retain();
		family = fam;
		deviceId = device;
	}

	DeviceInfo::deleter(devInfo);

	// DeviceInfo skips devices whose vendor-id *property* isn't a known GPU vendor, and the EFI overrides it to
	// 0xFFFF to keep AMDSupport off the iGPU until we patch it. Fall back to every IOPCIDevice's config space.
	if (!iGPU)
		findByConfigSpace();

	if (!iGPU) {
		SYSLOG("cr", "no supported Carrizo-family iGPU found, disabling");
		disableKextCallbacks();
		return;
	}

	logPCIInfo();
	if (!checkKernelArgument("-crnommio"))
		probeMMIO();

	// AMDSupport reads the VBIOS from "ATY,bin_image" before trying the (absent) PCI ROM.
	if (!checkKernelArgument("-crnovbios")) {
		bool ok = VBIOS::inject(iGPU, WIOKit::VendorID::ATIAMD, deviceId);
		iGPU->setProperty("CarrizoRed,vbios-injected", ok);
	}
}

void CRDriver::findByConfigSpace() {
	// Walk the device tree like DeviceInfo does (it has already waited for the PCI root to publish): this early in
	// boot the IOPCIDevices exist but may not be registered yet, so IOService matching could miss them.
	auto *iterator = IORegistryIterator::iterateOver(gIODTPlane, kIORegistryIterateRecursively);
	if (!iterator)
		return;
	while (auto *entry = iterator->getNextObject()) {
		auto *pci = OSDynamicCast(IOPCIDevice, entry);
		if (!pci)
			continue;
		auto vendor = WIOKit::readPCIConfigValue(pci, WIOKit::kIOPCIConfigVendorID);
		auto device = static_cast<uint16_t>(WIOKit::readPCIConfigValue(pci, WIOKit::kIOPCIConfigDeviceID));
		auto fam = familyForDevice(device);
		if (vendor != WIOKit::VendorID::ATIAMD || fam == ChipFamily::Unknown)
			continue;
		SYSLOG("cr", "found iGPU %s by config space (not listed by DeviceInfo)", safeString(pci->getName()));
		iGPU = pci;
		iGPU->retain();
		family = fam;
		deviceId = device;
		break;
	}
	iterator->release();
}

void CRDriver::processKext(KernelPatcher &, size_t index, mach_vm_address_t slide, size_t size) {
	for (auto &kext : kexts) {
		if (kext.loadIndex == index) {
			SYSLOG("cr", "%s loaded: slide 0x%llX size 0x%lX", kext.id, static_cast<uint64_t>(slide),
				static_cast<unsigned long>(size));
			return;
		}
	}
}

void CRDriver::logPCIInfo() {
	revision = static_cast<uint8_t>(WIOKit::readPCIConfigValue(iGPU, WIOKit::kIOPCIConfigRevisionID));
	auto subVendor = WIOKit::readPCIConfigValue(iGPU, WIOKit::kIOPCIConfigSubSystemVendorID, 0, 16);
	auto subDevice = WIOKit::readPCIConfigValue(iGPU, WIOKit::kIOPCIConfigSubSystemID, 0, 16);

	uint32_t propDeviceId = 0;
	WIOKit::getOSDataValue(iGPU, "device-id", propDeviceId);

	SYSLOG("cr", "found %s iGPU at %s: 1002:%04X rev 0x%02X subsys %04X:%04X (device-id property 0x%04X)",
		familyName(family), safeString(iGPU->getName()), deviceId, revision, subVendor, subDevice, propDeviceId & 0xFFFF);

	if (auto *bar0 = iGPU->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0))
		SYSLOG("cr", "BAR0 (FB aperture) 0x%llX size 0x%llX", static_cast<uint64_t>(bar0->getPhysicalAddress()),
			static_cast<uint64_t>(bar0->getLength()));
	if (auto *bar5 = iGPU->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5))
		SYSLOG("cr", "BAR5 (MMIO) 0x%llX size 0x%llX", static_cast<uint64_t>(bar5->getPhysicalAddress()),
			static_cast<uint64_t>(bar5->getLength()));

	iGPU->setProperty("CarrizoRed,family", familyName(family));
	iGPU->setProperty("CarrizoRed,revision", revision, 8);
}

void CRDriver::probeMMIO() {
	auto *map = iGPU->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
	if (!map) {
		SYSLOG("cr", "failed to map MMIO BAR, skipping register probe");
		return;
	}

	auto *mmio = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
	auto memSize = mmio[Reg::CONFIG_MEMSIZE];
	auto fbLocation = mmio[Reg::MC_VM_FB_LOCATION];
	auto fbOffset = mmio[Reg::MC_VM_FB_OFFSET];
	auto grbmStatus = mmio[Reg::GRBM_STATUS];
	auto gbAddrConfig = mmio[Reg::GB_ADDR_CONFIG];
	auto shaderArrayConfig = mmio[Reg::CC_GC_SHADER_ARRAY_CONFIG];
	auto rbBackendDisable = mmio[Reg::CC_RB_BACKEND_DISABLE];
	map->release();

	// Same sanity rule as gmc_v8_0_mc_init(): some firmware leaves garbage in the upper half.
	if ((memSize & 0xFFFF0000) && (memSize & 0xFFFF))
		memSize &= 0xFFFF;

	uint64_t fbBase = static_cast<uint64_t>(fbLocation & 0xFFFF) << 24;
	uint64_t fbTop = (static_cast<uint64_t>(fbLocation >> 16) << 24) | 0xFFFFFF;
	uint64_t carveoutPhys = static_cast<uint64_t>(fbOffset) << 22;

	SYSLOG("cr", "UMA carve-out %u MB at system 0x%llX, GPU FB range 0x%llX-0x%llX", memSize, carveoutPhys, fbBase,
		fbTop);
	SYSLOG("cr", "GRBM_STATUS 0x%08X GB_ADDR_CONFIG 0x%08X CC_GC_SHADER_ARRAY_CONFIG 0x%08X CC_RB_BACKEND_DISABLE 0x%08X",
		grbmStatus, gbAddrConfig, shaderArrayConfig, rbBackendDisable);

	iGPU->setProperty("CarrizoRed,vram-mb", memSize, 32);
	iGPU->setProperty("CarrizoRed,carveout-phys", carveoutPhys, 64);
	iGPU->setProperty("CarrizoRed,gb-addr-config", gbAddrConfig, 32);
}

void CRDriver::disableKextCallbacks() {
	for (auto &kext : kexts)
		kext.switchOff();
}
