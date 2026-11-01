# Sleep and wake

Status as of 0.1.0 (2026-10-31). Work on Sonoma's wake is **on hold**; this page is where to pick it up.

| Machine | macOS | Result |
|---|---|---|
| ASUS VivoBook 15 D509BA (A9-9425) | Catalina 10.15.7 | **Works:** sleeps (fans off), wakes on a key press with a clean picture. Needs `SSDT-GPRW` (below) |
| ASUS VivoBook 15 D509BA | Sonoma 14 | **Wake hangs:** the machine sleeps; a key press only starts the fans, with no picture, no panic and nothing logged after the sleep (see *Open: Sonoma*) |
| ASUS VivoBook 15 D509BA | Lid close | **Doesn't sleep:** macOS sees the machine in desktop mode (see *Open: lid*) |
| Dell Inspiron 3475, other machines, Big Sur–Ventura, Sequoia | | Untested |

The display-side fix is the same code on every macOS version and machine; it follows what Linux's DC driver does at
resume, using each machine's own VBIOS tables and transmitter parameters, with no copies of boot-time registers.

## What S3 does to the display

S3 powers the display block off. Before CarrizoRed's resume steps, after a wake:
- the VBIOS scratch registers (`BIOS_SCRATCH_0–15`, 0x5C9) and the backlight PWM (`BL_PWM_CNTL`/`CNTL2`/`PERIOD_CNTL`,
  `LVTMA_PWRSEQ_REF_DIV`, 0x4820–0x4822 and 0x481D) are back at reset values;
- DENTIST (0x124) has DPREFCLK at its reset divider (DID 0x64 = 100 MHz; the firmware sets 0x24 = 400 MHz at boot, from
  a 3.6 GHz VCO in IntegratedSystemInfo v1.9), and `DP_DTO0_MODULO` (0x142) is 0. With the modulo at 0 the DP stream's
  `DP_VID_M` (0x4AAA) is wrong and `DP_STEER_FIFO` (0x4AA5) overflows: a trained link that stays black;
- `DP_DPHY_INTERNAL_CTRL` (DP0 0x4AA7, defined only in Linux's `dce110_resource.c`) is 0 instead of 1, so with ASSR on in
  the panel the picture comes out as stripes;
- the AUX pads need DC's preparation before VBIOS AUX transfers work again.

Apple's Polaris driver redoes none of this for an APU: it runs ASIC_Init (VBIOS table 0) through
`AtomBiosProxy::initializeAsic`, but `CAILPostVBIOS` only posts at boot, and its display-clock path calls SetDCEClock,
which these VBIOSes don't have (table 46 missing; Apple's `m_setDceClock` is NULL). Apple also passes its Polaris
"no PLL" index 6 for DP pixel clocks.

## What CarrizoRed does

At sleep (`kIOMessageSystemWillSleep`):
1. saves the VBIOS scratch and backlight registers;
2. after Apple's DPCD 0x600 = 2 (panel to D3), powers the eDP panel off with the VBIOS (DIG1TransmitterControl v1.5
   POWER_OFF, the panel's parameters), as DC's `dce110_edp_power_control` does at suspend. `-crnosleeppoweroff`.

At wake, before Apple's first AUX transaction (all in `wrapExecuteBiosCommandTable`, `CarrizoRed/kern_cr.cpp`):
1. VBIOS EnableDispPowerGating (table 0x0D) INIT then DISABLE on pipes 0 and 1;
2. the display engine clock the way DC's `dce_set_clock` does on DCE 11.0: SetPixelClock V6 (table 0x0C) with
   `ucPpll` 8 (the DFS) and FirmwareInfo's `ulDefaultDispEngineClkFreq` (847.06 MHz on the D509BA), which also sets
   DPREFCLK and the DP DTO (DENTIST 0x18191111 afterwards: DPREFCLK 600 MHz, DISPCLK 847 MHz); a second call with
   `GEN_DPREFCLK` if the modulo is still 0. `-crnodispclk`;
3. the VBIOS scratch and backlight registers from before sleep, and an AUX engine reset;
4. eDP transmitter INIT with DC's parameters (`dcTransmitterParams`, built like `transmitter_control_v1_5`: PHY and HPD
   from Apple's call, connector 0x14, DP mode, 4 lanes, P0PLL, encoder 0). Apple's own parameters are DVI-mode
   DISABLE ones and broke link training;
5. a DC power cycle of the panel: POWER_OFF, 500 ms (the eDP T12 gap), POWER_ON, wait for HPD, clear `BLON_OVRD`.
   `-crnowakepowercycle`;
6. before each VBIOS AUX transfer after a wake, DC's `dce_aux` preparation: DDC pad in AUX mode, AUXN/AUXP impedance
   recalibration, `AUX_SW_DONE_ACK`, `RX_RECEIVE_WINDOW` 1. `-crnodcaux`.

