/*
 * SSDT-EC-USBX for the ASUS VivoBook 15 D509BA (A9-9425).
 *
 * macOS (Catalina and later) expects an ACPI device named "EC" for USB power management. This laptop's real
 * embedded controller is EC0 (PNP0C09) under \_SB.PCI0.SBRG, so we add a do-nothing "EC" next to it rather than
 * renaming the real one, which keeps EC0 (battery, lid, hotkeys) untouched. USBX supplies the USB power properties
 * macOS normally gets from Apple firmware. Both devices only exist under macOS (_OSI("Darwin")).
 *
 * Pattern from the Dortania OpenCore Install Guide (SSDT-EC-USBX-LAPTOP), path from this laptop's DSDT.
 * Compile: iasl SSDT-EC-USBX-LAPTOP.dsl
 */
DefinitionBlock ("", "SSDT", 2, "CRRED", "SsdtEC", 0x00001000)
{
    External (_SB_.PCI0.SBRG, DeviceObj)

    Scope (\_SB.PCI0.SBRG)
    {
        Device (EC)
        {
            Name (_HID, "ACID0001")  // _HID: Hardware ID
            Method (_STA, 0, NotSerialized)  // _STA: Status
            {
                If (_OSI ("Darwin"))
                {
                    Return (0x0F)
                }
                Else
                {
                    Return (Zero)
                }
            }
        }
    }

    Scope (\_SB)
    {
        Device (USBX)
        {
            Name (_ADR, Zero)  // _ADR: Address
            Method (_DSM, 4, NotSerialized)  // _DSM: Device-Specific Method
            {
                If ((Arg2 == Zero))
                {
                    Return (Buffer (One)
                    {
                         0x03
                    })
                }

                Return (Package (0x08)
                {
                    "kUSBSleepPowerSupply",
                    0x13EC,
                    "kUSBSleepPortCurrentLimit",
                    0x0834,
                    "kUSBWakePowerSupply",
                    0x13EC,
                    "kUSBWakePortCurrentLimit",
                    0x0834
                })
            }

            Method (_STA, 0, NotSerialized)  // _STA: Status
            {
                If (_OSI ("Darwin"))
                {
                    Return (0x0F)
                }
                Else
                {
                    Return (Zero)
                }
            }
        }
    }
}
