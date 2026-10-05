import 'validators.dart';

/// What the robot's screen shows in pairing mode:
///   `enco://pair?v=1&id=ENCO-7F3A&ap=ENCO-7F3A&pw=<ap password>&s=<pairing secret>`
/// See docs/robot_api.md section 1.
class QrPayload {
  const QrPayload({
    required this.deviceId,
    required this.apSsid,
    required this.apPassword,
    required this.pairingSecret,
  });

  final String deviceId;
  final String apSsid;
  final String apPassword;
  final String pairingSecret;

  static const int protocolVersion = 1;

  /// Returns null for anything that is not a valid v1 ENCO pairing code.
  static QrPayload? tryParse(String? raw) {
    if (raw == null || raw.length > 512) return null;
    final uri = Uri.tryParse(raw.trim());
    if (uri == null || uri.scheme != 'enco' || uri.host != 'pair') return null;
    final q = uri.queryParameters;
    if (q['v'] != '$protocolVersion') return null;
    final id = q['id'];
    final ap = q['ap'];
    final pw = q['pw'];
    final s = q['s'];
    if (!Validators.deviceId(id) ||
        !Validators.ssid(ap) ||
        !Validators.wpaPassword(pw) ||
        !Validators.pairingSecret(s)) {
      return null;
    }
    return QrPayload(deviceId: id!, apSsid: ap!, apPassword: pw!, pairingSecret: s!);
  }

  String toUri() => Uri(
        scheme: 'enco',
        host: 'pair',
        queryParameters: {'v': '$protocolVersion', 'id': deviceId, 'ap': apSsid, 'pw': apPassword, 's': pairingSecret},
      ).toString();
}

/// Where the pairing endpoints live. Normally the robot's SoftAP; in the dev flavor it can be the
/// local mock robot, in which case joining a hotspot is skipped.
class PairingTarget {
  const PairingTarget({required this.payload, required this.host, required this.port, required this.joinHotspot});

  final QrPayload payload;
  final String host;
  final int port;
  final bool joinHotspot;
}
