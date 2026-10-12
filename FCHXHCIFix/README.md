# FCHXHCIFix

Full write-up (cause, diagnosis, affected machines): [docs/FCH_XHCI.md](../docs/FCH_XHCI.md).

A small IOKit kext (no Lilu) that makes USB usable on AMD FCH xHCI controllers whose interrupts never reach macOS.

**Symptom:** USB works but crawls. On the ASUS VivoBook 15 D509BA (A9-9425, Stoney Ridge, FCH xHCI `1022:7914`) the
macOS installer copied from a USB stick at about 130 KB/s, `validate_root_image` took tens of minutes, and mice and
keyboards lagged or dropped out.

**Cause:** none of the controller's interrupts arrive, with plain MSI or with MSI-X, although the controller sends
them. AppleUSBXHCI then only services it from a 2 s fallback timer. Measured with `tools/XHCIProbe`: `USBSTS.EINT` set
in every 1 ms sample, one event-ring batch per 2 s, link in U0 100 % of the time. Linux, which uses MSI-X, has no
problem with the same controller; why macOS's never arrive is still open.

**Fix:** the kext attaches to the controller in its own `IOMatchCategory`, next to AppleUSBXHCIPCI. It finds that
driver's interrupt source on its work loop and signals it every ~2.5 ms. That runs AppleUSBXHCI's normal interrupt
handler, exactly as a delivered interrupt would. Signalling only when `USBSTS.EINT` is set is not enough; the
controller raises it just as rarely.

| Boot-arg | Effect |
|---|---|
| `-fchxhcioff` | Do nothing |
| `fchxhcimode=1` | Signal only when `USBSTS.EINT` is set (diagnosis; doesn't fix the D509BA) |

State: `ioreg -lw0 -r -c IOPCIDevice | grep FCHXHCIFix-state`, e.g.
`1 sources, 123456 forwards, 123456 polls; running, mode 2 (every poll)`.

It matches `1022:7914` only, the FCH xHCI of Carrizo, Bristol Ridge and Stoney Ridge. Other AMD xHCIs work with
macOS's interrupts. Build: `make -C FCHXHCIFix` (macOS, MacKernelSDK in the repository root), or the CI artifact.
