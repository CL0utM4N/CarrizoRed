#!/usr/bin/env python3
"""
Build an OpenCore EFI for the Kabini HP test laptops, macOS Monterey 12.

    python scripts/build_efi_hp.py <work dir> [--machine=<name>] [--unifiedred [--nopanicat]] [--spoof [--accel]]
                                             [--args="..."] [--oclog]
                                             [--debugoc] [--diag] [--nowifi] [--xhciprobe] [--fchxhcifix] [--nousb2]
                                             [--minimal] [--noecgpe] [--novarwrite] [--nohlt]

    --machine   hp-8133 (default): HP Notebook board 8133, A4-5000, Elantech trackpad, Intel 7260 Wi-Fi
                hp15-f008cl: HP 15-f008cl (board 2330), A6-5200, Synaptics touchpad, Realtek RTL8188EE Wi-Fi (no macOS
                driver). Both: Insyde UEFI 2.3.1 without a MAT, EC0 under \\_SB.PCI0.LPC0, FCH xHCI 7814, 4 Jaguar cores.
    --nopanicat with --unifiedred: no crpanicat=60 (the deliberate report panic), e.g. to install
    --debugoc   OpenCore's DEBUG binaries (unpacked/OpenCore-<ver>-DEBUG/) for a verbose log
    --diag      print OpenCore's log on screen through the firmware's own text console (TextRenderer
                SystemText) and to a file on the stick: for a black screen before the picker

<work dir> must contain:
    unpacked/OpenCore-<ver>-RELEASE/        OpenCorePkg release zip, extracted
    unpacked/<Kext>-<ver>-RELEASE/          Lilu, VirtualSMC, ECEnabler, VoodooPS2Controller, RestrictEvents
    unpacked/AirportItlwm-*-Monterey/       OpenIntelWireless/itlwm AirportItlwm_*_stable_Monterey.kext.zip, extracted
    unpacked/UnifiedRed/                     (--unifiedred) UnifiedRed.kext from CI (UnifiedRed-*-Debug.zip)
    downloads/AMD_Vanilla-patches.plist     github.com/AMD-OSX/AMD_Vanilla patches.plist
    downloads/HfsPlusLegacy.efi             github.com/acidanthera/OcBinaryData Drivers/HfsPlusLegacy.efi (no RDRAND)
    acpi/SSDT-EC-USBX-LAPTOP.aml            compiled from docs/opencore/SSDT-EC-USBX-LAPTOP.dsl (iasl)
    acpi/SSDT-XOSI.aml                      compiled from docs/opencore/SSDT-XOSI.dsl (iasl)
    recovery/com.apple.recovery.boot/       Monterey Recovery from OpenCore's macrecovery.py
                                            (-b Mac-FFE5EF870D7BA81A -m 00000000000000000 download)
and receives:
    usb/                                    copy its contents (EFI + com.apple.recovery.boot) to a FAT32 USB stick

SMBIOS serials are generated with OpenCore's macserial on the first build and kept in <work dir>/smbios.plist, so
rebuilding keeps the same identity. They never go into the repository.

Choices follow the Dortania OpenCore Install Guide for AMD family 15h/16h and CarrizoRed's laptop EFI, adjusted from
this machine's own data (docs/TEST_MACHINE.md): Insyde UEFI 2.3.1 without a Memory Attributes Table, no Above-4G
option, 4 CPU cores, real EC named EC0 under \\_SB.PCI0.LPC0, PS/2 keyboard and Elantech trackpad (ETD0706), Intel
Wireless 7260 (AirportItlwm), Realtek RTL8101E Ethernet (no maintained binary driver: Recovery uses Wi-Fi). Both
DSDTs decide the OS from _OSI("Windows 20xx"); macOS answers only "Darwin", and HPET._STA then switches the HPET off
(the probe read all ones, 2026-11-01), so _OSI is renamed to XOSI and SSDT-XOSI answers as Windows 8.1 does.
"""

import os
import plistlib
import shutil
import subprocess
import sys
import uuid
from pathlib import Path

