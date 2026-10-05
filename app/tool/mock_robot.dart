// Mock ENCO-02 robot for developing the app without hardware.
//
//   dart run tool/mock_robot.dart
//
// Serves the pairing (/prov/*) and robot (/api/*) endpoints from docs/robot_api.md on
// 127.0.0.1:8080 (loopback only). The iOS simulator reaches it directly; for an Android emulator
// run `adb reverse tcp:8080 tcp:8080`. In the dev app: Add robot -> paste icon -> paste the printed
// enco:// code with "Mock robot" ticked.
//
// The admin key is MOCK_ADMIN_KEY from the environment, or a random one printed at start-up.

// ignore_for_file: avoid_print

import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math';

final _rng = Random.secure();

String _randomToken(int bytes) =>
    base64Url.encode(List<int>.generate(bytes, (_) => _rng.nextInt(256))).replaceAll('=', '');

String _randomPassword(int len) {
  const chars = 'abcdefghijkmnpqrstuvwxyzACDEFGHJKLMNPQRTUVWXY34679';
  return List.generate(len, (_) => chars[_rng.nextInt(chars.length)]).join();
}

class MockRobot {
  final String id = 'ENCO-MOCK';
  String name = 'ENCO-02';
  final String apPassword = _randomPassword(12);
  final String pairSecret = _randomToken(18);
  final String adminKey = Platform.environment['MOCK_ADMIN_KEY'] ?? _randomToken(12);

  String? token;
  String provState = 'idle';
  String? provReason;
  DateTime? provStarted;

  final List<double> angles = [90, 90, 70];
  final List<Map<String, num>> limits = [
    {'axis': 0, 'pin': 0, 'min': 40, 'max': 120, 'center': 90},
    {'axis': 1, 'pin': 25, 'min': 50, 'max': 110, 'center': 90},
    {'axis': 2, 'pin': 26, 'min': 20, 'max': 120, 'center': 70},
  ];
  final List<double> speeds = [40, 40, 40];
  int volume = 60;
  bool tracking = false, caption = true, headAuto = true;
  String uiMode = 'face', character = 'k3';
  final DateTime boot = DateTime.now();

  // OTA simulation
  String fwMain = '1.0.0', fwCam = '1.0.0';
  String otaState = 'idle';
  String? otaTarget;
  int otaProgress = 0;
  Timer? otaTimer;

  String get pairingUri => Uri(scheme: 'enco', host: 'pair', queryParameters: {
        'v': '1', 'id': id, 'ap': id, 'pw': apPassword, 's': pairSecret, //
      }).toString();

  Future<void> handle(HttpRequest req) async {
    final res = req.response;
    final path = req.uri.path;
    final params = <String, String>{...req.uri.queryParameters};
    if (req.method == 'POST') {
      final body = await utf8.decoder.bind(req).join();
      if (body.length > 4096) return _send(res, 413, {'ok': false, 'error': 'too large'});
      if (body.isNotEmpty) params.addAll(Uri.splitQueryString(body));
    } else if (req.method != 'GET') {
      return _send(res, 405, {'ok': false, 'error': 'method'});
    }
    print('${req.method} $path ${params.keys.toList()}'); // values may be secrets - never printed

    if (path.startsWith('/prov/')) return _prov(req, res, path, params);
    if (path.startsWith('/api/')) return _api(req, res, path, params);
    return _send(res, 404, {'ok': false, 'error': 'not found'});
  }

  void _prov(HttpRequest req, HttpResponse res, String path, Map<String, String> p) {
    if (req.headers.value('x-pair-secret') != pairSecret) return _send(res, 401, {'ok': false, 'error': 'bad secret'});
    switch (path) {
      case '/prov/info':
        return _send(res, 200, {'id': id, 'name': name, 'proto': 1, 'fw': {'main': fwMain, 'cam': fwCam}});
      case '/prov/scan':
        return _send(res, 200, {
          'networks': [
            {'ssid': 'HomeNet', 'rssi': -48, 'secure': true},
            {'ssid': 'HomeNet-Guest', 'rssi': -63, 'secure': true},
            {'ssid': 'Neighbour', 'rssi': -81, 'secure': true},
            {'ssid': 'CoffeeShop', 'rssi': -85, 'secure': false},
          ]
        });
      case '/prov/wifi':
        final ssid = p['ssid'] ?? '', pw = p['password'] ?? '';
        provStarted = DateTime.now();
        provState = 'connecting';
        // Simulated outcomes: password "wrongpass" fails auth, SSID "Neighbour" is out of range.
        provReason = pw == 'wrongpass' ? 'auth_failed' : (ssid == 'Neighbour' ? 'no_ap' : null);
        return _send(res, 202, {'ok': true});
      case '/prov/result':
        if (provState == 'connecting' && DateTime.now().difference(provStarted!) > const Duration(seconds: 3)) {
          if (provReason != null) {
            provState = 'failed';
          } else {
            provState = 'connected';
            token = _randomToken(32);
          }
        }
        return _send(res, 200, {
          'state': provState,
          if (provState == 'connected') ...{'ip': '127.0.0.1', 'port': 8080, 'token': token},
          if (provState == 'failed') 'reason': provReason,
        });
      case '/prov/finish':
        provState = 'idle';
        return _send(res, 200, {'ok': true});
    }
    _send(res, 404, {'ok': false, 'error': 'not found'});
  }

