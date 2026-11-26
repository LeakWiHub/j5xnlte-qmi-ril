# j5xnlte-qmi-ril — Radio AIDL HAL (V1) over `/dev/wwan0qmi0`

Samsung Galaxy J5 2016 (j5xnlte, MSM8916, arm64), vendor API 34 (Android 14 vendor), running an Android 15 GSI.
The vendor image sits on the `cache` partition.
Russian version (with the full device-run history): [README_RU.md](README_RU.md).

## Status in one line

The HAL is built with the Android NDK in GitHub Actions and **runs on a real phone** (Android 15 GSI): LTE registration, the SIM in the settings menu,
voice calls with audio, microphone mute, SMS (both directions), USSD and mobile data were verified on the device (2026-11-26).
It is **not** a finished production modem: earpiece/speaker switching from the Android UI is not done, and the stock `rild` still runs next to this HAL.
Per-item status: **implemented / built / verified on device / not verified**.

## Layers

| Layer | Where | What it does |
|---|---|---|
| QMUX/TLV codec | `qmi/src/qmi_codec.c` | frames, CTL (u8 txid) and service (u16 txid) SDUs, TLVs, stream deframer |
| Transport | `qmi/src/qmi_dev.c` | `/dev/wwan0qmi0`, reader thread, pending-transaction table, timeouts, retries, indications, reconnect. No QRTR |
| Modem API | `ril/core/*.c` | CTL, DMS, UIM, NAS, WMS, VOICE (+USSD), WDS, WDA; event queue, worker thread |
| Radio HAL (**V1**) | `hal/src/*`, `hal/gen/*` | IRadioConfig, Modem, Sim, Network, Messaging, Voice, Data; 4-thread executor, USSD on its own thread |
| Call audio | `hal/src/voice_audio.cc` | in-HAL: CS-Voice ports, MSM8916 analog path, PCM open, mic mute (from Audio HAL log), volume (from `STREAM_VOICE_CALL`); raw ALSA ioctl mixer, no tinymix |
| Integration | `Android.bp`, `android/*` | cc_binary, init rc, VINTF, ueventd, telephony features |
| Recovery installer | `android/recovery/`, `scripts/make-recovery-zip.sh` | Lineage Recovery zip: install into `/vendor` + init autostart, plus an uninstall zip |
| adb installer | `scripts/install.sh`, `scripts/device/*` | step by step, with rollback |
| Tests | `tests/`, `scripts/run-all-tests.sh` | see below |

**Which AIDL version:** the vendor API is 34 (FCM level 7), whose matrix wants `android.hardware.radio.*` **V1**, so the default build is V1
(`AIDL_VER=1`, VINTF `<version>1</version>`). `AIDL_VER=2..4` builds the same sources against newer frozen snapshots (not exercised on a device).
V1 lacks `getImei` and `PhoneCapability.maxActiveVoice`; the HAL compiles those out (`hal/src/aidl_version.h`).

**Deliberately not implemented** (return `REQUEST_NOT_SUPPORTED`, never a fake success): `iccIo` writes, `getSmscAddress`, `enableModem`,
conference/call switching, CLIR, logical channels/STK, cell info, V4 satellite methods, VoLTE/IMS, more than one data call, IPv6, emergency calls without SIM.

## Readiness table

"Built" = built by Android NDK in GitHub Actions. An AOSP-tree build (`Android.bp`, rc as part of an image) has **not** been done.
"On device" = verified on the phone by the requester.

| Item | Implemented | Built | On device |
|---|---|---|---|
| Transport opens `/dev/wwan0qmi0`, CTL, DMS | yes | yes | **yes** |
| UIM: SIM detected | yes | yes | **yes** |
| NAS: LTE registration | yes | yes | **yes** (IN_SERVICE, LTE) |
| HAL registers, Android uses it | yes (7 services) | yes | **yes** (init service + VINTF fragment in `/vendor`) |
| SIM in Android settings screen | yes (CardStatus with ICCID: DMS, fallback EF_ICCID) | yes | **yes** (needed the ICCID, otherwise no subscription: `subId=-1`) |
| SMS | send, receive, ACK | yes | **yes**, incoming and outgoing (the modem delivers a bare TPDU; the HAL prepends SMSC length `0x00`) |
| Calls | yes | yes | **yes**: signalling and audio, microphone mute. DTMF, call waiting, conference not checked. Call volume and earpiece/speaker switching not confirmed |
| Mobile data | yes (WDA raw IP, SIO bind, IPv4 on `wwan0`) | yes | **yes** (traffic flows) |
| USSD (`*100#` etc.) | yes (Originate/Answer/Cancel, indications) | yes | **yes** |
| Autostart | init rc in the recovery zip (+ telephony features, VINTF) | yes | **yes** (init starts the service, telephony comes up) |
| Earpiece/speaker switch from Android UI | no | — | **does not work**: audio policy has only Speaker |

## Findings from the device

- **SMS:** an incoming message arrives as a TPDU without SMSC. Android expects the RIL format: SMSC length, SMSC, TPDU. The HAL prepends `0x00`.
- **Data, BAM-DMUX without QMAP:** `wwanN` is raw IP. WDA Set Data Format sets raw IP. The client is bound with **WDS Bind Data Port 0x0089** using the SIO port
  `A2_MUX_RMNET0 + N`, which is `0x0E04` for `wwan0`; Bind Mux Data Port 0x00A2 is rejected by the modem. `wwan0` is brought up **before** Start Network, because the driver sends the channel OPEN to the modem.
  **The RIL must set the IPv4 address itself**: Android adds only MTU, routes and DNS for cellular. Without it no packets left the device.
  Kernel counters of BAM-DMUX are always 0, so use the modem counters from WDS 0x24. The HAL logs a `data diag` line 20 s and 60 s after the call comes up.
