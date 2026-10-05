import 'package:http/http.dart' as http;

import '../models/robot_models.dart';
import 'firmware.dart';
import 'json_http.dart';
import 'validators.dart';

/// Client for the robot's LAN API (docs/robot_api.md section 3). Every request carries the
/// per-pairing bearer token; developer endpoints additionally carry the admin key. Neither is ever
/// logged or put in a URL.
class RobotApi {
  RobotApi({required String host, required int port, required String token, String? adminKey, http.Client? client})
      : _http = JsonHttp(host: host, port: port, client: client),
        _token = token,
        _adminKey = adminKey;

  final JsonHttp _http;
  final String _token;
  final String? _adminKey;

  String get host => _http.host;
  int get port => _http.port;
  bool get hasAdminKey => _adminKey != null && _adminKey.isNotEmpty;

  Map<String, String> get _auth => {'Authorization': 'Bearer $_token'};

  Map<String, String> get _admin {
    final key = _adminKey;
    // Fail closed: without a key we do not even send the request.
    if (key == null || key.isEmpty) throw const ApiException(ApiErrorKind.forbidden, 'admin key not set');
    return {..._auth, 'X-Admin-Key': key};
  }

  Future<Map<String, dynamic>> _get(String path, [Map<String, String>? q]) => _http.get(path, query: q, headers: _auth);
  Future<Map<String, dynamic>> _post(String path, [Map<String, String>? f]) => _http.post(path, form: f, headers: _auth);

  // ---- info & status ----

  Future<RobotInfo> info({Duration? timeout}) async =>
      RobotInfo.fromJson(await _http.get('/api/info', headers: _auth, timeout: timeout));

  Future<RobotStatus> status() async => RobotStatus.fromJson(await _get('/api/status'));

  Future<void> rename(String name) {
    if (!Validators.robotName(name)) throw const ApiException(ApiErrorKind.badRequest, 'bad name');
    return _post('/api/name', {'name': name.trim()});
  }

  // ---- head ----

  Future<void> setAngle(HeadAxis axis, double angle) =>
      _post('/api/servo', {'pin': '${axis.pin}', 'angle': angle.clamp(0, 180).toStringAsFixed(1)});

  Future<void> center() => _post('/api/center');

  Future<AnimationList> animations() async => AnimationList.fromJson(await _get('/api/anim'));

  Future<void> playAnimation(String name) {
    if (!Validators.identifier(name)) throw const ApiException(ApiErrorKind.badRequest, 'bad name');
    return _post('/api/anim', {'name': name});
  }

  Future<void> playRandomAnimation() => _post('/api/anim', {'random': '1'});
  Future<void> stopAnimation() => _post('/api/anim', {'stop': '1'});
  Future<void> setHeadAuto(bool on) => _post('/api/anim', {'auto': on ? '1' : '0'});

  // ---- face, voice, modes ----

  Future<void> setTracking(bool on) => _post('/api/track', {'on': on ? '1' : '0'});
  Future<void> setVolume(int v) => _post('/api/volume', {'value': '${v.clamp(0, 100)}'});
  Future<void> setUiMode(String mode) => _post('/api/ui_mode', {'mode': mode == 'chat' ? 'chat' : 'face'});
  Future<void> setCaption(bool on) => _post('/api/caption', {'on': on ? '1' : '0'});

  Future<void> setCharacter(String name) {
    if (!Validators.identifier(name)) throw const ApiException(ApiErrorKind.badRequest, 'bad name');
    return _post('/api/character', {'name': name});
  }

  Future<void> setExpression(String name) {
    if (!Validators.identifier(name)) throw const ApiException(ApiErrorKind.badRequest, 'bad name');
    return _post('/api/expression', {'name': name});
  }

  /// The robot speaks/answers this as if it was heard. 1-120 UTF-8 bytes (firmware limit).
  Future<void> say(String text) {
    final t = text.trim();
    if (t.isEmpty || t.length > 120) throw const ApiException(ApiErrorKind.badRequest, 'text must be 1-120 bytes');
    return _post('/api/say', {'text': t});
  }

