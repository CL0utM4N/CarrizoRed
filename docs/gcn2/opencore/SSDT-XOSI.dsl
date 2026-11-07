/*
 * SSDT-XOSI: answer the DSDT's _OSI queries as Windows 8.1 does, with OpenCore's ACPI patch renaming _OSI to XOSI in
 * the DSDT. Both HP Kabini laptops' DSDTs (Insyde, 2014/2015) set OSTB from _OSI("Windows 20xx"); macOS answers only
 * "Darwin", so OSTB stays 0 and HPET._STA clears the FCH's HPET enable (PM register 0x50, HPAD &= ~0x1F). Under macOS
 * the HPET then reads all ones (platform probe, 2026-11-01). Linux and Windows answer the Windows strings and keep it.
 * Same approach as the Dortania OpenCore guide's SSDT-XOSI.
 */
DefinitionBlock ("", "SSDT", 2, "KABINI", "XOSI", 0x00001000)
{
    Method (XOSI, 1, NotSerialized)
    {
        Local0 = Package ()
            {
                "Windows 2001",
                "Windows 2001.1",
                "Windows 2001 SP1",
                "Windows 2001 SP2",
                "Windows 2001 SP3",
                "Windows 2006",
                "Windows 2006 SP1",
                "Windows 2009",
                "Windows 2012",
                "Windows 2013"
            }
        If (_OSI ("Darwin"))
        {
            Return ((Ones != Match (Local0, MEQ, Arg0, MTR, Zero, Zero)))
        }

        Return (_OSI (Arg0))
    }
}
