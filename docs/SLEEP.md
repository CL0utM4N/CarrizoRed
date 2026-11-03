# Sleep and wake

Status 2026-11-03. Sleep works on Catalina, Big Sur and **Monterey 12.7** (fixed: macOS didn't flush the CPU caches
before S3, see *The fix: cache flush before S3*). Sonoma hung the same way as Monterey and is next to test. This page is
where to pick it up.

| Machine | macOS | Result |
|---|---|---|
| ASUS VivoBook 15 D509BA (A9-9425) | Catalina 10.15.7 | **Works:** sleeps (fans off), wakes on a key press with a clean picture. Needs `SSDT-GPRW` (below) |
| ASUS VivoBook 15 D509BA | Sonoma 14 | **Wake hung** like Monterey (no picture, no panic); retest with the cache flush (see *Open: Monterey and Sonoma*) |
| ASUS VivoBook 15 D509BA | Big Sur 11.7 | **Works** (2026-11-02), slow: ~26 s of GPU power-up (see *Also open*) |
| ASUS VivoBook 15 D509BA | Monterey 12.7 | **Works** (2026-11-03, cache flush before S3): back 27 s after the CPU resumes, 20 s of it the GPU power-up (see *Also open*) |
| ASUS VivoBook 15 D509BA | Lid close | **Doesn't sleep:** macOS sees the machine in desktop mode (see *Open: lid*) |
| Dell Inspiron 3475 (A6-9225) | Tahoe | **Sleeps and wakes** with the cache flush (2026-11-03, `CarrizoRed-1793707503.log`), but EHC1 wakes it at once into a dark wake, every sleep, like the ASUS before `SSDT-GPRW`. Its DSDT has the same `GPRW(0x18, …)` for EHC1 (XHC0, with the keyboard and mouse, is 0x19), so the ASUS `SSDT-GPRW` and rename went into its Tahoe EFI; untested |
| Other machines, Big Sur–Ventura, Sequoia | | Untested |

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

## The fix: cache flush before S3

The ACPI specification has the OS write back and invalidate the CPU caches before it sets SLP_EN, and ACPICA's
`AcpiHwLegacySleep` does so (`ACPI_FLUSH_CPU_CACHE`). Apple's port calls `pal_machine_sleep` at that point instead,
which on a Mac returns 0 and does nothing else, in every version: Intel hardware flushes on S3 entry by itself. The last
`wbinvd` before S3 is in `install_real_mode_bootstrap`, before `acpi_sleep_cpu` saves the CPU state. On this AMD APU
whatever is still only in cache at S3 is lost, the saved CR3, GDT, IDT and stack included, and the wake then dies in
`acpi_wake_prot`/`Lwake_64` before any driver runs: fans on, no picture, no panic, no Caps Lock. Whether those lines
reached memory by chance follows each version's memory layout, which is why Catalina and Big Sur woke and Monterey and
Sonoma didn't.

CarrizoRed turns `pal_machine_sleep`'s 8 bytes into `wbinvd; xor eax, eax; ret` (`patchSleepFlush`). Nothing in the
kernel calls it; Apple's ACPI driver calls it once, right before writing SLP_TYP and SLP_EN. On by default;
`-crnosleepflush` turns it off.

Found with the Caps Lock probe (`-crcapsprobe`, below): the trampoline turned the LED on (the firmware hands back),
no checkpoint in `acpi_sleep_kernel` flashed, and the memory-check build, whose callback happened to run a `wbinvd`
right before S3, got through the whole resume. Monterey 12.7 with the fix (`CarrizoRed-1746689635.log`): will sleep
99 s, CPU resumed 126.3 s, GPU power-up 131.0 to 151.3 s, has powered on 153.3 s.

The one wake before that (nvram10, the memory-check build) was cut short by CarrizoRed's 45 s wake watchdog while the
GPU was still powering up, because the SMU microcode reload timed out (11.6 s) that time. On the fixed wake it took
31 ms. Still to do: a watchdog that waits while the power-up is making progress, and why the reload can time out.

## Open: Monterey and Sonoma wake hang (solved for Monterey: the cache flush above)

