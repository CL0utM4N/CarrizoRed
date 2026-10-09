#!/usr/bin/env python3
"""
Build an OpenCore EFI for the ASUS VivoBook 15 D509BA (A9-9425, Stoney Ridge) test laptop.

    python scripts/build_efi_d509ba.py <work dir> [--gux] [--xhciprobe]

<work dir> must contain:
    unpacked/OpenCore-<ver>-RELEASE/        OpenCorePkg release zip, extracted
    unpacked/<Kext>-<ver>-RELEASE/          Lilu, VirtualSMC, ECEnabler, VoodooPS2Controller, RestrictEvents, NVMeFix
    downloads/AMD_Vanilla-patches.plist     github.com/AMD-OSX/AMD_Vanilla patches.plist
    acpi/SSDT-*.aml                         compiled from docs/opencore/SSDT-*.dsl (see SSDTS)
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
SMBIOS_MODEL = "iMac19,1"           # 2019 27" iMac: Sequoia-supported, shipped with Polaris (Radeon Pro 570X-580X).
# Not MacPro7,1/iMacPro1,1/MacPro6,1: AppleIntelMCEReporter matches those board-ids and panics on AMD CPUs
# (NULL deref in AppleIntelMCE::Controller, seen twice on this laptop, 2026-10-09). It lives in the system KC,
# so OpenCore Kernel->Block can't reliably remove it; avoiding the board-id is the robust fix.
BOOT_ARGS = "-v keepsyms=1 debug=0x100 npci=0x3000 msgbuf=1048576"
# npci=0x3000: needed on AMD systems without an "Above 4G decoding" option (this laptop has none).
# msgbuf=1048576: 1 MB kernel log buffer, so boot-time driver messages survive until `dmesg` is run.

KEXTS = [  # (bundle path inside its release folder, executable, comment)
    ("Lilu-*/Lilu.kext", "Lilu", "Patch engine"),
    ("VirtualSMC-*/Kexts/VirtualSMC.kext", "VirtualSMC", "SMC emulator"),
    ("VirtualSMC-*/Kexts/SMCBatteryManager.kext", "SMCBatteryManager", "Battery status"),
    ("ECEnabler-*/ECEnabler.kext", "ECEnabler", "Read multi-byte EC fields (battery)"),
    ("VoodooPS2Controller-*/VoodooPS2Controller.kext", "VoodooPS2Controller", "PS/2 controller"),
    ("VoodooPS2Controller-*/VoodooPS2Controller.kext/Contents/PlugIns/VoodooPS2Keyboard.kext", "VoodooPS2Keyboard",
     "PS/2 keyboard"),
    ("RestrictEvents-*/RestrictEvents.kext", "RestrictEvents", "CPU name, misc. fixes"),
    ("NVMeFix-*/NVMeFix.kext", "NVMeFix", "NVMe power management"),
]

# Optional (--gux): GenericUSBXHCI "RyzenXHCIFix" (github.com/RattletraPM/GUX-RyzenXHCIFix, unpacked to unpacked/GUX/).
# Experiment for the XHCI "reset failed" problem (2026-10-09): on some AMD APUs, letting GUX run its early init and
# bail out makes AppleUSBXHCIPCI work afterwards.
GUX_KEXT = ("GUX/GenericUSBXHCI.kext", "GenericUSBXHCI", "Experimental: AMD APU XHCI init workaround")
# Optional (--xhciprobe): tools/XHCIProbe diagnostic kext from CI, unpacked to unpacked/XHCIProbe/.
XHCIPROBE_KEXT = ("XHCIProbe/XHCIProbe.kext", "XHCIProbe", "Diagnostic: log/attempt xHCI start before AppleUSBXHCIPCI")

DRIVERS = ["OpenRuntime.efi", "OpenHfsPlus.efi", "ResetNvramEntry.efi"]

SSDTS = {  # compiled from docs/opencore/<name>.dsl into <work dir>/acpi/
    "SSDT-EC-USBX-LAPTOP.aml": "Fake EC + USBX (laptop, real EC0 under SBRG)",
    "SSDT-XOSI.aml": "XOSI: firmware sees Windows 10 under macOS (enables HPET)",
}


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
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    with_gux = "--gux" in sys.argv[1:]
    with_probe = "--xhciprobe" in sys.argv[1:]
    if len(args) != 1:
        sys.exit(__doc__)
    work = Path(args[0]).resolve()
    unpacked = work / "unpacked"
    oc = one(unpacked, "OpenCore-*-RELEASE")
    ssdts = [work / "acpi" / n for n in SSDTS]
    for ssdt in ssdts:
        if not ssdt.exists():
            die(f"{ssdt} missing: compile docs/opencore/{ssdt.stem}.dsl with iasl first")

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
    for ssdt in ssdts:
        shutil.copy2(ssdt, ocdir / "ACPI")
    for d in DRIVERS:
        shutil.copy2(oc / "X64" / "EFI" / "OC" / "Drivers" / d, ocdir / "Drivers")
    shutil.copy2(oc / "X64" / "EFI" / "OC" / "Tools" / "OpenShell.efi", ocdir / "Tools")

    cfg = plistlib.load(open(oc / "Docs" / "Sample.plist", "rb"))

    # --- ACPI -------------------------------------------------------------------------------------
    cfg["ACPI"]["Add"] = [{"Comment": SSDTS[n], "Enabled": True, "Path": n} for n in SSDTS]
    cfg["ACPI"]["Delete"] = []
    # _OSI -> XOSI in the DSDT only (SSDT-XOSI provides XOSI). See docs/opencore/SSDT-XOSI.dsl for why.
    cfg["ACPI"]["Patch"] = [{
        "Base": "", "BaseSkip": 0, "Comment": "_OSI to XOSI (DSDT)", "Count": 0, "Enabled": True,
        "Find": b"_OSI", "Limit": 0, "Mask": b"", "OemTableId": bytes(8), "Replace": b"XOSI", "ReplaceMask": b"",
        "Skip": 0, "TableLength": 0, "TableSignature": b"DSDT"}]
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
    # Precaution while installing: keep Apple's AMDSupport off the unsupported Stoney iGPU. AMDSupport matches any
    # display-class 1002:xxxx device (IOPCIMatch 0x00001002&0x0000FFFF). (The first install panics turned out to be
    # AppleIntelMCEReporter, not AMDSupport; this stays until CarrizoRed handles the iGPU.) IOPCIMatch compares the vendor-id *property*, so overriding it stops the
    # match; the class code is left alone so the boot framebuffer keeps working. Remove once CarrizoRed handles the iGPU.
    cfg["DeviceProperties"]["Add"] = {
        "PciRoot(0x0)/Pci(0x1,0x0)": {"vendor-id": bytes.fromhex("FFFF0000")},
    }
    cfg["DeviceProperties"]["Delete"] = {}

    # --- Kernel -----------------------------------------------------------------------------------
    adds = []
    extra = ([GUX_KEXT] if with_gux else []) + ([XHCIPROBE_KEXT] if with_probe else [])
    for pattern, exe, comment in KEXTS + extra:
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
