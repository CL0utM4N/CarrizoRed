//
//  kern_platprobe.hpp
//  UnifiedRed (from KaberiRed)
//
//  Platform probe (-crplatprobe): a read-only diagnostic for whole-machine freezes early in boot. See the .cpp.
//

#ifndef kern_platprobe_hpp
#define kern_platprobe_hpp

#include <Headers/kern_patcher.hpp>

namespace PlatformProbe {
	// Starts the probe thread if -crplatprobe is set. Safe to call once, as early as possible.
	void start();
	// Notes whether machine_idle still halts (the EFI builder's --nohlt test patch); the probe prints it.
	void checkIdle(KernelPatcher &patcher);
}

#endif /* kern_platprobe_hpp */
