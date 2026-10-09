/*
 * SSDT-XOSI for the ASUS VivoBook 15 D509BA (A9-9425).
 *
 * The DSDT's OSFL() sets OSVR from _OSI("Windows xxxx") checks. macOS only answers _OSI("Darwin"), so OSVR stays 3,
 * and the HPET's _STA then reports it disabled (it needs OSVR >= 0x0C, "Windows 2006") and clears HPEN. Without an
 * HPET, the PS/2 keyboard and USB were dead in macOS recovery. Under Linux (which claims Windows) all was fine.
 *
 * Together with the OpenCore ACPI patch "_OSI" -> "XOSI" (DSDT only), this makes the firmware's OS checks see
 * Windows 7 ("Windows 2009", OSVR = 0x0E) under macOS, and leaves every other OS on the real _OSI. Pattern from the
 * Dortania OpenCore guide.
 *
 * Why Windows 7 and not 10: with OSVR >= 0x0F ("Windows 2012") the firmware keeps XHCD/EHCD/ST_D/SD_D set, which
 * arms the FCH's D3cold power-gating. XHC0._PS3 then cuts the xHCI's power through SMI 0x67, and _PS0 has to reload
 * the controller's firmware (U3D0 -> LXFW) through SMI 0x66. Under macOS the controller never comes back
 * (AppleUSBXHCIPCI "reset failed" 0xe00002c9 on the first power-on, all USB dead). With OSVR = 0x0E those flags are
 * cleared, _PS0/_PS3 do nothing, and macOS only uses plain PCI D-states. Cost: the AMD I2C/GPIO/UART devices
 * (AMD0010, AMDI0030, AMDI0020) only report present for OSVR >= 0x0F, and macOS has no drivers for them anyway.
 * Compile: iasl SSDT-XOSI.dsl
 */
DefinitionBlock ("", "SSDT", 2, "CRRED", "XOSI", 0x00001000)
{
    Method (XOSI, 1, NotSerialized)
    {
        // Windows _OSI strings, oldest to newest (Microsoft "How to Identify the Windows Version in ACPI by Using _OSI").
        Local0 = Package ()
            {
                "Windows 2001",
                "Windows 2001.1",
                "Windows 2001 SP1",
                "Windows 2001 SP2",
                "Windows 2001 SP3",
                "Windows 2006",
                "Windows 2006 SP1",
                "Windows 2006 SP2",
                "Windows 2009"
            }
        If (_OSI ("Darwin"))
        {
            Return ((Ones != Match (Local0, MEQ, Arg0, MTR, Zero, Zero)))
        }
        Else
        {
            Return (_OSI (Arg0))
        }
    }
}
