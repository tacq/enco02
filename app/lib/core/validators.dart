import 'dart:convert';
import 'dart:io';

/// Allow-list validators for everything that arrives from outside the app: QR codes, robot
/// responses, UDP replies, the firmware manifest. Anything that fails is rejected, not repaired.
class Validators {
  Validators._();

  static final RegExp _deviceId = RegExp(r'^[A-Za-z0-9-]{4,32}$');
  static final RegExp _base64Url = RegExp(r'^[A-Za-z0-9_-]+$');
  static final RegExp _printableAscii = RegExp(r'^[\x20-\x7E]+$');
  static final RegExp _sha256 = RegExp(r'^[0-9a-f]{64}$');
  static final RegExp _version = RegExp(r'^\d{1,4}\.\d{1,4}\.\d{1,4}$');
  static final RegExp _name = RegExp(r'^[A-Za-z0-9_-]{1,32}$');

  static bool deviceId(String? v) => v != null && _deviceId.hasMatch(v);

  /// SSIDs are 1-32 bytes, UTF-8. No control characters.
  static bool ssid(String? v) {
    if (v == null || v.isEmpty) return false;
    final bytes = utf8.encode(v).length;
    return bytes <= 32 && !v.codeUnits.any((c) => c < 0x20 || c == 0x7F);
  }

  /// WPA2-PSK passphrase: 8-63 printable ASCII.
  static bool wpaPassword(String? v) =>
      v != null && v.length >= 8 && v.length <= 63 && _printableAscii.hasMatch(v);

  /// Home Wi-Fi password the user types: empty (open network) or WPA rules.
  static bool homeWifiPassword(String v) => v.isEmpty || wpaPassword(v);

  static bool pairingSecret(String? v) =>
      v != null && v.length >= 16 && v.length <= 64 && _base64Url.hasMatch(v);

  static bool deviceToken(String? v) =>
      v != null && v.length >= 16 && v.length <= 128 && _base64Url.hasMatch(v);

  static bool sha256Hex(String? v) => v != null && _sha256.hasMatch(v);

  static bool version(String? v) => v != null && _version.hasMatch(v);

  /// Animation, character and expression identifiers.
  static bool identifier(String? v) => v != null && _name.hasMatch(v);

  /// Friendly robot name: 1-24 UTF-8 bytes, no control characters.
  static bool robotName(String v) {
    final t = v.trim();
    if (t.isEmpty) return false;
    return utf8.encode(t).length <= 24 && !t.codeUnits.any((c) => c < 0x20 || c == 0x7F);
  }

  static bool port(int? v) => v != null && v > 0 && v < 65536;

  /// The robot must live on the local network. A robot (or spoofed UDP reply) must not be able to
  /// point the app - and its bearer token - at an internet host. Loopback only for the dev mock.
  static bool lanHost(String? v, {bool allowLoopback = false}) {
    if (v == null) return false;
    final addr = InternetAddress.tryParse(v);
    if (addr == null || addr.type != InternetAddressType.IPv4) return false;
    final b = addr.rawAddress;
    if (b[0] == 10) return true;
    if (b[0] == 172 && b[1] >= 16 && b[1] <= 31) return true;
    if (b[0] == 192 && b[1] == 168) return true;
    if (b[0] == 169 && b[1] == 254) return true;
    if (allowLoopback && b[0] == 127) return true;
    return false;
  }
}