  // ---- Wi-Fi / pairing ----

  Future<void> resetWifi() => _post('/api/wifi/reset');
  Future<void> unpair() => _post('/api/unpair');

  // ---- firmware updates ----

  Future<OtaStatus> otaStatus() async => OtaStatus.fromJson(await _get('/api/ota/status'));
  Future<void> otaCheck() => _post('/api/ota/check');

  /// Starts an install. With [component] (found by the app in the manifest) the robot downloads that
  /// exact image and checks its sha256; without it, the robot installs what it found itself.
  Future<void> otaStart(String target, {FirmwareComponent? component}) {
    if (!OtaStatus.isTarget(target)) throw const ApiException(ApiErrorKind.badRequest, 'bad target');
    final form = <String, String>{'target': target};
    if (component != null) {
      form.addAll({
        'version': component.version,
        'url': component.url.toString(),
        'sha256': component.sha256,
        'size': '${component.size}',
      });
    }
    return _post('/api/ota/start', form);
  }

  // ---- developer / admin ----

  Future<List<AxisLimit>> limits() async =>
      AxisLimit.listFromJson(await _http.get('/api/admin/limits', headers: _admin));

  Future<void> setLimit(AxisLimit l, {bool save = false}) {
    if (!l.isValid) throw const ApiException(ApiErrorKind.badRequest, 'invalid limit');
    return _http.post('/api/admin/limits', headers: _admin, form: {
      'axis': '${l.axis.index}',
      'min': l.min.toStringAsFixed(1),
      'max': l.max.toStringAsFixed(1),
      'center': l.center.toStringAsFixed(1),
      if (save) 'save': '1',
    });
  }

  Future<List<double>> speeds() async => _parseSpeeds(await _http.get('/api/speed', headers: _admin));

  Future<List<double>> setSpeed(HeadAxis axis, double degS) async => _parseSpeeds(await _http
      .post('/api/speed', headers: _admin, form: {'axis': '${axis.index}', 'value': degS.clamp(10, 120).toStringAsFixed(0)}));

  Future<void> testAxis(HeadAxis axis) => _http.post('/api/speed', headers: _admin, form: {'test': '${axis.index}'});
  Future<void> saveSpeeds() => _http.post('/api/speed', headers: _admin, form: {'save': '1'});

  Future<void> flipTracking(HeadAxis axis) => _http.post('/api/track', headers: _admin, form: {'flip': axis.name});
  Future<void> resetTrackingDirs() => _http.post('/api/track', headers: _admin, form: {'reset_dir': '1'});

  Future<Diagnostics> diagnostics() async => Diagnostics.fromJson(await _http.get('/api/admin/diag', headers: _admin));
  Future<void> reboot() => _http.post('/api/admin/reboot', headers: _admin);

  static final RegExp _consolePath = RegExp(r'^/api/[a-z0-9_/]{1,48}$');
  static final RegExp _consoleKey = RegExp(r'^[a-z0-9_]{1,24}$');

  /// Developer console: arbitrary request, but only to /api/* paths and with simple param names.
  Future<Map<String, dynamic>> raw({required bool post, required String path, required Map<String, String> params}) {
    if (!_consolePath.hasMatch(path) || path.contains('..') || !params.keys.every(_consoleKey.hasMatch)) {
      throw const ApiException(ApiErrorKind.badRequest, 'path must be /api/... and keys [a-z0-9_]');
    }
    return post ? _http.post(path, form: params, headers: _admin) : _http.get(path, query: params, headers: _admin);
  }

  static List<double> _parseSpeeds(Map<String, dynamic> j) {
    final l = j.list('speed').whereType<num>().map((e) => e.toDouble()).toList();
    return l.length == 3 ? l : const [40, 40, 40];
  }

  void close() => _http.close();
}