- **Voice, q6voice:** the DSP session starts once both `pcmC0D4p/c` are open. Ports are chosen with `PRI_MI2S_RX Voice Mixer CS-Voice` and `CS-Voice Capture Mixer TERT_MI2S_TX`.
  Toggling `SPK DAC Switch` distorts the speaker amp, so routing uses the digital `RX1/RX3 Mute Switch` and never touches `SPK DAC` during a call.
- **Mic mute** is not sent to RIL `setMute`, it goes through `AudioManager`. It shows up in the stock Audio HAL log as `AHAL_Module: setMicMute: default: 0|1`, which the HAL reads from logcat.
- **Call volume:** the `STREAM_VOICE_CALL` index from `dumpsys audio` is mapped to `RX1/2/3 Digital Volume`. The formula is empirical: 60 + 6·index−1, max 100.
- **USSD:** Originate 0x3A, Answer 0x3B, Cancel 0x3C, indications 0x3D and 0x3E, ASCII and UCS2.
- **Telephony features:** a fresh GSI declares no `android.hardware.telephony*`. Without them `com.android.phone` never talks to the RIL, and there is no `RILJ` log at all.
  The recovery zip installs `/vendor/etc/permissions/android.hardware.telephony.j5xnlte.xml`.
- **ICCID:** Android 15 creates no subscription, `subId=-1` and "no valid subs", when `CardStatus.iccid` is empty. That shows as an empty SIM menu and "mobile network unavailable" even while registered.
  The HAL reads DMS 0x3C, then EF_ICCID `2FE2` through UIM.
- **Vendor on `cache`:** the recovery installer looks for the vendor tree on `vendor`, then on `cache` by-name, and accepts a candidate only if it has `bin/hw` and `etc/init`.

## Known limitations

- Codec control names and ports are hard-coded for the J5 2016 (taken from `tinymix` on this device).
- Mic mute (logcat parsing) and volume (`dumpsys` parsing) are fragile and depend on the Android output formats.
- One data call, IPv4 only.
- The installers never disable the stock `rild`. On the Android 15 setup `vendor.ril-daemon` runs next to this HAL; Android uses this HAL anyway, but whether the
  stock rild interferes was **not investigated**. Do not intentionally run two RILs against one modem.
- Android 15 reports `REQUEST_NOT_SUPPORTED` (error 6) for several optional requests (e.g. `SEND_DEVICE_STATE`, `ENABLE_VONR`, `SET_DATA_PROFILE`); this is by design (not implemented, never a fake success).

## Installation

The ready zips are attached to the **Releases** page of the repository:

- `j5xnlte-ril-install.zip`: installs the HAL with autostart;
- `j5xnlte-ril-uninstall.zip`: removes everything it installed and restores the backed up files.

Install from Lineage Recovery: Apply update → Apply from ADB, then on the PC:
```
adb sideload j5xnlte-ril-install.zip
```
The zip is unsigned, recovery asks to continue. It looks for the vendor tree on `vendor`, then on `cache`, and writes into `/vendor`: the HAL,
`j5x-ril-start.sh` that loads the modem modules and restarts the HAL up to 20 times, `j5xnlte-ril.rc` as a root init service,
the VINTF fragment `android.hardware.radio.j5xnlte-qmi.xml` and the telephony feature file. Pre-existing files are backed up to `.j5ril-backup`.
The stock `rild` is not disabled.

Remove: `adb sideload j5xnlte-ril-uninstall.zip`. Kill switch without recovery: `setprop persist.vendor.j5ril.enable 0` and reboot.
Install is **verified on the device**: `cache` mounted as vendor, files installed, init service running. Uninstall and the kill switch were not exercised on the device.

Building the zips yourself: GitHub Actions builds the HAL with the Android NDK, then
`bash scripts/make-recovery-zip.sh <HAL binary> build/`. Locally the HAL is built with `scripts/build-hal-ndk.sh <NDK> <aidl> <aidl_api> <out>`.

## Layout

```
Android.bp                       cc_binary + static QMI/RIL library + prebuilt telephony features
android/                         init rc, recovery/ (update-binary, wrapper, rc), vintf, permissions, ueventd
qmi/                             codec, transport, log (plain C, no Android)
ril/core/                        synchronous modem API (ril_qmi.h) and QMI services
hal/src, hal/gen                 HAL implementation (+ voice_audio.cc); base classes from frozen AIDL V4
tools/                           gen-hal-base, qmi-probe, ril-cli, voice, diag
scripts/                         run-all-tests.sh, build-hal-ndk.sh, make-recovery-zip.sh, install.sh, device/*
tests/                           codec, transport, glue, HAL, install, recovery; fake modems in Python
docs/                            AUDIT_RU.md, device-logs/
DEPENDENCIES.md, LICENSE, NOTICE dependencies and licenses
```

## CI

GitHub Actions builds the HAL with the Android NDK and publishes the artifact `j5xnlte-radio-hal-ndk`.
