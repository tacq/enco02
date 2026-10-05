# ENCO-02 companion app

A Flutter app for iOS and Android: set up a new robot (QR code → robot hotspot → home Wi-Fi), then control it, update its firmware, and (in the developer build) tune its motors.

There is no backend. The phone talks to the robot directly on the same Wi-Fi, and firmware updates are published as GitHub Releases.

| Doc | What |
|---|---|
| [docs/robot_api.md](docs/robot_api.md) | The app ⇄ robot protocol: pairing, discovery, control, admin, OTA. Marks which endpoints exist in the firmware today and which are **new** |
| [docs/firmware_updates.md](docs/firmware_updates.md) | Update design: manifest, app-initiated vs robot-initiated (voice) approval, and the flash-layout work it needs |

## User flow

1. **Add robot**. Scan the QR code shown on the robot's screen (`enco://pair?...`).
2. The app joins the robot's WPA2 setup hotspot. The password comes from the QR code.
3. **Choose Wi-Fi**. The list is the robot's own scan (2.4 GHz only). Enter the password.
4. The robot joins your Wi-Fi while its hotspot stays up. It reports its LAN IP plus a device token, then drops the hotspot.
5. Name the robot, and you're on the **Control** page: face, expressions, head D-pad and animations, tracking, captions, screen mode, volume, and "talk to the robot".
6. **Settings**: rename, firmware update, change Wi-Fi, remove robot, language (System / English / 简体中文).

## Two builds, one codebase

| | entrypoint | Android | iOS |
|---|---|---|---|
| User | `lib/main_user.dart` | `--flavor user` (`com.enco.enco_app`) | same bundle id |
| Developer | `lib/main_dev.dart` | `--flavor dev` (`com.enco.enco_app.dev`, installs side by side) | same bundle id. TODO: add Xcode schemes for a separate `.dev` bundle |

The dev build adds a **Developer** tab with:
- live servo sliders
- per-axis limits and center (apply / save to NVS)
- axis speeds and test moves
- tracking-direction flips
- diagnostics and reboot
- a request console restricted to `/api/*`

It also adds a **paste setup code / mock robot** shortcut on the scan screen.

Every developer call also sends `X-Admin-Key`, and the robot checks it server-side. The key is `APP_ADMIN_KEY` in the gitignored `home_config.h`. It is entered once in the Developer tab and kept in the Keychain/Keystore. A user build never reads or sends it.

## Setup

```sh
cd app
# The platform folders are generated, not committed by hand:
flutter create --org com.enco --project-name enco_app --platforms ios,android .
python3 tool/setup_platforms.py      # permissions, flavors, entitlements (idempotent)
flutter pub get
flutter gen-l10n
flutter analyze
tool/run_tests.sh                    # unit tests on the plain Dart VM (or: flutter test)
```

> [!NOTE]
> On Santa-managed Macs, `dartaotruntime` may be blocked. That breaks `flutter test`, `dart run`, `dart test` and every app build, because Flutter's compiler only ships as an AOT snapshot. `flutter analyze`, `tool/run_tests.sh` and `dart tool/mock_robot.dart` still work: they run on the plain JIT VM. Building and running the app needs the binary approved.

Run:

```sh
flutter run --flavor user -t lib/main_user.dart     # Android
flutter run --flavor dev  -t lib/main_dev.dart      # Android, developer build
flutter run -t lib/main_dev.dart                    # iOS (simulator or device)
```

### Without a robot: mock robot

```sh
dart tool/mock_robot.dart            # 127.0.0.1:8080, prints a setup code (+ an admin key)
```

In the **dev** build, go to Add robot → paste icon, paste the `enco://` code, and keep "Mock robot" ticked. The iOS simulator reaches `127.0.0.1` directly. For an Android emulator, run `adb reverse tcp:8080 tcp:8080` first.

The mock simulates:
- pairing failures: password `wrongpass` → wrong password; SSID `Neighbour` → network not found
- a firmware update from 1.0.0 to 1.1.0, with progress

## Layout

```
lib/
  app.dart, app_config.dart, main_user.dart, main_dev.dart
  core/     json_http, robot_api, provisioning_client, hotspot, discovery,
            firmware (manifest + trusted-host redirects), robot_store, validators, qr_payload
  models/   saved_robot, robot_models
  state/    app_state (robots, locale, admin key), robot_session (connect/discover/poll)
  ui/       home_page, onboarding/ (scan → join → wifi → connecting → name),
            robot/ (control, developer, settings, firmware)
  l10n/     app_en.arb, app_zh.arb
tool/       mock_robot.dart, setup_platforms.py
test/       qr/validators, api + provisioning clients, firmware manifest
```

## Security notes

- **Pairing needs physical presence.** The QR code carries a per-session hotspot password and a pairing secret. A per-pairing bearer token is issued only after the robot reaches your Wi-Fi.
- **Secrets stay in secure storage.** Tokens and the admin key are kept in `flutter_secure_storage` (Keychain / Keystore). They are never logged, and never put in URLs (only headers and POST bodies).
- **All input is allow-listed.** QR fields, robot responses, UDP replies and the manifest are all validated. The robot address must be a private IPv4 address, so a spoofed reply can't send the token to the internet.
- **Firmware downloads are HTTPS-only, from GitHub hosts only.** Every redirect hop is checked, and the robot re-verifies the sha256.
- `TODO(security)`: LAN traffic is plain HTTP, because there is no TLS budget on the no-PSRAM ESP32. Signed firmware is not done yet; see `docs/firmware_updates.md`.
