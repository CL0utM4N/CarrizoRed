#!/usr/bin/env python3
"""
Build an OpenCore EFI for the ASUS VivoBook 15 D509BA (A9-9425, Stoney Ridge) test laptop.

    python scripts/build_efi_d509ba.py <work dir>

<work dir> must contain:
    unpacked/OpenCore-<ver>-RELEASE/        OpenCorePkg release zip, extracted
    unpacked/<Kext>-<ver>-RELEASE/          Lilu, VirtualSMC, ECEnabler, VoodooPS2Controller, RestrictEvents, NVMeFix
    downloads/AMD_Vanilla-patches.plist     github.com/AMD-OSX/AMD_Vanilla patches.plist
    acpi/SSDT-EC-USBX-LAPTOP.aml            compiled from docs/opencore/SSDT-EC-USBX-LAPTOP.dsl
and receives:
    usb/EFI/...                             copy usb/ (EFI + com.apple.recovery.boot) to a FAT32 USB stick

SMBIOS serials are generated with OpenCore's macserial on the first build and kept in <work dir>/smbios.plist,
so rebuilding keeps the same identity. They never go into the repository.

Choices follow the Dortania OpenCore Install Guide for AMD Family 15h/16h, adjusted from the machine's own data
(docs/TEST_MACHINES.md): AMI UEFI 2.7 *without* a Memory Attributes Table, no Above-4G option, 2 CPU cores, real EC
named EC0 under \\_SB.PCI0.SBRG, PS/2 keyboard, I2C trackpad (not supported here), Qualcomm Wi-Fi (no macOS driver).
"""

import plistlib
import shutil
import subprocess
import sys
import uuid
import os
from pathlib import Path

CORE_COUNT = 2                      # A9-9425: 2 cores (not threads)
SMBIOS_MODEL = "MacPro7,1"          # Dortania's AMD recommendation; supported by Sequoia (and Tahoe)
BOOT_ARGS = "-v keepsyms=1 debug=0x100 npci=0x3000"
# npci=0x3000: needed on AMD systems without an "Above 4G decoding" option (this laptop has none).

KEXTS = [  # (bundle path inside its release folder, executable, comment)
    ("Lilu-*/Lilu.kext", "Lilu", "Patch engine"),
    ("VirtualSMC-*/Kexts/VirtualSMC.kext", "VirtualSMC", "SMC emulator"),
    ("VirtualSMC-*/Kexts/SMCBatteryManager.kext", "SMCBatteryManager", "Battery status"),
    ("ECEnabler-*/ECEnabler.kext", "ECEnabler", "Read multi-byte EC fields (battery)"),
    ("VoodooPS2Controller-*/VoodooPS2Controller.kext", "VoodooPS2Controller", "PS/2 controller"),
    ("VoodooPS2Controller-*/VoodooPS2Controller.kext/Contents/PlugIns/VoodooPS2Keyboard.kext", "VoodooPS2Keyboard",
     "PS/2 keyboard"),
    ("RestrictEvents-*/RestrictEvents.kext", "RestrictEvents", "MacPro7,1 memory/PCI warnings, CPU name"),
    ("NVMeFix-*/NVMeFix.kext", "NVMeFix", "NVMe power management"),
]

DRIVERS = ["OpenRuntime.efi", "OpenHfsPlus.efi", "ResetNvramEntry.efi"]


def die(msg):
    sys.exit(f"error: {msg}")


def one(base, pattern):
    hits = sorted(base.glob(pattern))
    if len(hits) != 1:
        die(f"expected exactly one match for {pattern} in {base}, found {len(hits)}")
    return hits[0]


def amd_vanilla_patches(path):
    patches = plistlib.load(open(path, "rb"))["Kernel"]["Patch"]
    cores = 0
    for p in patches:
        if "cpuid_cores_per_package" in p.get("Comment", ""):
            rep = bytearray(p["Replace"])
            rep[1] = CORE_COUNT
            p["Replace"] = bytes(rep)
            cores += 1
    if cores != 4:
        die(f"expected 4 core-count patches in AMD_Vanilla, found {cores}")
    return patches


