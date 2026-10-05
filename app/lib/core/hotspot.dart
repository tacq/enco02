import 'dart:io';

import 'package:wifi_iot/wifi_iot.dart';

/// Joins / leaves the robot's pairing hotspot.
///
/// Android 10+: WifiNetworkSpecifier (system dialog, no internet on that network), then
/// forceWifiUsage so our sockets go over it instead of mobile data.
/// iOS 11+: NEHotspotConfiguration ("Join network?" prompt). Needs the Hotspot Configuration
/// entitlement (ios/Runner/Runner.entitlements).
class Hotspot {
  String? _joined;

  Future<bool> join(String ssid, String password) async {
    final ok = await WiFiForIoTPlugin.connect(
      ssid,
      password: password,
      security: NetworkSecurity.WPA,
      joinOnce: true,
      withInternet: false,
      timeoutInSeconds: 30,
    );
    if (!ok) return false;
    _joined = ssid;
    if (Platform.isAndroid) {
      await WiFiForIoTPlugin.forceWifiUsage(true);
    }
    return true;
  }

  /// Gives the phone back to its normal network. Safe to call more than once.
  Future<void> release() async {
    final ssid = _joined;
    _joined = null;
    if (ssid == null) return;
    if (Platform.isAndroid) {
      await WiFiForIoTPlugin.forceWifiUsage(false);
      await WiFiForIoTPlugin.disconnect();
    } else {
      await WiFiForIoTPlugin.removeWifiNetwork(ssid);
    }
  }
}
