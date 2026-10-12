#!/usr/bin/env python3
"""
Build an OpenCore EFI for the ASUS VivoBook 15 D509BA (A9-9425, Stoney Ridge) test laptop.

    python scripts/build_efi_d509ba.py <work dir> [--gux] [--xhciprobe] [--carrizored [--spoof [--accel] [--minimal]]] [--args="..."] [--oclog] [--nousbmap]

<work dir> must contain:
    unpacked/OpenCore-<ver>-RELEASE/        OpenCorePkg release zip, extracted
    unpacked/<Kext>-<ver>-RELEASE/          Lilu, VirtualSMC, ECEnabler, VoodooPS2Controller, RestrictEvents, NVMeFix,
                                            VoodooI2C (release zip: VoodooI2C.kext + VoodooI2CHID.kext)
    downloads/AMD_Vanilla-patches.plist     github.com/AMD-OSX/AMD_Vanilla patches.plist
    downloads/HfsPlus.efi                   github.com/acidanthera/OcBinaryData Drivers/HfsPlus.efi
    acpi/SSDT-*.aml                         compiled from docs/opencore/SSDT-*.dsl (see SSDTS)
and receives:
    usb/EFI/...                             copy usb/ (EFI + com.apple.recovery.boot) to a FAT32 USB stick

SMBIOS serials are generated with OpenCore's macserial on the first build and kept in <work dir>/smbios.plist,
so rebuilding keeps the same identity. They never go into the repository.

Choices follow the Dortania OpenCore Install Guide for AMD Family 15h/16h, adjusted from the machine's own data
(docs/TEST_MACHINES.md): AMI UEFI 2.7 *without* a Memory Attributes Table, no Above-4G option, 2 CPU cores, real EC
named EC0 under \\_SB.PCI0.SBRG, PS/2 keyboard, ELAN I2C trackpad (VoodooI2C), Qualcomm Wi-Fi (no macOS driver).
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
BOOT_ARGS = "-v keepsyms=1 debug=0x100 npci=0x3000 msgbuf=1048576 revpatch=sbvm"
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
    # ELAN HID-over-I2C touchpad (\_SB.I2CD.ETPD, PNP0C50 at 0x15) on the FCH I2C controller AMD0010, interrupt through
    # the AMD0030 GPIO controller. VoodooI2C 2.9+ matches both AMD IDs. They only report present with the firmware in
    # Windows 8+ mode (SSDT-XOSI). Dependencies first: VoodooGPIO, VoodooI2CServices, VoodooInput, VoodooI2C, HID.
    ("VoodooI2C-*/VoodooI2C.kext/Contents/PlugIns/VoodooGPIO.kext", "VoodooGPIO", "GPIO (AMD0030) for I2C interrupts"),
    ("VoodooI2C-*/VoodooI2C.kext/Contents/PlugIns/VoodooI2CServices.kext", "VoodooI2CServices", "VoodooI2C services"),
    ("VoodooI2C-*/VoodooI2C.kext/Contents/PlugIns/VoodooInput.kext", "VoodooInput", "Trackpad input (Magic Trackpad)"),
    ("VoodooI2C-*/VoodooI2C.kext", "VoodooI2C", "I2C controller (AMD0010)"),
    ("VoodooI2C-*/VoodooI2CHID.kext", "VoodooI2CHID", "HID-over-I2C touchpad (ELAN)"),
    # The FCH xHCI (1022:7914) never delivers its interrupts to macOS; without this USB runs off AppleUSBXHCI's 2 s
    # fallback timer (FCHXHCIFix/README.md). From the CI artifact (FCHXHCIFix-*.zip), unpacked to unpacked/FCHXHCIFix/.
    ("FCHXHCIFix/FCHXHCIFix.kext", "FCHXHCIFix", "USB: service the FCH xHCI, whose interrupts are lost"),
]

# Optional (--gux): GenericUSBXHCI "RyzenXHCIFix" (github.com/RattletraPM/GUX-RyzenXHCIFix, unpacked to unpacked/GUX/).
# Experiment for the XHCI "reset failed" problem (2026-10-09): on some AMD APUs, letting GUX run its early init and
# bail out makes AppleUSBXHCIPCI work afterwards.
GUX_KEXT = ("GUX/GenericUSBXHCI.kext", "GenericUSBXHCI", "Experimental: AMD APU XHCI init workaround")
# Optional (--xhciprobe): tools/XHCIProbe diagnostic kext from CI, unpacked to unpacked/XHCIProbe/.
XHCIPROBE_KEXT = ("XHCIProbe/XHCIProbe.kext", "XHCIProbe", "Diagnostic: log/attempt xHCI start before AppleUSBXHCIPCI")
# Optional (--carrizored): the CarrizoRed kext from CI (CarrizoRed-*-Release.zip), unpacked to unpacked/CarrizoRed/.
CARRIZORED_KEXT = ("CarrizoRed/CarrizoRed.kext", "CarrizoRed", "CarrizoRed (Lilu plugin, iGPU)")

# HfsPlus.efi (Apple's HFS+ driver, from github.com/acidanthera/OcBinaryData Drivers/, in downloads/) instead of
# OpenCore's OpenHfsPlus.efi, which is much slower: booting the full installer's HFS+ partition, the firmware took 112 s
# to load BootKernelExtensions.kc (2026-10-10). Drivers not in OpenCore's release come from downloads/.
DRIVERS = ["OpenRuntime.efi", "HfsPlus.efi", "ResetNvramEntry.efi"]

SSDTS = {  # compiled from docs/opencore/<name>.dsl into <work dir>/acpi/
    "SSDT-EC-USBX-LAPTOP.aml": "Fake EC + USBX (laptop, real EC0 under SBRG)",
    "SSDT-XOSI.aml": "XOSI: firmware sees Windows 10 under macOS; XHCF clears XHCD (no xHCI D3cold)",
}


# AppleUSBXHCI::reset() waits only 100 ms for USBCMD.HCRST to clear (clock_interval_to_deadline(100, 1000000)).
# The D509BA's FCH xHCI (1022:7914) needs ~105 ms, so the driver declares it dead and powers it off (XHCIProbe trace,
# docs/TEST_MACHINES.md). Raise the wait to 1000 ms. The search starts at reset() itself and the first
# "mov edi, 100; mov esi, 1000000" in it is that deadline (offset 0x23b in 15.8.1). Linux allows up to 10 s.
XHCI_RESET_PATCH = {
    "Arch": "x86_64", "Base": "__ZN12AppleUSBXHCI5resetEv", "Comment": "AppleUSBXHCI reset() timeout 100 -> 1000 ms",
    "Count": 1, "Enabled": True, "Find": bytes.fromhex("bf64000000be40420f00"),
    "Identifier": "com.apple.driver.usb.AppleUSBXHCI", "Limit": 0x400, "Mask": b"", "MaxKernel": "",
    "MinKernel": "20.0.0", "Replace": bytes.fromhex("bfe8030000be40420f00"), "ReplaceMask": b"", "Skip": 0,
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
    with_cr = "--carrizored" in sys.argv[1:]
    with_spoof = "--spoof" in sys.argv[1:]
    with_oclog = "--oclog" in sys.argv[1:]
    if with_spoof and not with_cr:
        die("--spoof needs --carrizored (it injects the VBIOS the Polaris drivers need)")
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
        src = oc / "X64" / "EFI" / "OC" / "Drivers" / d
        if not src.exists():
            src = work / "downloads" / d
            if not src.exists():
                die(f"{d} missing: put it in {work / 'downloads'}")
        shutil.copy2(src, ocdir / "Drivers")
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
    if with_spoof:
        # Phase 1 test (ROADMAP): let Apple's Polaris 11 (Baffin) drivers attach to the iGPU and record where they fail.
        # 0x67EF is in the IOPCIMatch lists of AMD9500Controller, AMDRadeonX4000 (Baffin) and HWServices; CAIL reads
        # the real device ID from config space, so it still takes its Stoney path (docs/APPLE_KEXTS.md).
        cfg["DeviceProperties"]["Add"]["PciRoot(0x0)/Pci(0x1,0x0)"] = {"device-id": bytes.fromhex("EF670000")}
    cfg["DeviceProperties"]["Delete"] = {}

    # --- Kernel -----------------------------------------------------------------------------------
    adds = []
    extra = ([GUX_KEXT] if with_gux else []) + ([XHCIPROBE_KEXT] if with_probe else []) +         ([CARRIZORED_KEXT] if with_cr else [])
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
    # USB map (codeless kext from the DSDT's _UPC / _PLD, docs/opencore/USBMap.md). --nousbmap leaves it out.
    if "--nousbmap" not in sys.argv[1:]:
        usbmap = Path(__file__).resolve().parent.parent / "docs" / "opencore" / "USBMap.kext"
        shutil.copytree(usbmap, ocdir / "Kexts" / usbmap.name)
        adds.append({"Arch": "Any", "BundlePath": usbmap.name, "Comment": "USB map (XHC0, EHC1)", "Enabled": True,
                     "ExecutablePath": "", "MaxKernel": "", "MinKernel": "", "PlistPath": "Contents/Info.plist"})
    cfg["Kernel"]["Add"] = adds
    cfg["Kernel"]["Block"] = []
    cfg["Kernel"]["Force"] = []
    cfg["Kernel"]["Patch"] = amd_vanilla_patches(work / "downloads" / "AMD_Vanilla-patches.plist") + [XHCI_RESET_PATCH]
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
    if with_oclog:
        # Target 0x41: logging on + write opencore-<date>.txt to the root of the EFI partition OpenCore booted from.
        # DisplayLevel 0x80000042: errors, warnings and info (kext injection results are info).
        cfg["Misc"]["Debug"].update(Target=0x41, DisplayLevel=0x80000042)
    cfg["Misc"]["Security"].update(AllowSetDefault=True, BlacklistAppleUpdate=True, ExposeSensitiveData=6,
                                   ScanPolicy=0, SecureBootModel="Disabled", Vault="Optional")
    cfg["Misc"]["Entries"] = []
    cfg["Misc"]["Tools"] = [{"Arguments": "", "Auxiliary": True, "Comment": "UEFI shell", "Enabled": True,
                             "Flavour": "OpenShell:UEFIShell:Shell", "FullNvramAccess": False,
                             "Name": "UEFI Shell", "Path": "OpenShell.efi", "RealPath": False,
                             "TextMode": False}]

    # --- NVRAM ------------------------------------------------------------------------------------
    boot = cfg["NVRAM"]["Add"]["7C436110-AB2A-4BBB-A880-FE41995C9F82"]
    # Spoof tests bring up the display controller first: -crnoaccel keeps AMDRadeonX4000/HWServices off (--accel to allow).
    boot["boot-args"] = BOOT_ARGS + (" -crnoaccel" if with_spoof and "--accel" not in sys.argv[1:] else "")
    # --minimal: also keep AGPM and AGDP off and use legacy interrupts (spoof boots 6-8 froze as the GUI started).
    if with_spoof and "--minimal" in sys.argv[1:]:
        boot["boot-args"] += " -crnoagpm -crnoagdp -crnomsi"
    # --args="...": extra boot-args for one-off test variants.
    for arg in sys.argv[1:]:
        if arg.startswith("--args="):
            boot["boot-args"] += " " + arg[len("--args="):]
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
