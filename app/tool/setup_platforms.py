#!/usr/bin/env python3
"""Patches the generated android/ and ios/ projects with what the ENCO app needs.

Run once from app/ after generating the platform folders:

    flutter create --org com.enco --project-name enco_app --platforms ios,android .
    python3 tool/setup_platforms.py

Idempotent: re-running it changes nothing.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

if not (ROOT / "android").is_dir() or not (ROOT / "ios").is_dir():
    sys.exit("android/ and ios/ not found - run `flutter create --org com.enco --project-name enco_app "
             "--platforms ios,android .` in app/ first.")


def patch(path: pathlib.Path, marker: str, fn):
    text = path.read_text()
    if marker in text:
        print(f"  ok      {path.relative_to(ROOT)}")
        return
    new = fn(text)
    if new == text:
        sys.exit(f"could not patch {path} - layout changed? Apply by hand (see README).")
    path.write_text(new)
    print(f"  patched {path.relative_to(ROOT)}")


# ---------------------------------------------------------------- Android
manifest = ROOT / "android/app/src/main/AndroidManifest.xml"
PERMS = """
    <!-- ENCO: camera for the QR code, Wi-Fi to join the robot's setup hotspot, LAN for control. -->
    <uses-permission android:name="android.permission.INTERNET" />
    <uses-permission android:name="android.permission.CAMERA" />
    <uses-permission android:name="android.permission.ACCESS_WIFI_STATE" />
    <uses-permission android:name="android.permission.CHANGE_WIFI_STATE" />
    <uses-permission android:name="android.permission.ACCESS_NETWORK_STATE" />
    <uses-permission android:name="android.permission.CHANGE_NETWORK_STATE" />
    <uses-permission android:name="android.permission.ACCESS_FINE_LOCATION" android:maxSdkVersion="32" />
    <uses-permission android:name="android.permission.NEARBY_WIFI_DEVICES" android:usesPermissionFlags="neverForLocation" />
"""
patch(manifest, "ENCO: camera", lambda t: re.sub(r"(<manifest[^>]*>)", r"\1" + PERMS, t, count=1))
patch(
    manifest,
    "networkSecurityConfig",
    lambda t: t.replace("<application", '<application\n        android:networkSecurityConfig="@xml/network_security_config"', 1),
)

nsc = ROOT / "android/app/src/main/res/xml/network_security_config.xml"
nsc.parent.mkdir(parents=True, exist_ok=True)
if not nsc.exists():
    nsc.write_text(
        """<?xml version="1.0" encoding="utf-8"?>
<!--
  The robot API is plain HTTP on the LAN / setup hotspot at addresses only known at runtime, so
  cleartext must be allowed. TODO(security): the app itself restricts HTTP to private IPv4 hosts
  (Validators.lanHost) and authenticates with a per-pairing token; everything on the internet
  (firmware manifest) is HTTPS-only in code.
-->
<network-security-config>
    <base-config cleartextTrafficPermitted="true">
        <trust-anchors>
            <certificates src="system" />
        </trust-anchors>
    </base-config>
</network-security-config>
"""
    )
    print(f"  created {nsc.relative_to(ROOT)}")

gradle = ROOT / "android/app/build.gradle.kts"
FLAVORS = """
    // ENCO flavors: `user` = store app, `dev` = developer/admin app (installs side by side).
    flavorDimensions += "app"
    productFlavors {
        create("user") {
            dimension = "app"
            resValue("string", "app_name", "ENCO-02")
        }
        create("dev") {
            dimension = "app"
            applicationIdSuffix = ".dev"
            resValue("string", "app_name", "ENCO-02 Dev")
        }
    }
"""
patch(gradle, "ENCO flavors", lambda t: re.sub(r"(\nandroid \{\n)", r"\1" + FLAVORS, t, count=1))
patch(manifest, "@string/app_name", lambda t: re.sub(r'android:label="[^"]*"', 'android:label="@string/app_name"', t, count=1))

# -------------------------------------------------------------------- iOS
plist = ROOT / "ios/Runner/Info.plist"
PLIST_KEYS = """	<key>NSCameraUsageDescription</key>
	<string>Scan the QR code on your ENCO-02's screen to set it up. / 扫描 ENCO-02 屏幕上的二维码进行设置。</string>
	<key>NSLocalNetworkUsageDescription</key>
	<string>Find and control your ENCO-02 on your Wi-Fi. / 在你的 Wi-Fi 中查找并控制 ENCO-02。</string>
	<key>NSAppTransportSecurity</key>
	<dict>
		<!-- Plain HTTP only to local-network hosts (the robot). Internet traffic stays HTTPS. -->
		<key>NSAllowsLocalNetworking</key>
		<true/>
	</dict>
"""
patch(plist, "NSLocalNetworkUsageDescription", lambda t: t.replace("<dict>\n", "<dict>\n" + PLIST_KEYS, 1))

ent = ROOT / "ios/Runner/Runner.entitlements"
if not ent.exists():
    ent.write_text(
        """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<!-- Join the robot's setup hotspot (NEHotspotConfiguration). -->
	<key>com.apple.developer.networking.HotspotConfiguration</key>
	<true/>
	<!-- LAN discovery broadcast. Must be requested from Apple before release; remove it for
	     personal-team builds if signing fails. -->
	<key>com.apple.developer.networking.multicast</key>
	<true/>
</dict>
</plist>
"""
    )
    print(f"  created {ent.relative_to(ROOT)}")

pbx = ROOT / "ios/Runner.xcodeproj/project.pbxproj"
patch(
    pbx,
    "CODE_SIGN_ENTITLEMENTS",
    lambda t: re.sub(
        r"(\n(\t+)PRODUCT_BUNDLE_IDENTIFIER = com\.enco\.encoApp;)",
        r"\n\2CODE_SIGN_ENTITLEMENTS = Runner/Runner.entitlements;\1",
        t,
    ),
)

# ------------------------------------------------------------------ misc
default_test = ROOT / "test/widget_test.dart"
if default_test.exists() and "MyApp" in default_test.read_text():
    default_test.unlink()
    print("  removed test/widget_test.dart (template counter test)")

print("done")
