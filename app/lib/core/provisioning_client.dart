import 'package:http/http.dart' as http;

import 'json_http.dart';
import 'qr_payload.dart';
import 'validators.dart';

class WifiNetwork {
  const WifiNetwork({required this.ssid, required this.rssi, required this.secure});

  final String ssid;
  final int rssi;
  final bool secure;
}

enum ProvisionState { idle, connecting, connected, failed }

class ProvisionResult {
  const ProvisionResult({required this.state, this.ip, this.port, this.token, this.reason});

  final ProvisionState state;
  final String? ip;
  final int? port;
  final String? token;

  /// `auth_failed`, `no_ap`, `timeout` or another short code from the robot.
  final String? reason;
}

/// Talks to the robot's pairing endpoints over its SoftAP (docs/robot_api.md section 1). Every
/// request carries the pairing secret from the QR code.
class ProvisioningClient {
  ProvisioningClient(this.target, {http.Client? client, this.allowLoopback = false})
      : _http = JsonHttp(host: target.host, port: target.port, client: client, timeout: const Duration(seconds: 6));

  final PairingTarget target;
  final bool allowLoopback;
  final JsonHttp _http;

  Map<String, String> get _headers => {'X-Pair-Secret': target.payload.pairingSecret};

  /// Confirms we are talking to the robot from the QR code.
  Future<void> verifyDevice() async {
    final j = await _http.get('/prov/info', headers: _headers);
    if (j.str('id') != target.payload.deviceId) {
      throw const ApiException(ApiErrorKind.badResponse, 'device id mismatch');
    }
  }

  Future<List<WifiNetwork>> scan() async {
    // A scan blocks the robot's radio for a few seconds.
    final j = await _http.get('/prov/scan', headers: _headers, timeout: const Duration(seconds: 15));
    final seen = <String>{};
    final out = <WifiNetwork>[];
    for (final e in j.list('networks').whereType<Map<String, dynamic>>().take(40)) {
      final ssid = e.str('ssid');
      if (!Validators.ssid(ssid) || !seen.add(ssid!)) continue;
      out.add(WifiNetwork(ssid: ssid, rssi: (e.integer('rssi') ?? -100).clamp(-100, 0), secure: e.boolean('secure') ?? true));
    }
    out.sort((a, b) => b.rssi.compareTo(a.rssi));
    return out;
  }

  /// Sends home Wi-Fi credentials in the POST body (never the URL). The password is not kept.
  Future<void> sendWifi(String ssid, String password) async {
    if (!Validators.ssid(ssid) || !Validators.homeWifiPassword(password)) {
      throw const ApiException(ApiErrorKind.badRequest, 'invalid ssid or password');
    }
    await _http.post('/prov/wifi', headers: _headers, form: {'ssid': ssid, 'password': password});
  }

  Future<ProvisionResult> result() async {
    final j = await _http.get('/prov/result', headers: _headers);
    final s = j.str('state');
    final state = ProvisionState.values.firstWhere((e) => e.name == s, orElse: () => ProvisionState.idle);
    if (state == ProvisionState.connected) {
      final ip = j.str('ip'), token = j.str('token');
      final port = j.integer('port') ?? 80;
      if (!Validators.lanHost(ip, allowLoopback: allowLoopback) || !Validators.deviceToken(token) || !Validators.port(port)) {
        throw const ApiException(ApiErrorKind.badResponse, 'bad pairing result');
      }
      return ProvisionResult(state: state, ip: ip, port: port, token: token);
    }
    final reason = j.str('reason');
    return ProvisionResult(state: state, reason: reason != null && reason.length <= 32 ? reason : null);
  }

  /// Tells the robot to persist everything and drop its hotspot.
  Future<void> finish() => _http.post('/prov/finish', headers: _headers);

  void close() => _http.close();
}