def smbios(work, oc):
    store = work / "smbios.plist"
    if store.exists():
        return plistlib.load(open(store, "rb"))
    macserial = oc / "Utilities" / "macserial" / ("macserial.exe" if os.name == "nt" else "macserial")
    out = subprocess.run([str(macserial), "-m", SMBIOS_MODEL, "-g", "-n", "1"], capture_output=True, text=True,
                         check=True).stdout.strip()
    serial, mlb = [s.strip() for s in out.split("|")]
    data = {"SystemProductName": SMBIOS_MODEL, "SystemSerialNumber": serial, "MLB": mlb,
            "SystemUUID": str(uuid.uuid4()).upper(), "ROM": os.urandom(6)}
    plistlib.dump(data, open(store, "wb"))
    return data


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    work = Path(sys.argv[1]).resolve()
    unpacked = work / "unpacked"
    oc = one(unpacked, "OpenCore-*-RELEASE")
    ssdt = work / "acpi" / "SSDT-EC-USBX-LAPTOP.aml"
    if not ssdt.exists():
        die(f"{ssdt} missing: compile docs/opencore/SSDT-EC-USBX-LAPTOP.dsl with iasl first")

    usb = work / "usb"
    efi = usb / "EFI"
    if efi.exists():
        shutil.rmtree(efi)
    shutil.copytree(oc / "X64" / "EFI" / "BOOT", efi / "BOOT")
    ocdir = efi / "OC"
    (ocdir / "ACPI").mkdir(parents=True)
    (ocdir / "Drivers").mkdir()
    (ocdir / "Kexts").mkdir()
    (ocdir / "Tools").mkdir()
    shutil.copy2(oc / "X64" / "EFI" / "OC" / "OpenCore.efi", ocdir)
    shutil.copytree(oc / "X64" / "EFI" / "OC" / "Resources", ocdir / "Resources")
    shutil.copy2(ssdt, ocdir / "ACPI")
    for d in DRIVERS:
        shutil.copy2(oc / "X64" / "EFI" / "OC" / "Drivers" / d, ocdir / "Drivers")
    shutil.copy2(oc / "X64" / "EFI" / "OC" / "Tools" / "OpenShell.efi", ocdir / "Tools")

    cfg = plistlib.load(open(oc / "Docs" / "Sample.plist", "rb"))

    # --- ACPI -------------------------------------------------------------------------------------
    cfg["ACPI"]["Add"] = [{"Comment": "Fake EC + USBX (laptop, real EC0 under SBRG)", "Enabled": True,
                           "Path": ssdt.name}]
    cfg["ACPI"]["Delete"] = []
    cfg["ACPI"]["Patch"] = []
    cfg["ACPI"]["Quirks"].update(FadtEnableReset=False, NormalizeHeaders=False, RebaseRegions=False,
                                 ResetHwSig=False, ResetLogoStatus=True, SyncTableIds=False)

    # --- Booter: AMD, firmware without MAT --------------------------------------------------------
    cfg["Booter"]["MmioWhitelist"] = []
    cfg["Booter"]["Patch"] = []
    cfg["Booter"]["Quirks"].update(
        AvoidRuntimeDefrag=True, DevirtualiseMmio=False, EnableSafeModeSlide=True, EnableWriteUnprotector=True,
        ProvideCustomSlide=True, RebuildAppleMemoryMap=False, SetupVirtualMap=True, SyncRuntimePermissions=False,
        ProtectUefiServices=False, ResizeAppleGpuBars=-1, AllowRelocationBlock=False, DiscardHibernateMap=False,
        ForceExitBootServices=False, ProvideMaxSlide=0)

    # --- DeviceProperties ---------------------------------------------------------------------------
    # Keep Apple's AMDSupport off the unsupported Stoney iGPU while installing: AMDSupport matches any display-class
    # 1002:xxxx device (IOPCIMatch 0x00001002&0x0000FFFF) and panics in start() on a chip it doesn't know (NULL deref,
    # first install attempt, 2026-10-09). IOPCIMatch compares the vendor-id *property*, so overriding it stops the
    # match; the class code is left alone so the boot framebuffer keeps working. Remove once CarrizoRed handles the iGPU.
    cfg["DeviceProperties"]["Add"] = {
        "PciRoot(0x0)/Pci(0x1,0x0)": {"vendor-id": bytes.fromhex("FFFF0000")},
    }
    cfg["DeviceProperties"]["Delete"] = {}

    # --- Kernel -----------------------------------------------------------------------------------
    adds = []
    for pattern, exe, comment in KEXTS:
        src = one(unpacked, pattern)
        if "/PlugIns/" not in pattern:
            shutil.copytree(src, ocdir / "Kexts" / src.name)
        rel = pattern.split("/", 1)[1]
        if rel.startswith("Kexts/"):
            rel = rel[len("Kexts/"):]
        adds.append({"Arch": "Any", "BundlePath": rel, "Comment": comment, "Enabled": True,
                     "ExecutablePath": f"Contents/MacOS/{exe}", "MaxKernel": "", "MinKernel": "",
                     "PlistPath": "Contents/Info.plist"})
    cfg["Kernel"]["Add"] = adds
    cfg["Kernel"]["Block"] = []
    cfg["Kernel"]["Force"] = []
    cfg["Kernel"]["Patch"] = amd_vanilla_patches(work / "downloads" / "AMD_Vanilla-patches.plist")
    cfg["Kernel"]["Emulate"].update(DummyPowerManagement=True, Cpuid1Data=b"", Cpuid1Mask=b"")
    cfg["Kernel"]["Quirks"].update(
        AppleCpuPmCfgLock=False, AppleXcpmCfgLock=False, AppleXcpmExtraMsrs=False, DisableIoMapper=False,
        DisableLinkeditJettison=True, LapicKernelPanic=False, PanicNoKextDump=True, PowerTimeoutKernelPanic=True,
        ProvideCurrentCpuInfo=True, XhciPortLimit=False, ThirdPartyDrives=False)
    cfg["Kernel"]["Scheme"].update(FuzzyMatch=True, KernelArch="x86_64", KernelCache="Auto")

    # --- Misc -------------------------------------------------------------------------------------
    cfg["Misc"]["Boot"].update(HideAuxiliary=False, PickerMode="Builtin", ShowPicker=True, Timeout=5,
                               PollAppleHotKeys=True)
    cfg["Misc"]["Debug"].update(AppleDebug=True, ApplePanic=True, DisableWatchDog=True, Target=3)
    cfg["Misc"]["Security"].update(AllowSetDefault=True, BlacklistAppleUpdate=True, ExposeSensitiveData=6,
                                   ScanPolicy=0, SecureBootModel="Disabled", Vault="Optional")
    cfg["Misc"]["Entries"] = []
    cfg["Misc"]["Tools"] = [{"Arguments": "", "Auxiliary": True, "Comment": "UEFI shell", "Enabled": True,
                             "Flavour": "OpenShell:UEFIShell:Shell", "FullNvramAccess": False,
                             "Name": "UEFI Shell", "Path": "OpenShell.efi", "RealPath": False,
                             "TextMode": False}]

    # --- NVRAM ------------------------------------------------------------------------------------
    boot = cfg["NVRAM"]["Add"]["7C436110-AB2A-4BBB-A880-FE41995C9F82"]
    boot["boot-args"] = BOOT_ARGS
    boot["csr-active-config"] = bytes(4)
    boot["prev-lang:kbd"] = "en-US:0"
    cfg["NVRAM"]["WriteFlash"] = True

    # --- PlatformInfo -----------------------------------------------------------------------------
    cfg["PlatformInfo"]["Generic"].update(smbios(work, oc))
    cfg["PlatformInfo"]["UpdateSMBIOSMode"] = "Create"

    # --- UEFI -------------------------------------------------------------------------------------
    cfg["UEFI"]["Drivers"] = [{"Arguments": "", "Comment": "", "Enabled": True, "LoadEarly": False, "Path": d}
                              for d in DRIVERS]
    for drv in cfg["UEFI"]["Drivers"]:
        drv["HideVerbose"] = False
    cfg["UEFI"]["Output"].update(ProvideConsoleGop=True, Resolution="Max")
    cfg["UEFI"]["Quirks"].update(ReleaseUsbOwnership=True, RequestBootVarRouting=True, UnblockFsConnect=False)

    plistlib.dump(cfg, open(ocdir / "config.plist", "wb"), sort_keys=False)
    print(f"EFI written to {efi}")
    print(f"SMBIOS {SMBIOS_MODEL}, serials stored in {work / 'smbios.plist'}")


if __name__ == "__main__":
    main()