CORE_COUNT = 4                      # A4-5000, A6-5200: 4 Jaguar cores
MACHINES = {
    "hp-8133": {"trackpad": "Elantech PS/2 trackpad (ETD0706)", "wifi": True},
    # HP 15-f008cl: Synaptics PS/2 touchpad (Linux: SynPS/2, fw 7.5), RTL8188EE Wi-Fi with no macOS driver, RTL8106E
    # Ethernet, LVDS panel. Same EC path, PCI layout of the FCH and HPET/_OSI behaviour as the hp-8133 (DSDT, 2026-11-01).
    "hp15-f008cl": {"trackpad": "Synaptics PS/2 touchpad", "wifi": False},
}
SMBIOS_MODEL = "iMac19,1"           # Monterey-supported, AMD (Polaris) graphics. Not MacPro6,1/7,1 or iMacPro1,1:
# AppleIntelMCEReporter matches those board-ids and panics on AMD CPUs (CarrizoRed, 2026-10-09).
BOOT_ARGS = "-v keepsyms=1 debug=0x100 npci=0x3000 msgbuf=1048576"
# npci=0x3000: AMD systems without an "Above 4G decoding" option. msgbuf: 1 MB kernel log, so boot messages survive.

KEXTS = [  # (bundle path inside its release folder, executable, comment)
    ("Lilu-*/Lilu.kext", "Lilu", "Patch engine"),
    ("VirtualSMC-*/Kexts/VirtualSMC.kext", "VirtualSMC", "SMC emulator"),
    ("VirtualSMC-*/Kexts/SMCBatteryManager.kext", "SMCBatteryManager", "Battery status"),
    ("ECEnabler-*/ECEnabler.kext", "ECEnabler", "Read multi-byte EC fields (battery)"),
    ("VoodooPS2Controller-*/VoodooPS2Controller.kext", "VoodooPS2Controller", "PS/2 controller"),
    ("VoodooPS2Controller-*/VoodooPS2Controller.kext/Contents/PlugIns/VoodooInput.kext", "VoodooInput",
     "Trackpad input (Magic Trackpad emulation)"),
    ("VoodooPS2Controller-*/VoodooPS2Controller.kext/Contents/PlugIns/VoodooPS2Keyboard.kext", "VoodooPS2Keyboard",
     "PS/2 keyboard"),
    ("VoodooPS2Controller-*/VoodooPS2Controller.kext/Contents/PlugIns/VoodooPS2Trackpad.kext", "VoodooPS2Trackpad",
     "PS/2 trackpad"),
    ("RestrictEvents-*/RestrictEvents.kext", "RestrictEvents", "CPU name, misc. fixes"),
    ("AirportItlwm-*-Monterey/AirportItlwm.kext", "AirportItlwm", "Intel Wireless 7260 (Wi-Fi, also in Recovery)"),
]
# --unifiedred: the UnifiedRed kext from CI (UnifiedRed-*-Debug.zip), unpacked to unpacked/UnifiedRed/.
UNIFIEDRED_KEXT = ("UnifiedRed/UnifiedRed.kext", "UnifiedRed", "UnifiedRed (Lilu plugin, iGPU)")
# USB: macOS never saw the stick on any port (Recovery waits for boot-uuid-media; 2026-10-17). The external ports are
# on the FCH xHCI 1022:7814 (Linux dump), the generation before CarrizoRed's 7914, whose interrupts never reached macOS.
# --xhciprobe: CarrizoRed's XHCIProbe (unpacked/XHCIProbe/), observe-only here (-xhciprobe_noact): prints the
#              controller's interrupt and port state on the verbose screen every 2 s.
# --fchxhcifix: github.com/CL0utM4N/FCHXHCIFix (unpacked/FCHXHCIFix/), its IOPCIPrimaryMatch extended to 1022:7814.
XHCIPROBE_KEXT = ("XHCIProbe/XHCIProbe.kext", "XHCIProbe", "Diagnostic: xHCI state on the verbose screen")
FCHXHCIFIX_KEXT = ("FCHXHCIFix/FCHXHCIFix.kext", "FCHXHCIFix", "USB: service the FCH xHCI if its interrupts are lost")

