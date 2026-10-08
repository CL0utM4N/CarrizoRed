//
//  kern_start.cpp
//  CarrizoRed
//

#include <Headers/kern_api.hpp>
#include <Headers/plugin_start.hpp>

#include "kern_cr.hpp"

static CRDriver cr;

static const char *bootargOff[] {"-croff"};
static const char *bootargDebug[] {"-crdbg"};
static const char *bootargBeta[] {"-crbeta"};

PluginConfiguration ADDPR(config) {
	xStringify(PRODUCT_NAME),
	parseModuleVersion(xStringify(MODULE_VERSION)),
	LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
	bootargOff,
	arrsize(bootargOff),
	bootargDebug,
	arrsize(bootargDebug),
	bootargBeta,
	arrsize(bootargBeta),
	KernelVersion::Monterey,
	KernelVersion::Tahoe,
	[]() { cr.init(); },
};
