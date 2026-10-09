/*
 * SSDT-XOSI for the ASUS VivoBook 15 D509BA (A9-9425).
 *
 * The DSDT's OSFL() sets OSVR from _OSI("Windows xxxx") checks. macOS only answers _OSI("Darwin"), so OSVR stays 3,
 * and the HPET's _STA then reports it disabled (it needs OSVR >= 0x0C, "Windows 2006") and clears HPEN. Without an
 * HPET, the PS/2 keyboard and USB were dead in macOS recovery. Under Linux (which claims Windows) all was fine.
 *
 * Together with the OpenCore ACPI patch "_OSI" -> "XOSI" (DSDT only), this makes the firmware's OS checks see
 * Windows 10 under macOS, and leaves every other OS on the real _OSI. Pattern from the Dortania OpenCore guide.
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
}
