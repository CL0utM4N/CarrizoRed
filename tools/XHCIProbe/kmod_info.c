//
//  kmod_info.c
//  XHCIProbe
//
//  kmod_info for a plain IOKit kext: no start/stop routines (IOKit loads the class from the personality).
//

#include <mach/mach_types.h>
#include <mach/kmod.h>

#define XP_STR_(x) #x
#define XP_STR(x)  XP_STR_(x)
#define XP_KMOD_DECL(name, version, start, stop) KMOD_EXPLICIT_DECL(name, version, start, stop)

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

__attribute__((visibility("default"))) XP_KMOD_DECL(MODULE_NAME, XP_STR(MODULE_VERSION), _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