# Apple's HFS+ driver instead of OpenCore's much slower OpenHfsPlus.efi; it isn't in OpenCore's release. The *Legacy*
# build: HfsPlus.efi executes RDRAND, which Jaguar doesn't have, and the firmware hung loading it (2026-10-17: the
# black screen before the picker; the --diag log stopped at "Driver HfsPlus.efi at 1 is being loaded").
DRIVERS = ["OpenRuntime.efi", "HfsPlusLegacy.efi", "ResetNvramEntry.efi"]

SSDTS = {  # compiled from docs/opencore/<name>.dsl into <work dir>/acpi/
    "SSDT-EC-USBX-LAPTOP.aml": "Fake EC + USBX (laptop, real EC0 under LPC0)",
    "SSDT-XOSI.aml": "_OSI answers as Windows 8.1 (with the DSDT's _OSI to XOSI rename)",
}

# Bonaire XT: in the IOPCIMatch lists of AMD8000Controller, AMDRadeonX4000 (AMDBonaireGraphicsAccelerator) and
# AMDRadeonX4000HWServices (CI). CAIL still reads the real device ID (0x9832) from config space (docs/APPLE_KEXTS.md).
SPOOF_DEVICE_ID = bytes.fromhex("5C660000")


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
    opts = sys.argv[1:]
    args = [a for a in opts if not a.startswith("--")]
    with_kr = "--unifiedred" in opts
    machine = next((a.split("=", 1)[1] for a in opts if a.startswith("--machine=")), "hp-8133")
    if machine not in MACHINES:
        die(f"unknown machine {machine}: one of {', '.join(MACHINES)}")
    profile = MACHINES[machine]
    with_spoof = "--spoof" in opts
    if with_spoof and not with_kr:
        die("--spoof needs --unifiedred")
    if len(args) != 1:
        sys.exit(__doc__)
    work = Path(args[0]).resolve()
    unpacked = work / "unpacked"
    oc = one(unpacked, "OpenCore-*-RELEASE")
    # Binaries from the DEBUG build with --debugoc; Sample.plist and macserial always from the release.
    ocbin = one(unpacked, "OpenCore-*-DEBUG") if "--debugoc" in opts else oc
    ssdts = [work / "acpi" / n for n in SSDTS]
    for ssdt in ssdts:
        if not ssdt.exists():
            die(f"{ssdt} missing: compile docs/opencore/{ssdt.stem}.dsl with iasl first")
    recovery = work / "recovery" / "com.apple.recovery.boot"
    if not (recovery / "BaseSystem.dmg").exists():
        die(f"{recovery}/BaseSystem.dmg missing: run macrecovery.py (see the docstring)")

    usb = work / "usb"
    if usb.exists():
        shutil.rmtree(usb)
    efi = usb / "EFI"
    shutil.copytree(ocbin / "X64" / "EFI" / "BOOT", efi / "BOOT")
    ocdir = efi / "OC"
    for sub in ("ACPI", "Drivers", "Kexts", "Tools"):
        (ocdir / sub).mkdir(parents=True)
    shutil.copy2(ocbin / "X64" / "EFI" / "OC" / "OpenCore.efi", ocdir)
    shutil.copytree(oc / "X64" / "EFI" / "OC" / "Resources", ocdir / "Resources")
    for ssdt in ssdts:
        shutil.copy2(ssdt, ocdir / "ACPI")
    for d in DRIVERS:
        src = ocbin / "X64" / "EFI" / "OC" / "Drivers" / d
        if not src.exists():
            src = work / "downloads" / d
            if not src.exists():
                die(f"{d} missing: put it in {work / 'downloads'}")
        shutil.copy2(src, ocdir / "Drivers")
    shutil.copy2(ocbin / "X64" / "EFI" / "OC" / "Tools" / "OpenShell.efi", ocdir / "Tools")
    shutil.copytree(recovery, usb / "com.apple.recovery.boot")

    cfg = plistlib.load(open(oc / "Docs" / "Sample.plist", "rb"))

    # --- ACPI -------------------------------------------------------------------------------------
    cfg["ACPI"]["Add"] = [{"Comment": SSDTS[n], "Enabled": True, "Path": n} for n in SSDTS]
    cfg["ACPI"]["Delete"] = []
    # With SSDT-XOSI. DSDT only: SSDT8 (AMD's) asks _OSI("Windows 2012") and _OSI("Linux") for itself.
    cfg["ACPI"]["Patch"] = [{
        "Base": "", "BaseSkip": 0, "Comment": "_OSI to XOSI", "Count": 0, "Enabled": True,
        "Find": b"_OSI", "Limit": 0, "Mask": b"", "OemTableId": bytes(8),
        "Replace": b"XOSI", "ReplaceMask": b"", "Skip": 0, "TableLength": 0, "TableSignature": b"DSDT"}]
    if "--noecgpe" in opts:
        # Test: keep macOS from listening to the EC's event line (GPE 3), in case the EC floods it (the battery is dead,
        # and _Q09 is the battery-status event). Renames only EC0's Name (_GPE, 0x03): its bytes occur once in the
        # DSDT, apart from the root \_GPE scope. The EC still answers reads; its events (lid, AC, hotkeys) are lost.
        cfg["ACPI"]["Patch"].append({
            "Base": "", "BaseSkip": 0, "Comment": "EC0 _GPE to XGPE (no EC events)", "Count": 1, "Enabled": True,
            "Find": bytes.fromhex("085F4750450A03"), "Limit": 0, "Mask": b"", "OemTableId": bytes(8),
            "Replace": bytes.fromhex("08584750450A03"), "ReplaceMask": b"", "Skip": 0, "TableLength": 0,
            "TableSignature": b"DSDT"})
    cfg["ACPI"]["Quirks"].update(FadtEnableReset=False, NormalizeHeaders=False, RebaseRegions=False,
                                 ResetHwSig=False, ResetLogoStatus=True, SyncTableIds=False)

    # --- Booter: AMD, UEFI 2.3.1 firmware without a Memory Attributes Table ----------------------
    cfg["Booter"]["MmioWhitelist"] = []
    cfg["Booter"]["Patch"] = []
    cfg["Booter"]["Quirks"].update(
        AvoidRuntimeDefrag=True, DevirtualiseMmio=False, EnableSafeModeSlide=True, EnableWriteUnprotector=True,
        ProvideCustomSlide=True, RebuildAppleMemoryMap=False, SetupVirtualMap=True, SyncRuntimePermissions=False,
        ProtectUefiServices=False, ResizeAppleGpuBars=-1, AllowRelocationBlock=False, DiscardHibernateMap=False,
        ForceExitBootServices=False, ProvideMaxSlide=0)
    if "--novarwrite" in opts:
        # Test (2026-10-17): the platform probe showed healthy clocks and no pending interrupts up to ~250 ms before
        # each freeze, then an abrupt total stop, a few seconds into every boot whatever was loaded: the CPU inside
        # firmware code that never returns. macOS writes NVRAM through UEFI runtime services (interrupts off); on this
        # Insyde firmware the variable service is SMM code. OpenRuntime then refuses macOS's writes instead.
        cfg["Booter"]["Quirks"]["DisableVariableWrite"] = True

    # --- DeviceProperties ---------------------------------------------------------------------------
    # Keep Apple's AMDSupport (which matches any display-class 1002:xxxx device) off the unsupported iGPU while
    # installing: IOPCIMatch compares the vendor-id *property*. The class code stays, so the boot framebuffer keeps
    # working. UnifiedRed finds the iGPU by config space, so it doesn't mind.
    igpu = {"vendor-id": bytes.fromhex("FFFF0000")}
    if with_spoof:
        # Phase 1 spoof test: let Apple's GCN 2 drivers attach and record where they stop.
        igpu = {"device-id": SPOOF_DEVICE_ID}
    cfg["DeviceProperties"]["Add"] = {"PciRoot(0x0)/Pci(0x1,0x0)": igpu}
    if "--nousb2" in opts:
        # Test (2026-10-17): with one core the kernel stopped ~2 s after USB came up, XHCIProbe's 1 ms sampler getting
        # only ~150 of ~2000 samples first, as if an interrupt storm took the CPU. Keep Apple's EHCI/OHCI drivers off
        # the FCH's USB 2.0/1.1 controllers (they share legacy IRQs; only the webcam and Bluetooth are on them; the
        # external ports are on the xHCI): IOPCIClassMatch compares the class-code property.
        for dev in ("Pci(0x12,0x0)", "Pci(0x12,0x2)", "Pci(0x13,0x0)", "Pci(0x13,0x2)"):
            cfg["DeviceProperties"]["Add"][f"PciRoot(0x0)/{dev}"] = {"class-code": bytes.fromhex("FFFFFFFF")}
    cfg["DeviceProperties"]["Delete"] = {}

    # --- Kernel -----------------------------------------------------------------------------------
    adds = []
    # --nowifi: leave AirportItlwm out (suspected of the freeze in Recovery, 2026-10-17).
    kexts = [k for k in KEXTS if not (("--nowifi" in opts or not profile["wifi"]) and k[1] == "AirportItlwm")]
    kexts = [(p, e, profile["trackpad"] if e == "VoodooPS2Trackpad" else c) for p, e, c in kexts]
    # --minimal: baseline with only what macOS needs on AMD (Lilu + VirtualSMC, + ECEnabler), to tell the add-on kexts (battery/EC,
    # PS/2, RestrictEvents, Wi-Fi) apart from macOS itself in the total freeze after rooting (2026-10-17).
    if "--minimal" in opts:
        # ECEnabler stays: without it the EC's _REG fails (AE_BAD_PARAMETER in EC0.ITLB/ECRI/_REG: the EC has fields
        # wider than a byte, which AppleACPIEC can't access), so the firmware's EC handshake never completes.
        kexts = [k for k in KEXTS if k[1] in ("Lilu", "VirtualSMC", "ECEnabler")]
    kexts += [XHCIPROBE_KEXT] if "--xhciprobe" in opts else []
    kexts += [FCHXHCIFIX_KEXT] if "--fchxhcifix" in opts else []
    for pattern, exe, comment in kexts + ([UNIFIEDRED_KEXT] if with_kr else []):
        src = one(unpacked, pattern)
        if "/PlugIns/" not in pattern:
            shutil.copytree(src, ocdir / "Kexts" / src.name)
        rel = pattern.split("/", 1)[1]
        if rel.startswith("Kexts/"):
            rel = rel[len("Kexts/"):]
        adds.append({"Arch": "Any", "BundlePath": rel, "Comment": comment, "Enabled": True,
                     "ExecutablePath": f"Contents/MacOS/{exe}", "MaxKernel": "", "MinKernel": "",
                     "PlistPath": "Contents/Info.plist"})
    if "--fchxhcifix" in opts:
        info = ocdir / "Kexts" / "FCHXHCIFix.kext" / "Contents" / "Info.plist"
        plist = plistlib.load(open(info, "rb"))
        for personality in plist["IOKitPersonalities"].values():
            if "0x78141022" not in personality["IOPCIPrimaryMatch"]:
                personality["IOPCIPrimaryMatch"] += " 0x78141022"
        plistlib.dump(plist, open(info, "wb"))
    cfg["Kernel"]["Add"] = adds
    cfg["Kernel"]["Block"] = []
    cfg["Kernel"]["Force"] = []
    cfg["Kernel"]["Patch"] = amd_vanilla_patches(work / "downloads" / "AMD_Vanilla-patches.plist")
    if "--nohlt" in opts:
        # Test (2026-10-17): the platform probe's own 250 ms sleep stopped waking ~2 s in while busy threads went on
        # printing, then everything stopped: timer and keyboard interrupts no longer reach the CPU once it idles. With
        # no CPU PM kext (always so on AMD), machine_idle() idles with plain `sti; hlt` (pmCPU.c); on AMD a HLT on every
        # core can send the CPU into C1E or an SMM handler (MSR C001_0055), where Linux uses the FCH's C-state port
        # (_CST: I/O 0x414). Make it `sti; nop`: the idle thread spins with interrupts on. Hotter: short tests only.
        # Monterey 21.6.0: `fb f4 fa` once in _machine_idle (+0x1C8).
        cfg["Kernel"]["Patch"].append({
            "Arch": "x86_64", "Base": "_machine_idle", "Comment": "machine_idle: sti; hlt -> sti; nop (test)",
            "Count": 1, "Enabled": True, "Find": bytes.fromhex("FBF4FA"), "Identifier": "kernel", "Limit": 0x400,
            "Mask": b"", "MaxKernel": "21.99.99", "MinKernel": "21.0.0", "Replace": bytes.fromhex("FB90FA"),
            "ReplaceMask": b"", "Skip": 0})
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
    if "--oclog" in opts:
        # Target 0x41: write opencore-<date>.txt to the root of the EFI partition; errors, warnings and info.
        cfg["Misc"]["Debug"].update(Target=0x41, DisplayLevel=0x80000042)
    if "--diag" in opts:
        # Target 0x43: log on, on screen, to a file (a hang leaves its last lines on screen).
        cfg["Misc"]["Debug"].update(Target=0x43, DisplayLevel=0x80000042, DisplayDelay=0)
    # SecureBootModel Default: AirportItlwm needs IO80211Family loaded, which Apple Secure Boot provides on Big Sur+
    # (with it Disabled, IO80211Family would have to be force-loaded).
    cfg["Misc"]["Security"].update(AllowSetDefault=True, BlacklistAppleUpdate=True, ExposeSensitiveData=6,
                                   ScanPolicy=0, SecureBootModel="Default", Vault="Optional")
    cfg["Misc"]["Entries"] = []
    cfg["Misc"]["Tools"] = [{"Arguments": "", "Auxiliary": True, "Comment": "UEFI shell (dmpstore)", "Enabled": True,
                             "Flavour": "OpenShell:UEFIShell:Shell", "FullNvramAccess": False,
                             "Name": "UEFI Shell", "Path": "OpenShell.efi", "RealPath": False,
                             "TextMode": False}]

    # --- NVRAM ------------------------------------------------------------------------------------
    boot = cfg["NVRAM"]["Add"]["7C436110-AB2A-4BBB-A880-FE41995C9F82"]
    boot["boot-args"] = BOOT_ARGS
    if with_kr:
        # UnifiedRed's first runs: the report at 60 s. Spoof tests keep the accelerator off unless --accel.
        boot["boot-args"] += " -crdbg" + ("" if "--nopanicat" in opts else " crpanicat=60")
        if with_spoof and "--accel" not in opts:
            boot["boot-args"] += " -crnoaccel"
    if "--xhciprobe" in opts:
        boot["boot-args"] += " -xhciprobe_noact"
    for arg in opts:
        if arg.startswith("--args="):
            boot["boot-args"] += " " + arg[len("--args="):]
    boot["csr-active-config"] = bytes(4)
    boot["prev-lang:kbd"] = "en-US:0"
    cfg["NVRAM"]["WriteFlash"] = True

    # --- PlatformInfo -----------------------------------------------------------------------------
    cfg["PlatformInfo"]["Generic"].update(smbios(work, oc))
    cfg["PlatformInfo"]["UpdateSMBIOSMode"] = "Create"

    # --- UEFI -------------------------------------------------------------------------------------
    cfg["UEFI"]["Drivers"] = [{"Arguments": "", "Comment": "", "Enabled": True, "HideVerbose": False,
                               "LoadEarly": False, "Path": d} for d in DRIVERS]
    # Keep the firmware's own screen mode: with Resolution=Max the picker never appeared on this Insyde UEFI 2.3.1
    # (black screen, 2026-10-17); switching modes on old GOP drivers is a common cause of that.
    cfg["UEFI"]["Output"].update(ProvideConsoleGop=True, Resolution="", ConsoleMode="")
    if "--diag" in opts:
        # The firmware's own text console instead of OpenCore's graphics renderer (whose console switch is a
        # classic cause of a black screen before the picker on old firmware).
        cfg["UEFI"]["Output"].update(TextRenderer="SystemText")
    cfg["UEFI"]["Quirks"].update(ReleaseUsbOwnership=True, RequestBootVarRouting=True, UnblockFsConnect=False)

    plistlib.dump(cfg, open(ocdir / "config.plist", "wb"), sort_keys=False)
    print(f"EFI for {machine} written to {usb}")
    print(f"boot-args: {boot['boot-args']}")
    print(f"SMBIOS {SMBIOS_MODEL}, serials stored in {work / 'smbios.plist'}")


if __name__ == "__main__":
    main()
