//
//  kern_cr.hpp
//  CarrizoRed
//

#ifndef kern_cr_hpp
#define kern_cr_hpp

#include <Headers/kern_patcher.hpp>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/pci/IOPCIDevice.h>

#include "kern_asic.hpp"

class CRDriver {
public:
	// Map the GPU's registers if start-up couldn't (see probeMMIO); cheap once mapped.
	static void ensureMmio();
	void init();

private:
	static CRDriver *callback;

	void processPatcher(KernelPatcher &patcher);
	void processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size);
	void patchPolarisController(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size);

	static uint32_t wrapGetDisplayLimit(void *that);
	static uint8_t wrapGetNumLineBuffers(void *that);
	static uint32_t wrapGetDigLimit(void *that);

	void patchPlls(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size);
	static void *wrapCreatePllAssigner(void *controller, void *connectors, uint8_t numConnectors, uint8_t numPlls);
	static uint64_t wrapSetPixelClock(void *that, void *clockParams, const void *connectorInfo);
	static void wrapSetCrtcToPixelClock(void *that, uint8_t crtc, uint8_t pll, void *clockParams);
	mach_vm_address_t orgCreatePllAssigner {0}, orgSetPixelClock {0}, orgSetCrtcToPixelClock {0};

	void blockProbe(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size, const char *probe,
		const char *arg);
	static bool wrapStartFail(IOService *that, IOService *provider);
	void patchAgdpBoardId(KernelPatcher &patcher, size_t kext);
	void setMacGpuProperties();
	void setGpuModel();
	void setSoftPowerPlayTable();

	void patchInterrupts(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size);
	static void wrapIhSetHardwareEnabled(void *that, bool enable);
	static void wrapIhHandler(void *that, void *owner, void *arg, void *nub, int source);
	static void wrapIhSetReadPointer(void *that, uint32_t pointer);
	mach_vm_address_t orgIhSetHardwareEnabled {0}, orgIhHandler {0}, orgIhSetReadPointer {0};
	bool stormBreak {true};
	static uint32_t wrapRegRead32(void *that, uint32_t offset);
	static uint32_t wrapHwRead32(void *that, uint32_t offset);
	static uint32_t wrapRead16(void *that, uint32_t offset);
	static uint32_t wrapRead8(void *that, uint32_t offset);
	static void wrapWrite16(void *that, uint32_t offset, uint32_t value);
	static void wrapWrite8(void *that, uint32_t offset, uint32_t value);
	mach_vm_address_t orgHwRead32 {0}, orgRead16 {0}, orgRead8 {0}, orgWrite16 {0}, orgWrite8 {0};
	static void wrapRegWrite32(void *that, uint32_t offset, uint32_t value);
	static uint32_t wrapPcieRead32(void *that, uint32_t offset);
	static void wrapPcieWrite32(void *that, uint32_t offset, uint32_t value);
	static uint32_t wrapIndRead(void *that, uint32_t index, uint32_t data, uint32_t offset);
	static void wrapIndWrite(void *that, uint32_t index, uint32_t data, uint32_t offset, uint32_t value);
	mach_vm_address_t orgRegRead32 {0}, orgRegWrite32 {0}, orgPcieRead32 {0}, orgPcieWrite32 {0}, orgIndRead {0},
		orgIndWrite {0};
	static IOService *wrapProbeNoMatch(IOService *that, IOService *provider, SInt32 *score);

	void patchSupport(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size);
	static uint64_t wrapGetConfigProperty(void *that, const char *name, void *value, uint32_t size);
	mach_vm_address_t orgGetConfigProperty {0};
	static bool wrapTestVram(void *that, uint32_t region, bool flag);
	mach_vm_address_t orgTestVram {0};

	void traceController(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size);
	void traceSupport(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size);
	void traceFramebuffer(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size);
	static IODeviceMemory *wrapGetApertureRange(void *that, int aperture);
	mach_vm_address_t orgGetApertureRange {0};
	static IOService *wrapAtiProbe(IOService *that, IOService *provider, SInt32 *score);
	static bool wrapAtiStart(IOService *that, IOService *provider);
	mach_vm_address_t orgAtiProbe {0}, orgAtiStart {0};

	// Power / policy / config-space tracing (see patchPci in kern_cr.cpp).
	void patchPci(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size);
	void patchPolicy(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size);
	static bool isGpuFunction(IOPCIDevice *device);
	static IOReturn wrapPciSetPowerState(IOPCIDevice *that, unsigned long state, IOService *whatDevice);
	static IOReturn wrapPciSetPCIPowerState(IOPCIDevice *that, uint8_t state, uint32_t options);
	static IOReturn wrapBridgeSetDevicePowerState(IOService *that, IOPCIDevice *device, uint32_t options,
		unsigned long prevState, unsigned long newState);
	static void wrapConfigWrite8(IOPCIDevice *that, uint8_t offset, uint8_t value);
	static void wrapConfigWrite16(IOPCIDevice *that, uint8_t offset, uint16_t value);
	static void wrapConfigWrite32(IOPCIDevice *that, uint8_t offset, uint32_t value);
	static void wrapExtConfigWrite8(IOPCIDevice *that, uint64_t offset, uint8_t value);
	static void wrapExtConfigWrite16(IOPCIDevice *that, uint64_t offset, uint16_t value);
	static void wrapExtConfigWrite32(IOPCIDevice *that, uint64_t offset, uint32_t value);
	mach_vm_address_t orgPciSetPowerState {0}, orgPciSetPCIPowerState {0}, orgBridgeSetDevicePowerState {0},
		orgConfigWrite8 {0}, orgConfigWrite16 {0}, orgConfigWrite32 {0}, orgExtConfigWrite8 {0},
		orgExtConfigWrite16 {0}, orgExtConfigWrite32 {0};
	static IOReturn wrapDeviceControlAttribute(void *that, uint32_t attribute, uint64_t *value, uint64_t size,
		uint64_t *arg4, uint64_t *arg5, void *arguments);
	static IOReturn wrapWranglerAttribute(void *that, uint32_t attribute, uint64_t *value, uint64_t size,
		uint64_t *arg4, uint64_t *arg5, void *arguments);
	mach_vm_address_t orgDeviceControlAttribute {0}, orgWranglerAttribute {0};
	static IOReturn wrapInstallInterrupts(void *that);
	static uint8_t wrapExecuteBiosCommandTable(void *that, uint32_t table, void *params, uint32_t flag);
	mach_vm_address_t orgExecuteBiosCommandTable {0};
	static uint8_t wrapMaxLinkLanes(const void *that);
	static uint8_t wrapMaxLinkRate(const void *that);
	mach_vm_address_t orgMaxLinkLanes {0}, orgMaxLinkRate {0};
	static IOReturn wrapAuxReadBytes(void *that, uint32_t address, uint32_t length, uint8_t *data, uint8_t flags);
	static IOReturn wrapAuxWriteBytes(void *that, uint32_t address, uint32_t length, uint8_t *data, uint8_t flags);
	mach_vm_address_t orgAuxReadBytes {0}, orgAuxWriteBytes {0};
	mach_vm_address_t orgInstallInterrupts {0};
	static void wrapWritePCIReg8(void *that, uint32_t offset, uint8_t value);
	static void wrapWritePCIReg16(void *that, uint32_t offset, uint16_t value);
	static void wrapWritePCIReg32(void *that, uint32_t offset, uint32_t value);
	mach_vm_address_t orgWritePCIReg8 {0}, orgWritePCIReg16 {0}, orgWritePCIReg32 {0};
	uint8_t gpuBus {0xFF}, gpuDevice {0xFF};

	// Acceleration (AMDRadeonX4000): Stoney has one sDMA engine and one shader engine, and its "VRAM" is a carve-out
	// in system RAM, so local page-table entries need the carve-out's physical address (see patchAccelerator).
	void traceKext(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size, size_t kext);
	void patchAccelerator(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size);
	static bool wrapAllocateHWEngines(void *that);
	static bool wrapStartHWEngines(void *that);
	static void *wrapGetHWChannel(void *that, uint32_t engine, uint32_t ring);
	static uint64_t wrapSetupHWCapabilities(void *that);
	static uint64_t wrapUpdatePTEsDMA(void *that, uint64_t dst, uint64_t count, uint64_t addr, uint64_t flags,
		uint64_t incr);
	static uint64_t wrapGetPTEValue(void *that, uint32_t level, uint64_t addr, uint32_t flags, uint32_t fragment);
	static uint64_t wrapGetPDEValue(void *that, uint32_t level, uint64_t addr);
	mach_vm_address_t orgAllocateHWEngines {0}, orgStartHWEngines {0}, orgGetHWChannel {0}, orgSetupHWCapabilities {0}, orgUpdatePTEsDMA {0},
		orgGetPTEValue {0}, orgGetPDEValue {0};

	void startWatcher();
	static void watcherThread(void *arg, wait_result_t);

	void findByConfigSpace();
	void logPCIInfo();
	void probeMMIO();
	void relocateFb();
	void initDisplayRegs();
	void checkDisplayRegs(uint32_t t);
	void panicReport(uint32_t t, const char *tree, const char *reason);
	const char *composeReport(uint32_t t, const char *tree);
	void disableKextCallbacks();

	IOPCIDevice *iGPU {nullptr};
	ChipFamily family {ChipFamily::Unknown};
	uint16_t deviceId {0};
	uint8_t revision {0};
};

#endif /* kern_cr_hpp */
