/*
 * SSDT-XOSI for the ASUS VivoBook 15 D509BA (A9-9425).
 *
 * The DSDT's OSFL() sets OSVR from _OSI("Windows xxxx") checks. macOS only answers _OSI("Darwin"), so OSVR stays 3,
 * and the HPET's _STA then reports it disabled (it needs OSVR >= 0x0C, "Windows 2006") and clears HPEN. Without an
 * HPET, the PS/2 keyboard and USB were dead in macOS recovery. Under Linux (which claims Windows) all was fine.
 *
 * Together with the OpenCore ACPI patch "_OSI" -> "XOSI" (DSDT only), this makes the firmware's OS checks see
 * Windows 10 under macOS (OSFL: OSVR = 0x10, "Windows 2013"; ATK MSOS: OSWA, "Windows 2015"), and leaves every other
 * OS on the real _OSI. Pattern from the Dortania OpenCore guide. What Windows 8+ turns on:
 *   - the AMD I2C controllers (AMD0010, the ELAN touchpad's bus), GPIO (AMDI0030) and UARTs (AMD0020) report present;
 *   - the EC sends the brightness Fn keys as ACPI notifications (Notify (VGA.LCD, 0x86 / 0x87)).
 *
 * The catch, and XHCF below: with OSVR >= 0x0F the firmware's FINI (called from \_SB.PCI0._INI) keeps XHCD set, which
 * arms the FCH xHCI's D3cold power-gating. XHC0._PS3 then cuts the controller's power through SMI (FDDC 0x17), and
 * _PS0 has to reload its firmware (U3D0 -> LXFW). Under macOS the controller never comes back (AppleUSBXHCIPCI "reset
 * failed" 0xe00002c9 on its first power-on, all USB dead; install attempt 4). Under Windows 7 (OSVR = 0x0E) FINI
 * clears XHCD, and _S0W / _PS0 / _PS3 / _PR3 do nothing. XHCF clears just XHCD again after FINI, so the xHCI behaves as
 * under Windows 7 while everything else sees Windows 10. Its _INI runs after \_SB.PCI0._INI because ACPI runs a
 * parent's _INI before its children's. EHCD, ST_D and SD_D (EHCI, SATA, SD D3cold) stay as Windows 10 leaves them:
 * EHCI worked in attempt 4.
 * Compile: iasl SSDT-XOSI.dsl
 */
DefinitionBlock ("", "SSDT", 2, "CRRED", "XOSI", 0x00002000)
{
    External (\XHCD, FieldUnitObj)
    External (\_SB.PCI0, DeviceObj)

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
                "Windows 2009",
                "Windows 2012",
                "Windows 2013",
                "Windows 2015"
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

    Scope (\_SB.PCI0)
    {
        Device (XHCF)
        {
            Name (_HID, "XHCF0000")  // _HID: Hardware ID
            Method (_INI, 0, NotSerialized)  // _INI: Initialize
            {
                If (_OSI ("Darwin"))
                {
                    \XHCD = Zero
                }
            }

            Method (_STA, 0, NotSerialized)  // _STA: Status
            {
                If (_OSI ("Darwin"))
                {
                    Return (0x0B)  // present, enabled, not shown in UI (_INI only runs when present)
                }

                Return (Zero)
            }
        }
    }
}
