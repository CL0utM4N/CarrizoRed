//
//  kern_info.c
//  CarrizoRed
//
//  Equivalent of the <target>_info.c file Xcode generates for kexts: declares
//  kmod_info and points libkmod's _start/_stop at Lilu's plugin entry points.
//

#include <mach/mach_types.h>
#include <mach/kmod.h>

#define CR_STR_(x) #x
#define CR_STR(x)  CR_STR_(x)

// Indirection so MODULE_NAME is expanded before KMOD_EXPLICIT_DECL stringises it.
#define CR_KMOD_DECL(name, version, start, stop) KMOD_EXPLICIT_DECL(name, version, start, stop)

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
extern kern_return_t KMOD_START(kmod_info_t *ki, void *data);
extern kern_return_t KMOD_STOP(kmod_info_t *ki, void *data);

__attribute__((visibility("default"))) CR_KMOD_DECL(MODULE_NAME, CR_STR(MODULE_VERSION), _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = KMOD_START;
__private_extern__ kmod_stop_func_t *_antimain = KMOD_STOP;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
