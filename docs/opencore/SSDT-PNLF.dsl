/*
 * SSDT-PNLF: the backlight device macOS looks for before it treats a panel as built-in.
 *
 * IOGraphics only lets AppleBacklightDisplay take a display when some service matches the name "backlight"
 * (IOFramebuffer.cpp: gIOFBHaveBacklight, nameMatching("backlight")); on a Mac that's the PNLF device, _HID APP0002 with
 * _CID "backlight". Without it the eDP panel gets AppleDisplay, as if it were an external monitor: no brightness slider
 * or keys, and macOS stays in desktop mode, so closing the lid on AC doesn't sleep the machine (desktop mode is set while
 * no AppleBacklightDisplay exists). The panel's connection must also be built-in with a panel sense type, which Apple's
 * AMD framebuffer reports for the eDP connector (connector type 0x2, LVDS).
 *
 * Brightness then goes through Apple's AMD framebuffer (BaffinBiosDigitalConnection::setBacklight), not through this
 * device. Only under macOS; other OSes don't see it.
 *
 * Compile: iasl SSDT-PNLF.dsl
 */
DefinitionBlock ("", "SSDT", 2, "CRED", "PNLF", 0x00001000)
{
    Scope (\_SB)
    {
        Device (PNLF)
        {
            Name (_HID, EisaId ("APP0002"))  // _HID: Hardware ID
            Name (_CID, "backlight")  // _CID: Compatible ID
            Name (_UID, Zero)  // _UID: Unique ID
            Method (_STA, 0, NotSerialized)  // _STA: Status
            {
                If (_OSI ("Darwin"))
                {
                    Return (0x0B)
                }

                Return (Zero)
            }
        }
    }
}
