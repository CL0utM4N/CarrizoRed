# D509BA USB map (`USBMap.kext`)

Codeless kext (`docs/opencore/USBMap.kext`) built from the DSDT, following Dortania's manual mapping guide. Ports
come from `_UPC` (connectable, type) and `_PLD` (group position: ports with the same group are one physical socket):

| Controller | Port | ACPI | Type | Socket |
|---|---|---|---|---|
| XHC0 `1022:7914` | 1 / 5 | PRT1 / PRT5 | 3 (USB 3 Type-A) | blue USB-A, USB 2 / USB 3 side (group 5) |
| | 2 / 6 | PRT2 / PRT6 | 9 (Type-C with switch) | USB-C, USB 2 / USB 3 side (group 6) |
| | 7 | PRT7 | 0 (USB 2 Type-A) | black USB 2.0 port (group 0x0B) |
| | 8 | PRT8 | 0 (USB 2 Type-A) | black USB 2.0 port (group 0x0C) |
| | 3, 4 | PRT3, PRT4 | not connectable | (left out) |
| EHC1 `1022:7908` | 1 | GHUB | 255 (internal) | AMD internal hub: webcam, Bluetooth |

So the two USB 2.0 Type-A ports are on the xHCI controller, not on EHCI. 8 ports: under the 15-port limit,
`XhciPortLimit` stays off. Personalities use `AppleUSBHostMergeProperties` (Big Sur+), matched by controller name and
`model` = iMac19,1. To verify on hardware: `ioreg -p IOUSB -w0` with a device in each socket.