First seen on the D509BA, Sonoma, same EFI and kext as the working Catalina:
- the disk log ends while macOS writes its sleep image (hibernatemode 3, `sleep params v2, type 2`);
- macOS first waits the full 30 s for `wifip2pd` (`PM notification timeout (pid …, wifip2pd)`) before going down;
- a key press only starts the fans: no picture, nothing more in the disk log, no panic in NVRAM.

NVRAM marks (`CR-wake`, `-crwakemark`, nvram2–4.bin) always end at Apple's GPU halt during sleep
(`Cail_Tonga_MicroEngineControlCp (call 8)`), 30 s after "will sleep". On Catalina those same calls are also the last
CarrizoRed events, followed by macOS writing the sleep image. There was never a "system will power on" and never a
"CPU sleeping" from the `IOCPUSleepKernel` hook. So either Sonoma never reaches the CPU sleep (a driver's power-off or
the sleep image hangs), or the hook or late NVRAM writes don't work there. The last build tried (679efdd) also logs
the power manager's own phases (`IOPMrootDomain::tracePoint`: 0x14 drivers, 0x17 platform, 0x18 CPUs, 0x2x wake) and
records in the `CR-wake` header whether the hooks went in; run as nvram40/41 (below).

**2026-11-02, first lead:** `pmset -g log` records every attempt as "Entering Sleep state" followed directly by the
next boot: no wake, no failure record. In every Sonoma log the last line is CarrizoRed's watcher tick about half a
second before Apple's GPU halt; on Catalina the log runs on through the drivers' power-off. The watcher read GPU, SMU
and display registers every 500 ms through the whole sleep, and on Sonoma the sleep image is set up after the GPU is
powered down, a longer window for a read of the powered-off GPU to stall a core. Since this build the watcher's register
reads pause from "will sleep" until the GPU is powered again (`-crnosleeppause` turns that off). It didn't help (below).

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
decides it (on Monterey, `-crnoaccel` alone still hangs: see the tests below).

**nvram40/41 (-crwakemark):** the power manager's phases 13, 14, 15, 16 all pass, with every driver powered down,
the GPU included. The last mark is "> IOHibernateSystemHasSlept". The nvram41 panic (an NX fault in a test hook on
`IOPlatformExpert::sleepKernel`, reverted in 0c93c67) shows the sleep got past `IOHibernateSystemHasSlept` and
`evaluateSystemSleepPolicyFinal` into the platform's `sleepKernel`. NVRAM writes after `IOHibernateSystemHasSlept`
(which does the last NVRAM sync before sleep) don't persist, so the marks can't see further. That makes the missing
"CPU sleeping" in nvram40 meaningless. What's left: the platform sleep, `IOCPUSleepKernel` → `acpi_sleep_kernel` →
`hibernate_write_image` (the sleep image, hibernatemode 3, written with polled disk I/O, drawing progress into the
console framebuffer set up by `IOHibernateSystemHasSlept`), S3 entry, and the resume. A one-off `hibernatemode 0` sleep
(diagnostic only) tells the sleep image apart from the rest.

**hibernatemode 0 also hangs**, so the sleep image isn't the cause.

**2026-11-02, which versions wake (same EFI, same kext):** Catalina wakes, Big Sur 11.7 wakes, Monterey 12.7 and
Sonoma hang. The Catalina and Big Sur–Sequoia EFIs are identical apart from the kext build and `config.plist`, and the
configs apply equivalent patches on 19.x and 20.x. From 21 (Monterey) the config adds IOPCIFamily "Disable 10 bit
tags" and the `thread_*` "Remove non-monotonic time panic" patches; the CPU-ID patches only change versions.

