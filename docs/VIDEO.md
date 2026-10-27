# Video decode and encode (UVD / VCE) on Stoney

Research notes (2026-10-17): what Apple's Polaris stack has, how Stoney differs, and a plan. Stage 1 is
implemented behind `-crvideo` (see Progress); without it CarrizoRed skips the video engines' power-up.

## The engines

| | Stoney (Linux `vi.c`) | Polaris 10/11 | Carrizo |
|---|---|---|---|
| Decode | **UVD 6.2** (`uvd_v6_2_ip_block`) | UVD 6.3 | UVD 6.0 |
| Encode | **VCE 3.4** (`vce_v3_4_ip_block`), one instance | VCE 3.4, two instances | VCE 3.1 |
| UVD encode rings | none | 2 (used for HEVC encode) | none |
| Codecs, decode | H.264, HEVC, MPEG-2/4, VC-1, JPEG (`cz_video_codecs_decode`) | adds HEVC 10-bit | as Stoney |
| Codecs, encode | H.264 only (`tonga_video_codecs_encode`) | H.264 (VCE), HEVC (UVD encode) | H.264 |

Linux drives UVD 6.0/6.2/6.3 with one file (`uvd_v6_0.c`); the only 6.2/6.3 difference is the UVD encode rings
(`uvd_v6_0_enc_support`: Polaris only). VCE 3.4 on Stoney follows the Polaris paths (`asic_type >= CHIP_STONEY` in
`vce_v3_0.c`), with VCE1 harvested (`vce_v3_0_get_harvest_config`: Fiji and Stoney have one VCE).

## What Apple ships (macOS 15.8.1 KDK)

- `AMDRadeonX4000` has the engine classes for exactly Polaris's versions: `AMDUVD6v3HWEngine` (+ decode ring and
  UVD-encode rings/channels), `AMDVCE3v4HWEngine` (+ rings, LLQ rings), a generic `AMDVIUVDHWEngine`, user-client
  contexts `AMDAccelUVDContext` / `AMDAccelVCEContext`, and AMD's shared multimedia layer (`AMDSMLVIInterface`
  `createUVD`/`createVCE`, `getUVDFW`/`getVCEFW` through virtual calls). User space (AppleGVA / VideoToolbox and AMD's
  video driver bundle) talks to those contexts; it isn't in the KDK.
- **Firmware.** VCE images are embedded in the accelerator (chunks of Linux's Polaris and Carrizo VCE images match at
  `0x2da110` / `0x2aec50`, with different versions and headers). UVD firmware isn't stored in plain form for any chip:
  HWLibs has `Cail_MCILDecryptUVDFW`, and `UVDFWLib_GetFirmwareImage` is a stub. Nothing matches Linux's
  `stoney_uvd.bin` or `stoney_vce.bin`.
- **CAIL** (HWLibs) has a Stoney video path: `Stoney_check_vce_harvested`, `Stoney_set_uvd_clk_gating_branches(_in_dpg_mode)`,
  `stoney_power_down_uvd_block_for_dpg_mode`, `stoney_vce_pgfsm`, plus the generic `CailUvdInit`, `CailUvdPowerOnOff`,
  `CailVceInit`, `CailVcePowerOnOff`. Power on/off goes through PowerPlay (`PHM_PowerGateUVD`/`VCE`), which under the
  Dummy back end does nothing (`PhwDummy_PowerGateUVD`): the engines stay power-gated, and touching their registers
  then can hang the bus, which is why CarrizoRed skips their power-up today.

## What has to be done