  void _api(HttpRequest req, HttpResponse res, String path, Map<String, String> p) {
    final auth = req.headers.value('authorization');
    if (token == null || auth != 'Bearer $token') return _send(res, 401, {'ok': false, 'error': 'unauthorized'});
    final isAdmin = req.headers.value('x-admin-key') == adminKey;
    bool needAdmin() {
      if (isAdmin) return false;
      _send(res, 403, {'ok': false, 'error': 'admin key required'});
      return true;
    }

    switch (path) {
      case '/api/info':
        return _send(res, 200, {
          'id': id, 'name': name, 'proto': 1, 'fw': {'main': fwMain, 'cam': fwCam}, //
          'characters': ['k3', 'fox'],
          'expressions': ['neutral', 'happy', 'sad', 'angry', 'surprised', 'shy', 'pout', 'wink', 'thinking', 'sleepy'],
        });
      case '/api/status':
        return _send(res, 200, {
          'servo0': angles[0], 'servo25': angles[1], 'servo26': angles[2], 'ui_mode': uiMode, //
          'ip': '127.0.0.1', 'heap': 41000 + _rng.nextInt(2000), 'volume': volume, 'tracking': tracking, 'cam': true,
          'rssi': -55, 'character': character, 'caption': caption, 'head_auto': headAuto,
          'uptime_s': DateTime.now().difference(boot).inSeconds,
        });
      case '/api/name':
        name = p['name'] ?? name;
        return _ok(res);
      case '/api/servo':
        final pin = int.tryParse(p['pin'] ?? ''), angle = double.tryParse(p['angle'] ?? '');
        final i = const {0: 0, 25: 1, 26: 2}[pin];
        if (i == null || angle == null) return _send(res, 400, {'ok': false, 'error': 'Missing pin or angle'});
        angles[i] = angle.clamp(limits[i]['min']!.toDouble(), limits[i]['max']!.toDouble());
        return _send(res, 200, {'success': true, 'pin': pin, 'angle': angles[i]});
      case '/api/center':
        for (var i = 0; i < 3; i++) {
          angles[i] = limits[i]['center']!.toDouble();
        }
        return _ok(res);
      case '/api/anim':
        if (p.containsKey('auto')) headAuto = p['auto'] == '1';
        if (p.isNotEmpty) return _ok(res);
        return _send(res, 200, {
          'anims': [
            {'name': 'nod', 'label': 'Nod', 'idle': false},
            {'name': 'shake', 'label': 'Shake', 'idle': false},
            {'name': 'bobble', 'label': 'Bobble', 'idle': true},
            {'name': 'curious', 'label': 'Curious', 'idle': true},
          ],
          'playing': false,
          'auto': headAuto,
        });
      case '/api/track':
        if (p.containsKey('flip') || p.containsKey('reset_dir')) {
          if (needAdmin()) return;
        }
        if (p.containsKey('on')) tracking = p['on'] == '1';
        return _send(res, 200, {'tracking': tracking, 'cam': true, 'yaw_dir': 1, 'pitch_dir': 1, 'roll_dir': 1});
      case '/api/volume':
        volume = int.tryParse(p['value'] ?? '')?.clamp(0, 100) ?? volume;
        return _send(res, 200, {'volume': volume});
      case '/api/ui_mode':
        uiMode = p['mode'] == 'chat' ? 'chat' : 'face';
        return _ok(res);
      case '/api/caption':
        caption = p['on'] == '1';
        return _ok(res);
      case '/api/character':
        if (!const ['k3', 'fox'].contains(p['name'])) return _send(res, 404, {'ok': false, 'error': 'unknown'});
        character = p['name']!;
        return _ok(res);
      case '/api/expression':
      case '/api/say':
        return _ok(res);
      case '/api/wifi/reset':
      case '/api/unpair':
        token = null;
        print('*** robot unpaired; pair again with:\n$pairingUri');
        return _ok(res);
      case '/api/ota/status':
        return _send(res, 200, {
          'state': otaState,
          'target': otaTarget,
          'progress': otaProgress,
          'available': otaState == 'available'
              ? [
                  {'target': 'main', 'version': '1.1.0', 'size': 2900000, 'notes': {'en': 'Mock update: smoother head motion', 'zh': '模拟更新：头部动作更顺滑'}}
                ]
              : [],
        });
      case '/api/ota/check':
        if (fwMain == '1.0.0' && otaState == 'idle') otaState = 'available';
        return _ok(res);
      case '/api/ota/start':
        if (otaTimer != null) return _send(res, 409, {'ok': false, 'error': 'busy'});
        otaTarget = p['target'] == 'cam' ? 'cam' : 'main';
        final version = p['version'] ?? '1.1.0';
        otaState = 'downloading';
        otaProgress = 0;
        otaTimer = Timer.periodic(const Duration(milliseconds: 300), (t) {
          otaProgress += 5;
          if (otaProgress >= 100) {
            otaProgress = 100;
            otaState = 'done';
            if (otaTarget == 'cam') {
              fwCam = version;
            } else {
              fwMain = version;
            }
            t.cancel();
            otaTimer = null;
          } else if (otaProgress >= 90) {
            otaState = 'installing';
          } else if (otaProgress >= 80) {
            otaState = 'verifying';
          }
        });
        return _ok(res);
      case '/api/speed':
        if (needAdmin()) return;
        final axis = int.tryParse(p['axis'] ?? ''), v = double.tryParse(p['value'] ?? '');
        if (axis != null && axis >= 0 && axis < 3 && v != null) speeds[axis] = v.clamp(10, 120);
        return _send(res, 200, {'speed': speeds});
      case '/api/admin/limits':
        if (needAdmin()) return;
        if (req.method == 'POST') {
          final axis = int.tryParse(p['axis'] ?? '');
          final mn = double.tryParse(p['min'] ?? ''), mx = double.tryParse(p['max'] ?? ''), c = double.tryParse(p['center'] ?? '');
          if (axis == null || axis < 0 || axis > 2 || mn == null || mx == null || c == null || !(mn < mx && c >= mn && c <= mx)) {
            return _send(res, 400, {'ok': false, 'error': 'invalid limit'});
          }
          limits[axis]
            ..['min'] = mn
            ..['max'] = mx
            ..['center'] = c;
          return _ok(res);
        }
        return _send(res, 200, {'axes': limits});
      case '/api/admin/diag':
        if (needAdmin()) return;
        return _send(res, 200, {
          'heap': 41000, 'min_heap': 18000, 'largest_block': 2100, //
          'uptime_s': DateTime.now().difference(boot).inSeconds, 'rssi': -55, 'cam': true, 'reset_reason': 'poweron',
        });
      case '/api/admin/reboot':
        if (needAdmin()) return;
        return _ok(res);
    }
    _send(res, 404, {'ok': false, 'error': 'not found'});
  }