**Big Sur's wake (`CarrizoRed-1746611414.log`, `pmset.rtf`)** is a real resume from RAM: the kernel prints
"regular wake" (pmset's "Wake from Safe Sleep" only reflects hibernatemode 3), then trace point 0x22, the platform
wake actions (`IOPlatformWakeAction -> AppleRTC, AppleKeyStore, AppleSMC`), 0x23, `IOCPUSleepKernel exit`, and the
GPU gets "system will power on". The wake feels slow (about a minute) because of the GPU power-up (*Also open*), not
because of a sleep fault. On Sonoma the GPU never gets "will power on", so the resume stops in the kernel before any
driver wakes.

**The Monterey 12.3 CPU wake order (tested, not the cause).** XNU source diff, xnu-7195.141.2 (Big Sur) vs
xnu-8020.140.41 (Monterey). In `IOCPUSleepKernel` (iokit/Kernel/IOCPU.cpp):

| | Big Sur, Monterey 12.0–12.2 (xnu-8019) | Monterey 12.3+ (xnu-8020.101), Sonoma |
|---|---|---|
| 1st | trace 0x22, `IOPlatformActionsPostResume` (AppleRTC, AppleKeyStore, AppleSMC = VirtualSMC) | trace 0x23, `processor_start` of the other cores |
| 2nd | trace 0x23, `processor_start` of the other cores | trace 0x22, `IOPlatformActionsPostResume` |

xnu-8020.140 keeps the old order for iOS only ("temporarily", rdar://88891040); the Sonoma 14.8.9 kernel binary has
the new order (0x23, `processor_start_reason`, 0x22). With `cpus=1` there is no other core to start, and Monterey
still hangs (`hw.ncpu` wasn't checked on that boot).

### Monterey 12.7 tests (2026-11-02 to 11-03)

Every one still hangs: the key press starts the fans, with no picture, no panic and no Caps Lock LED.

| Test | Rules out |
|---|---|
| `hibernatemode 0` | the sleep image |
| `cpus=1` | the CPU wake order (if `cpus=1` took effect) |
| `cpus=1`, plus the three Monterey-only config patches off (both "Remove non-monotonic time panic", IOPCIFamily "Disable 10 bit tags") | the config: Monterey then gets the same patches as Big Sur, apart from version splits of the CPU-ID patches |
| `-crnoaccel` (display controller only, no accelerator or CAIL) | Apple's accelerator and its wake |
| `-crtscsync` (below) | the cores' TSCs: at boot they are -0x4f and +0x23 ticks apart, i.e. in sync |
| Caps Lock during the hang | nothing; but no LED, so nothing in macOS answers. Either the firmware never hands back control, or the kernel stopped with interrupts off |
| RTCMemoryFixup 1.0.7 with `rtcfx_exclude=00-FF` (no macOS writes to CMOS memory) | a CMOS byte macOS writes at sleep clashing with the firmware's S3 resume |
| `-crnoaccel` plus CarrizoRed's sleep and wake steps off (`-crnosleeppoweroff -crnowakerestore -crnowakepowercycle -crnodcaux -crnodispclk`) | set up, result not reported |

No CarrizoRed at all (kext and `device-id` spoof off): Monterey doesn't sleep at all (the screen stays on), so this
split can't be made that way.

**`-crtscsync`** (2ef26df, test only): after each `processor_start(_reason)` all CPUs read the TSC together inside
`mp_rendezvous`, and any CPU behind the highest reading moves forward by the difference (never backwards). It also
measures once at boot. Written because Monterey on Intel panics at wake with "Non-monotonic time" inside
`processor_start` from `IOCPUSleepKernel` (acidanthera bugtracker #1900), the panic the AMD config's `thread_*`
patches remove. No existing kext fits Stoney: ForgedInvariant's license excludes use with AI tools, AmdTscSync only
syncs on a timer seconds after the wake, CpuTscSync writes `TSC_ADJUST` (MSR 0x3B, not on Stoney) from Monterey on.

**Hibernation (`hibernatemode 25`, diagnostic only):** the machine powered off, which is what hibernation does, and
nvram9 holds a new `boot-image` (the image's location, set once the kernel has written it). So on Monterey the whole
sleep side works, image write and power-off included. The restore wasn't tested: OpenCore's `Misc > Boot >
HibernateMode` is `None` in the test EFI, and the image key (`IOHibernateRTCVariables`) isn't in nvram9.

### Monterey vs Big Sur, Apple's code on the wake path

All compared function by function on normalized disassembly, Big Sur 11.7.11 vs Monterey 12.5.1 (each binary taken
from its kernel collection), plus the XNU source.

- **The kernel's resume** is equivalent. `acpi_wakeup.s` is identical in source; `acpi_sleep_cpu` and
  `acpi_wake_prot_entry` differ only in where their save area sits. `acpi_sleep_kernel` only adds
  `kptimer_stop_curcpu`/`kptimer_curcpu_up` (kperf). Unchanged instruction for instruction: `x86_64_pre_sleep`/
  `post_sleep`, `rtc_nanotime_init`, `pat_init`, `pmMarkAllCPUsOff`, `rtc_decrementer_configure`, `pmTimerRestore`,
  `pal_serial_init`, `kdebug_wake`. Compiler output or struct offsets only: `install_real_mode_bootstrap` (the wake
  trampoline, still copied to physical 0x2000), `cpu_syscall_init`, `lapic_configure`, `mtrr_update_cpu`, `init_fpu`,
  `mca_cpu_init`. `ucode_update_wake_and_apply_cpu_was` only acts when a microcode update is loaded.
- **AppleACPIPlatform:** the sleep and resume path is identical instruction for instruction (`sleepPlatform`,
  `handleSleepKernelCallback`, `sleepKernelCallback`, `picSleepWake`, `AcpiHwLegacySleep`/`Wake`/`WakePrep`,
  `AcpiSetFirmwareWakingVector`, `AcpiEnterSleepStatePrep`, `AcpiHwExecuteSleepMethod`, `systemDidWakePhase3`/`4`).
  The rest is compiler output (register allocation, inlining, typed `kalloc`): `AcpiEnterSleepState`,
  `AcpiLeaveSleepState(Prep)`, `systemWillSleep`, `systemDidWakePhase1`, `AppleACPICPU::initCPU`/`startSerialized`.
- **AppleRTC:** `rtcRead`/`rtcWrite` dropped an `& 0x7f` on bank 0, where the index is already below 0x80. No
  change in behaviour.

### Monterey vs Big Sur, PCI and USB power (2026-11-03)

Also compared, Big Sur 11.7.11 vs Monterey 12.5.1 (Boot KCs; IOPCIFamily source 428.120.4 vs 484.140.2):
- **IOPCIFamily's `_PS0`/`_PS3`:** `IOPCIDevice::attach` (which methods exist) and `setPCIPowerState` (evaluate `_PSx`,
  then write PMCSR) are the same apart from locks and allocators. `_PS3` runs before the D3 write, so XHC0's and EHC1's
  `_PS3` see `U_PS`/`E_PS` != 3 and do nothing at sleep on both versions (with `XHCD`/`EHCD` = 1, which is what
  `SSDT-XOSI` leaves on the D509BA: `FINI` clears them only below Windows 8).
- **AppleUSBXHCIPCI RTD3:** reworked in Monterey (`setPowerStateGated`, "scheduling/initiating RTD3 entry"), but
  `pciWakeInitializeForRTD3` only enables it when the controller's ACPI node has `_GPE`; XHC0 has none, on both. 1022:7914
  gets the generic `AppleUSBXHCIPCI` (`AppleAMDUSBXHCIPCI` matches only 1002:73a4/73a6).
- **CarrizoRed's kernel hooks:** by default only `cs_validate_page` (dyld page patches), not on the wake path.
  `acpi_sleep_kernel` and `IOCPUSleepKernel` have plain prologues (pushes, `sub rsp`, `mov r15, rsi`; no RIP-relative
  operand in the first 14 bytes) on both, so a probe can hook them.
- **FADT:** day alarm index 0x0D (AMD FCH register D), month 0, century 0x32; RTC0 exposes bank 0 only. Nothing odd.

Unexplained: the nvram41 panic was a kernel NX fault in Lilu's trampoline memory at `IOPlatformExpert::sleepKernel`,
while the `tracePoint` routes, in the same Lilu buffer, had run earlier in that sleep. Either that route never got a
real trampoline (Lilu's 4 KB buffer full: see the comment above `routePool` in `kern_cr.cpp`), or the late sleep makes
that memory non-executable. In the second case, the first Lilu-plugin trampoline to run during the wake (ECEnabler on
`_WAK`'s EC accesses, NVMeFix, VirtualSMC) faults with interrupts off, which would also hang without a panic. The
kext bisect below tells these apart.

### Other projects (2026-11-03)

No published fix for an AMD APU waking on Monterey 12.3+. A Ryzen 1700X desktop had the same split (11.6.4 wakes, 12.3
doesn't) and reported a fix from OpenCore 0.8.0, SMBIOS iMac19,2 then MacPro7,1, and NVRAM `wake-failure`; this EFI
already runs OpenCore 1.0, and the SMBIOS part is unproven (several changes at once). NootedRed's reports are mostly
a black screen with the system running, or a graphics wake-timeout panic; neither matches.

More, 2026-11-03:
- acidanthera bugtracker #1900 (ASUS UX430AU and other Intel laptops, Big Sur fine, Monterey "black screen, fans
  spinning"): a "Non-monotonic time" panic in `IOCPUSleepKernel` → `processor_start`, fixed by CpuTscSync 1.0.6 plus
  `TSC_sync_margin=0`. Here the config's `thread_*` patches remove that panic and `cpus=1` still hangs.
  `TSC_sync_margin` only matters for the `start_cpu` barrier, which is the same code in xnu-7195 and xnu-8020 and only
  kprintfs on release kernels.
- AMD OS X, Ryzen 5800H laptop (Lenovo Legion 5): Monterey panics without `cpus=1` (no TSC_ADJUST); wake from sleep
  listed as unsolved.
- AMD OS X, Ryzen 5950X desktop on Monterey 12.6.8: wake needed a CMOS reset; fixed after several changes at once
  (USB map, `ReleaseUsbOwnership`, own SSDT-EC/USBX). Not isolated.
- Dortania's RTC page: AppleRTC writes to CMOS areas the firmware uses cause CMOS errors, safe-mode boots and panics
  on reboot or wake; `DisableRtcChecksum` (0x58–0x59 only), RTCMemoryFixup with `rtcfx_exclude=00-FF` bisected
  by halves, then `rtc-blacklist`. No report of it causing a silent wake hang. **Tested on Monterey: RTCMemoryFixup
  doesn't fix it**, so it isn't AppleRTC's CMOS writes.
- No report found tying `ProtectMemoryRegions` or `UEFI > ReservedMemory` to a Monterey wake.

### Where it stands

The code macOS runs on the way down and back up is the same in Big Sur (wakes) and Monterey (hangs), and the hang
leaves nothing running. What changes between versions around that code is **memory layout** (where the kernel, the
kernel collections and their data sit, against memory the firmware's S3 resume uses) or **hardware state** a driver
leaves behind at sleep.

Next, cheapest first:
0. **No CarrizoRed, then fewer kexts** (no build needed): Monterey with CarrizoRed and the `device-id` spoof off
   (VESA display), `pmset -g log` as the sign of life. If that wakes, the cause is in the GPU stack. If it still hangs,
   go down to Lilu, VirtualSMC and VoodooPS2 only, then add the rest back in halves: ECEnabler, SMCBatteryManager,
   NVMeFix, RestrictEvents (`revpatch`), the VoodooI2C set, FCHXHCIFix with the xHCI reset patch, USBMap. Also free:
   sleep within a minute of boot. If that wakes and later sleeps don't, firmware memory that macOS reuses is a lead
   (`ProtectMemoryRegions`, `UEFI > ReservedMemory`).
   Tried 2026-11-03: without CarrizoRed, Monterey doesn't sleep at all (screen stays on); the kext halves still apply
   with CarrizoRed kept.
1. **Caps Lock probe, `-crcapsprobe`:** the LED through the keyboard controller (i8042 0xED, the keyboard port
   re-enabled with 0xAE first). Going to sleep, at the start of `acpi_sleep_kernel`: on 1 s, then off (proves the
   controller takes the commands). After the key press: on when `acpi_sleep_kernel` returns, held 3 s; off when
   `IOCPUSleepKernel` returns (CPUs started, platform wake actions run). No LED: the firmware's resume, or the
   kernel's resume up to the end of `acpi_sleep_kernel`. Stays on: between there and the end of `IOCPUSleepKernel`.
   On 3 s then off: later than thought.
   **Result (2026-11-03, Monterey): the 1 s blink going to sleep, then nothing after the key press.** The wake never
   reaches the end of `acpi_sleep_kernel`. Second version: the wake trampoline (physical 0x2000, the FACS waking
   vector; `AcpiSetFirmwareWakingVector` is called only in `sleepPlatform`, before `acpi_sleep_kernel`) starts with a
   jump to a real-mode stub at 0x2C00 that sets the LED, then runs the original `cli` and `lgdt` and jumps back. LED
   on after the key press: the firmware reached macOS, and the hang is in the trampoline, `acpi_wake_prot` or the
   start of `acpi_sleep_kernel`'s resume. No LED: the firmware's S3 resume never hands over. Three short blinks going
   to sleep instead of one long one: the trampoline didn't start as expected and wasn't patched.
   **Result (2026-11-03, Monterey): the LED comes on after the key press and stays on.** The firmware's S3 resume
   works and hands over to macOS; the hang is between the trampoline and the end of `acpi_sleep_kernel`: the switch
   to 64-bit mode (`acpi_wake_prot`), the CPU state restore (`acpi_wake_prot_entry`), then `acpi_sleep_kernel`'s resume
   steps (`rtc_nanotime_init`, `x86_64_post_sleep`, `cpu_syscall_init`, `mca_cpu_init`, `mtrr_update_cpu`,
   `ucode_update_wake_and_apply_cpu_was`, `pat_init`, `vmx_resume`, `lapic_configure`, `rtc_decrementer_configure`,
   `init_fpu`, `IOCPURunPlatformActiveActions`, `pmCPUMarkRunning`, `pmTimerRestore`, `rtc_timer_start`,
   `install_real_mode_bootstrap`). Code identical to Big Sur's, so the cause is in what it runs on: page tables,
   descriptor tables and MSRs it restores, or a patch or hook in that path.
   Third version: checkpoints. The calls `acpi_sleep_kernel` makes after `acpi_sleep_cpu` to those steps go through
   thunks (call sites redirected; no Lilu trampolines, most of the functions start RIP-relative) that flash the LED off
   and on (0.25 s each, timed by the TSC) and jump on. The disk log's "Caps Lock checkpoints" line at boot maps flash
   numbers to steps. No flash: the hang is in `acpi_wake_prot`/`acpi_wake_prot_entry`.
   **Result (2026-11-03, Monterey): no flash, the LED just stays on** (to confirm: the boot log's checkpoint line
   lists steps, i.e. the call sites were patched on 12.7). If so, the hang is in `acpi_wake_prot` (32-bit, paging
   off: `SWITCH_TO_64BIT_MODE` with `BootPML4`, stack `low_eintstack`, all in `__HIB`) or `Lwake_64` (restore CR4,
   EFER.NXE, CR0, CR3, GDT/LDT/IDT, segments, GS bases, TR, then the saved stack and the return into
   `acpi_sleep_kernel`). The CPU start-up at boot runs the same 32-bit switch and works, so what differs at wake is
   the state saved before sleep and the memory it all sits in. Lead: the firmware's S3 resume overwriting memory the
   kernel uses (`__HIB` code and data, `BootPML4`, the save area; Linux reserves low memory for firmware that does
   this), at addresses that move with the macOS version's layout.
   Checkpoint line confirmed on 12.7 (all 11 steps listed), so the result stands.
   Fourth version, memory check: just before S3 (a callback in front of the platform's, which `acpi_sleep_cpu` calls
   once the CPU state is saved) CarrizoRed checksums what the early wake reads, in groups: 1 `acpi_wake_prot`, 2
   `BootPML4` and its tables on the switch's path, 3 `Lwake_64`'s code, 4 the save area (`master_gdtr`, `saved_*`;
   `low_eintstack` ends right below it), 5 the kernel's page tables on the restore's path, 6 GDT, 7 IDT, 8 TSS, 9 the
   stack `Lwake_64` returns on (page tables without accessed/dirty bits). At wake, 0x2800 sends the trampoline to a
   32-bit stub at 0x2C80 that sums it all again before `acpi_wake_prot`: all equal, LED off and on to the 64-bit
   switch; a difference, the LED blinks the group number forever (short off-blinks, then 1 s on).
   **Result (2026-11-03, Monterey): the LED went off (memory intact), then 11 flashes, the 3 s hold, and off: the
   whole low-level resume ran, `IOCPUSleepKernel` included, for the first time.** What this build changed before S3:
   the callback ran a `wbinvd` right before `AcpiEnterSleepState`. **Cause: no cache flush before S3.** ACPICA's
   `AcpiHwLegacySleep` flushes the caches before SLP_EN (`ACPI_FLUSH_CPU_CACHE`, which the ACPI specification
   requires); Apple's port calls `pal_machine_sleep` there instead, which on a Mac only returns 0 (Big Sur and
   Monterey alike), and `AcpiHwLegacySleep`, `AcpiEnterSleepState` and `handleSleepKernelCallback` have no `wbinvd`.
   The last `wbinvd` before S3 is `install_real_mode_bootstrap`'s in `sleepPlatform`, before `acpi_sleep_cpu` saves the
   CPU state. Intel hardware flushes on S3 entry; on this AMD APU whatever is only in cache is lost, and the wake
   depends on whether the saved state reached memory by chance, which follows the version's memory layout.
   **Fix (default, `-crnosleepflush` turns it off):** `pal_machine_sleep`'s 8 bytes become `wbinvd; xor eax, eax;
   ret`; nothing in the kernel calls it, Apple's ACPI driver calls it once, right before writing SLP_TYP/SLP_EN. The
   memory check is now opt-in (`-crwakememcheck`). Still open: the wake stops after `IOCPUSleepKernel`.
   **That wake (nvram10, the memory-check build): CarrizoRed's wake watchdog panicked, "wake stuck 45 s after
   kIOMessageSystemWillPowerOn: system never powered on, watcher ticks 83".** So the kernel was fully back and
   running (the watcher ticked), the drivers had "will power on", and the GPU was in its power-up: the SMU microcode
   reload timed out after 11.6 s (`resp b0033 b0034 b0252 b0254...`), one CAIL wait failed (`[ec05]&1!=0`), then
   `pm4.initializeMicroEngine` took 7.8 s. Big Sur's whole wake took about a minute, so the 45 s watchdog cut a slow
   wake short. Next: the flush build with `-crnowakepanic`, waiting ~2 minutes; then the watchdog needs to allow for
   a GPU power-up that is still making progress, and the SMU reload timeout needs a look.
2. **Hibernation restore:** `HibernateMode` `NVRAM` in the test EFI, `hibernatemode 25`, power on. If the session
   comes back, macOS's side of the wake works and the firmware's S3 resume is the problem.

Older ideas, superseded: `pmset -g log` and `log show` around a failed attempt (pmset only shows "Entering Sleep
state" then the next boot); `-crwakemark` phase lists (NVRAM writes stop at `IOHibernateSystemHasSlept`).

## Open: lid

Closing the lid doesn't sleep. The kernel log shows `clamshell closed 1, disabled 0, desktopMode 1`: macOS treats the
machine as a desktop with an external display (sleep factors include `ExternalDisplay`), which disables clamshell sleep.
Likely cause: the eDP panel attaches as `AppleDisplay` instead of `AppleBacklightDisplay` (check with
`ioreg -lw0 -r -c AppleBacklightDisplay`). Belongs with the brightness-keys work, which needs the same thing.

## Also open

- **The GPU power-up takes about 20 s at every boot and wake** (all macOS versions, Catalina included; most of the
  ~26 s `WakeTime` on Big Sur). Apple's engine-control code waits about 2 s at a time: `PP_InitializeEX` 4 s (between
  `CAILQueryASICInfo` and `PHM_Initialize`), `Cail_Tonga_MicroEngineControlCp` 8 s and 6 s, and
  `Cail_Tonga_MicroEngineControlSdma` 2 s twice (`pm4.start` 14 s, `sdma.start` 4 s, `hw.powerUp` 20 s). It's an
  existing delay, not a sleep fault; finding what those waits time out on would save ~20 s per boot and wake.
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
| `-crcapsprobe` | Test: the Caps Lock LED shows how far a wake gets (*Where it stands*) |
| `-crtscsync` | Test: syncs the cores' TSCs after the wake starts them; "TSC ..." lines in the disk log, the boot measurement too |
| `-crpreemptcheck` | Panics naming the event source or interest handler that leaks a preemption disable |

Read NVRAM with OpenShell `dmpstore -all -s fs0:\nvram.bin`; `CR-wake` is plain text separated by `;`.
