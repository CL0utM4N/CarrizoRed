//
//  kern_cr.hpp
//  CarrizoRed
//

#ifndef kern_cr_hpp
#define kern_cr_hpp

#include <Headers/kern_patcher.hpp>
#include <IOKit/pci/IOPCIDevice.h>

#include "kern_asic.hpp"

class CRDriver {
public:
	void init();

private:
	static CRDriver *callback;

	void processPatcher(KernelPatcher &patcher);
	void processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size);

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