1. **Power.** Send SMU8's `UVDPowerON` (0x8, argument 1 = dynamic power gating) / `VCEPowerON` (0xA) when CAIL powers
   the engine up, and `UVDPowerOFF` (0x7) / `VCEPowerOFF` (0x9) when it powers down: hook `PhwDummy_PowerGateUVD`/`VCE`
   (or CAIL's power on/off). Optionally UVD/VCE DPM (`SetUvdHardMin` 0x1C …, `UVD_DPM_MASK`, `VCE_DPM_MASK`). The SMU
   mailbox is ours alone (nvram200-203: no foreign messages).
2. **Firmware.**
   - VCE: same IP version as Polaris, so first try Apple's own Polaris VCE image; if it doesn't run, supply Linux's
     `stoney_vce.bin` (VCE 52.0.3).
   - UVD: supply Linux's `stoney_uvd.bin` (1.43.15). Apple's loader expects its encrypted Polaris image, so
     `loadUVDFW` / `getUVDFW` need a hook that hands over the plain Stoney image (Linux loads it directly into the VCPU
     cache, no PSP on VI).
   - AMD's firmware licence (linux-firmware `LICENSE.amdgpu`) allows redistribution in binary form with the notice.
     Keep binaries out of the repo: CI downloads them from linux-firmware and embeds them in the kext.
3. **Engine differences.**
   - UVD: no encode rings on Stoney, so `AMDUVD6v3HWEngine` must not create or start them, and HEVC *encode* must be
     reported as unsupported (Polaris does HEVC encode on UVD; `MM_EnableHEVCEncode` in the accelerator is the first
     thing to check).
   - VCE: one instance (VCE1 harvested); CAIL's `Stoney_check_vce_harvested` should report it.
4. **User space.** With the device-ID spoof, AppleGVA should pick the Polaris decoder/encoder paths. HEVC 10-bit
   decode isn't in Stoney's list: it has to be filtered, or fail cleanly.
5. **DRM.** FairPlay in the TV app and Safari relies on hardware decode on AMD Macs; with UVD working it may work as
   on Polaris iMacs (iMac19,1 SMBIOS), unverified.

## Order

Decode first (the bigger win: the CPU is two slow cores; 1080p H.264/HEVC playback and DRM need UVD), then encode
(H.264 only; screen recording, FaceTime, export). Each step is verified by counters in the black box (power-on
answers, firmware loaded, ring tests) before any playback test.

## Progress

- **Stage 1, implemented (opt-in `-crvideo`, untested):** before `AMDHardware::powerUpHWEngines`, the SMU gets
  `UVDPowerON` (static gating), `VCEPowerON` and `EnableAllSmuFeatures(UVD_DPM | VCE_DPM)`; Apple's power-up then
  includes UVD and VCE (SAMU stays skipped). Apple's `loadUVDFW`, UVD `start`/`startRBEngine`, VCE `loadFW`/`start`
  are traced (results in the trace summary), and once the SMU has answered, UVD/VCE status, VCPU control, soft reset,
  LMI status and ring pointers go into the disk log every second and into the report's `video:` line. Apple's
  power-gating calls are not hooked: the Dummy back end's `PhwDummy_PowerGateUVD`/`VCE` are 11-byte stubs packed back
  to back, too small for a jump; the engines simply stay powered.
- **Stage 1 result:** the SMU powers both engines (`UVDPowerON 1`, `VCEPowerON 1`), but Apple's `loadUVDFW`, UVD
  `start` and the VCE stages never ran: the engines start when a decode session opens, and none did.
- **Stage 2, implemented (`-crvideo`, in testing):** AppleGVA chooses the hardware decoder from the platform's
  board-id and expects the Intel iGPU's for iMac19,1. As WhateverGreen's `unfairgva=4` does, CarrizoRed renames the
  property AppleGVA reads (`board-id`, next to `hw.model` in the dyld shared cache) to `hwgva-id` and sets that on the
  device tree root to iMacPro1,1's board-id, an AMD-only Mac. The rest of the system keeps its SMBIOS. `-crnogva` leaves
  AppleGVA alone.
- **Stage 2 result (Sonoma):** the lock screen's wallpaper opened an AMD video context and closed it 2 ms later
  without any further call. User space said why (`log show`): `VTDecoderXPCService: (AppleGVA) [HEVC] GVA ERROR:
  IOGVACodec not set`.
- **Stage 3, implemented (`-crvideo`, in testing):** AppleGVA's lookups only accept a GPU whose registry name is
  `GFX0`, `GFXA`, `IGPU`, `IOPP` or `display`, and the iGPU had its ACPI name `VGA`: it is renamed `GFX0` (as
  WhateverGreen does for AMD GPUs). And `AMDGraphicsAccelerator::configureDevice` publishes `IOGVAHEVCDecode` only when
  the PCI device's `MM_EnableHEVCDecode` and `MM_EnableHEVCEncode` are both true, which AMD9500Controller's
  `aty_properties` set false for every Polaris: they are forced true for that call, and HEVC encode is withdrawn again
  afterwards (Stoney's UVD has no encode rings). The report's `hevc` bits: 1 flags forced, 2 decode published,
  4 encode withdrawn, 8 renamed.
- **Stage 3 result (Sonoma):** `hevc f` (renamed, decode published, encode withdrawn). AppleGVA now picks the AMD
  decoder and opens a context per attempt (four in a boot), but the lock screen's aerial wallpaper is **HEVC Main10,
  3840x2160, 10-bit**, and every attempt ends in `AVD_api.Create failed err=5` / `Error creating the physical
  accelerator`, then software decode. Stoney's UVD 6.2 decodes 8-bit HEVC only (Linux `cz_video_codecs_decode`), so
  the aerial wallpapers stay on the CPU whatever the driver does; a still wallpaper is the fix for that lag. Whether
  UVD itself works has to be shown with an 8-bit stream (H.264 or 8-bit HEVC).
- **Next, from the stage 3 log:** whether Apple's UVD 6.3 / VCE 3.4 firmware starts on Stoney (UVD_STATUS, VCPU
  running, ring test). If not, stage 2 replaces the firmware in the engine's buffer (`AMDUVDHWEngine::loadUVDFW` asks
  the shared multimedia layer for the image, `this+0x160`, then decrypts it when a flag says so) with Linux's
  `stoney_uvd.bin` / `stoney_vce.bin`, fetched by CI rather than committed.