  void _ok(HttpResponse res) => _send(res, 200, {'ok': true});

  void _send(HttpResponse res, int code, Map<String, Object?> body) {
    res
      ..statusCode = code
      ..headers.contentType = ContentType.json
      ..headers.set('Cache-Control', 'no-store')
      ..headers.set('X-Content-Type-Options', 'nosniff')
      ..write(jsonEncode(body));
    res.close();
  }
}

Future<void> main() async {
  final robot = MockRobot();
  // Loopback only: this mock has no rate limiting and is never meant to be reachable from the LAN.
  final server = await HttpServer.bind(InternetAddress.loopbackIPv4, 8080);
  print('Mock ENCO-02 listening on http://127.0.0.1:8080');
  print('Pairing code (paste in the dev app):\n${robot.pairingUri}');
  if (Platform.environment['MOCK_ADMIN_KEY'] == null) {
    print('WARNING: MOCK_ADMIN_KEY not set, generated a throwaway admin key for this run: ${robot.adminKey}');
  }
  print('Simulated failures: password "wrongpass" -> auth_failed, SSID "Neighbour" -> no_ap');
  await for (final req in server) {
    unawaited(robot.handle(req).catchError((Object e) {
      print('error: $e');
      req.response.statusCode = 500;
      req.response.close();
    }));
  }
}