Steps 1, 3 and 4 are `-crnowakerestore`.

Always (boot and wake):
- DP/eDP SetPixelClock with `ucPpll` = `ATOM_PPLL_INVALID` (0xFF) and no dividers, as DC's `set_pixel_clock_v6` does,
  so the VBIOS programs the DP DTO. `-crnodppclk`;
- after Apple writes DPCD 0x10A = 1 (ASSR in the panel), `DP_DPHY_INTERNAL_CTRL` = 1, as DC's `setup_panel_mode` does
  for eDP. `-crnoassrsource`.

Opt-in experiments that copy boot values back (not the Linux way, kept for bisecting): `-crwakeregrestore`,
`-crtps1restore`, `-crtmdprestore`, `-cruniphyrestore`, `-crauxhpd`.

## ACPI: SSDT-GPRW (D509BA)

On the D509BA the internal EHCI controller (EHC1, GPE 0x18: webcam and Bluetooth behind its hub) woke the machine at
once, every ~90 s, into a dark wake. `SSDT-GPRW` returns sleep state 0 for GPE 0x18 under Darwin only; XHC0 (GPE 0x19,
the external USB ports) stays a wake source, so USB keyboards still wake it. Lost: wake from a Bluetooth keyboard or
mouse.

It needs the OpenCore ACPI patch renaming only the method **definition** `GPRW` → `XPRW` (find `4750525702`, replace
`5850525702`, `Count` 1). Renaming all occurrences makes the callers skip the SSDT. Source: `Support files/acpi/SSDT-GPRW.dsl`
in the D509BA's EFI folder (CarrizoRed-RE). Other machines need their own check (`pmset -g log`, look for the wake
reason).

## Open: Sonoma wake hang (on hold)

Seen on the D509BA, Sonoma, same EFI and kext as the working Catalina:
- the disk log ends while macOS writes its sleep image (hibernatemode 3, `sleep params v2, type 2`);
- macOS first waits the full 30 s for `wifip2pd` (`PM notification timeout (pid …, wifip2pd)`) before going down;
- a key press only starts the fans: no picture, nothing more in the disk log, no panic in NVRAM.

NVRAM marks (`CR-wake`, `-crwakemark`, nvram2–4.bin) always end at Apple's GPU halt during sleep
(`Cail_Tonga_MicroEngineControlCp (call 8)`), 30 s after "will sleep". On Catalina those same calls are also the last
CarrizoRed events, followed by macOS writing the sleep image. There was never a "system will power on" and never a
"CPU sleeping" from the `IOCPUSleepKernel` hook. So either Sonoma never reaches the CPU sleep (a driver's power-off or
the sleep image hangs), or the hook or late NVRAM writes don't work there. The last build tried (679efdd) also logs
the power manager's own phases (`IOPMrootDomain::tracePoint`: 0x14 drivers, 0x17 platform, 0x18 CPUs, 0x2x wake) and
records in the `CR-wake` header whether the hooks went in; it hasn't been run yet.

**2026-11-02, first lead:** `pmset -g log` records every attempt as "Entering Sleep state" followed directly by the
next boot: no wake, no failure record. In every Sonoma log the last line is CarrizoRed's watcher tick about half a
second before Apple's GPU halt; on Catalina the log runs on through the drivers' power-off. The watcher read GPU, SMU
and display registers every 500 ms through the whole sleep, and on Sonoma the sleep image is set up after the GPU is
powered down, a longer window for a read of the powered-off GPU to stall a core. Since this build the watcher's register
reads pause from "will sleep" until the GPU is powered again (`-crnosleeppause` turns that off). Untested.

**2026-11-02, the watcher pause (4584599) didn't fix it.** pmset still shows "Entering Sleep state" and then the next
boot.

**Catalina vs Sonoma, Apple's GPU sleep and wake code (Catalina 10.15.7 kexts vs Sonoma 14.8.9; CAIL vs Sequoia
15.8.1, the same CAIL build as Sonoma with symbols).** Every difference, accounted for:

| Function | Catalina | Sonoma | Effect |
|---|---|---|---|
| `AMDGraphicsAccelerator::systemWillSleep` | `stopGfxOff`, `disableHWPerfPollTimer` | Baffin's empty gfx-off slot; the timer stop moved into `powerOff` | none (`stopGfxOff` is empty on Catalina too) |
| same | `AMDHWMemory::hasAllocation` + `free` | `safeFree` | same |
| same | – | releases the `reduceVRAM` block (a debug feature) | none normally |
| same | – | power-event ring entries, flag bits | bookkeeping |
| `AMDHardware::powerOff` | `disableCmdBufTiming` | removed | none on the GPU |
| same | `disableTimeStampInterrupts` | `disableInterrupts`: the same, plus `AMDHWVMM::disableInterrupts` | none (empty on VI) |
| `stopHWEngines`, `powerOffHWEngines` (the five MicroEngineControl halts in the logs) | | same calls, other status bits | bookkeeping |
| `powerDownCAIL` → `AmdCailServices::powerDown` → `Cail_Powerdown(0x20)` → `Cail_Tahiti_Powerdown` | | call trees identical to depth 4 | none |
| `setVirtualSpace(nullptr)`, `disableAllocations`, `AMDHWRegisters::disable` | | same, one more status bit | bookkeeping |
| wake: `systemDidWake`, `internalDidWake`, `willWake`, `powerUpCAIL`, `powerUpHWEngines` | | same, vtable shifts and timestamps | none |
| wake: `AMDHardware::powerUp` | same steps, plus `infiniteWaitOnPowerUp` on the PM4 channel and cmd-buffer timing | no `infiniteWaitOnPowerUp`, records time since power-up | wake only |
| wake: `powerUpHW` | `initialize_hardware`, `enableInterruptHubAgp` | `initializeHardwareRegisters` directly; no interrupt hub buffer | wake only |
| AMD9500Controller, AMDSupport, AMDFramebuffer sleep code | | compiler changes (`memset`, typed allocators); `setPanelPowerState` no longer resets the cached HPD state | wake only |

CarrizoRed's own events during the sleep are identical on both (panel off, the same five halts). CarrizoRed's
version-specific code in this window, the dyld page patcher (`cs_validate_page` on Sonoma, `cs_validate_range` on
Catalina), only compares and copies page bytes. The `wifip2pd` 30 s wait is a Sonoma daemon (no Wi-Fi driver is
loaded) and doesn't hold the sleep. So no difference in the GPU's sleep path explains a hang; what's left is either
the wake (where `powerUp` and `powerUpHW` did change) or something outside the GPU, and the logs can't tell which,
because Sonoma leaves no record after the GPU halt. The boot with `-crnoctl -crnoaccel` (no Apple GPU drivers)
decides it.

Next steps, cheapest first:
1. On Sonoma after a failed attempt: `pmset -g log | grep -iE "sleep|wake|failure|darkwake" | tail -80` (Apple's own
   record of the failure and its phase), the disk logs of the failed boots (`/private/var/log/CarrizoRed-*.log`), and
   `log show --start … --end …` around the attempt;
2. a run with `-crwakemark` on Catalina (works) and Sonoma, and compare the phase lists;
3. Caps Lock LED after the key press (it toggles only if the kernel and VoodooPS2 run) and a 3-minute wait (the sleep
   watchdog panics if the kernel runs but the wake never completes);
4. if the sleep never completes: find the driver from the 0x14 trace details; if the CPU never resumes: firmware and
   OpenCore (`Booter` quirks, hibernation, `HibernateMode`).

## Open: lid

Closing the lid doesn't sleep. The kernel log shows `clamshell closed 1, disabled 0, desktopMode 1`: macOS treats the
machine as a desktop with an external display (sleep factors include `ExternalDisplay`), which disables clamshell sleep.
Likely cause: the eDP panel attaches as `AppleDisplay` instead of `AppleBacklightDisplay` (check with
`ioreg -lw0 -r -c AppleBacklightDisplay`). Belongs with the brightness-keys work, which needs the same thing.

## Also open

- Repeated sleep cycles, wake from a USB keyboard, more than a few minutes awake after a wake (an old `preemption_level`
  panic ~88 s after wake on Catalina; `-crpreemptcheck` names the culprit), the Dell, Big Sur–Sequoia.
- Harmless-looking leftovers after a wake on Catalina: `DISPOUT_ERROR_COUNT` 0x54, one failed DPCD transfer (likely
  MST 0x111), a 0x600 = 1 retry at boot.

## Diagnostics

| Argument | What it gives |
|---|---|
| (default) | The disk log `/private/var/log/CarrizoRed-<time>.log`: sleep, wake, every VBIOS call of a wake, a display report 30 s after a wake (or 1 s after a failed DPCD transfer) |
| `-crwakemark` | `CR-wake` in NVRAM (Apple's GUID 7C436110-…): CarrizoRed's events from "will sleep" to 30 s after the wake, the CPU's sleep and resume, the power manager's phases. Forced flash writes, up to 100 per sleep |
| (default) | Wake watchdogs: a panic with the black-box report if a wake doesn't complete (45 s after "will power on", or 150 s of awake time after "will sleep" with the watcher stopped). `-crnowakepanic` |
| `-crpreemptcheck` | Panics naming the event source or interest handler that leaks a preemption disable |

Read NVRAM with OpenShell `dmpstore -all -s fs0:\nvram.bin`; `CR-wake` is plain text separated by `;`.
