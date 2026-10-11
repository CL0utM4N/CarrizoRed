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
	void init();

private:
	static CRDriver *callback;

	void processPatcher(KernelPatcher &patcher);
	void processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size);
	void patchPolarisController(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size);

	static uint32_t wrapGetDisplayLimit(void *that);
	static uint8_t wrapGetNumLineBuffers(void *that);

	void blockProbe(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size, const char *probe,
		const char *arg);
	static bool wrapStartFail(IOService *that, IOService *provider);

	void patchInterrupts(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size);
	static void wrapIhSetHardwareEnabled(void *that, bool enable);
	static void wrapIhHandler(void *that, void *owner, void *arg, void *nub, int source);
	static void wrapIhSetReadPointer(void *that, uint32_t pointer);
	mach_vm_address_t orgIhSetHardwareEnabled {0}, orgIhHandler {0}, orgIhSetReadPointer {0};
	bool stormBreak {true};
	static uint32_t wrapRegRead32(void *that, uint32_t offset);
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

	void startWatcher();
	static void watcherThread(void *arg, wait_result_t);

	void findByConfigSpace();
	void logPCIInfo();
	void probeMMIO();
	void disableKextCallbacks();

	IOPCIDevice *iGPU {nullptr};
	ChipFamily family {ChipFamily::Unknown};
	uint16_t deviceId {0};
	uint8_t revision {0};
};

#endif /* kern_cr_hpp */
