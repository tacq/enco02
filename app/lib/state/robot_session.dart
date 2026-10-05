import 'dart:async';

import 'package:flutter/foundation.dart';

import '../core/discovery.dart';
import '../core/json_http.dart';
import '../core/robot_api.dart';
import '../models/robot_models.dart';
import '../models/saved_robot.dart';
import 'app_state.dart';

enum Connection { connecting, online, offline, unauthorized }

/// Live connection to one paired robot: finds it (saved address, then LAN discovery), polls its
/// status while the control screen is open, and exposes the API client.
class RobotSession extends ChangeNotifier {
  RobotSession({required this.app, required SavedRobot robot}) : _robot = robot;

  final AppState app;
  SavedRobot _robot;
  RobotApi? _api;
  Timer? _poll;
  int _failures = 0;
  bool _disposed = false;

  Connection connection = Connection.connecting;
  RobotInfo? info;
  RobotStatus? status;
  OtaStatus ota = OtaStatus.idle;
  AnimationList? animations;

  SavedRobot get robot => _robot;
  RobotApi? get api => _api;

  static const _pollInterval = Duration(seconds: 2);
  static const _otaPollInterval = Duration(seconds: 1);

  Future<void> start() async {
    connection = Connection.connecting;
    _notify();
    if (!await _tryConnect(_robot.host, _robot.port)) {
      final found = await Discovery(allowLoopback: app.config.isDev).find(_robot.id);
      if (found == null || !await _tryConnect(found.host, found.port)) {
        if (connection != Connection.unauthorized) connection = Connection.offline;
        _notify();
        _schedulePoll(const Duration(seconds: 10));
        return;
      }
      _robot = _robot.copyWith(host: found.host, port: found.port);
      await app.upsertRobot(_robot);
    }
    connection = Connection.online;
    _failures = 0;
    _notify();
    unawaited(_refreshExtras());
    _schedulePoll(_pollInterval);
  }

  Future<bool> _tryConnect(String host, int port) async {
    final api = RobotApi(host: host, port: port, token: _robot.token, adminKey: app.adminKey);
    try {
      info = await api.info(timeout: const Duration(seconds: 3));
      if (info!.id != _robot.id) {
        api.close();
        return false;
      }
      _api?.close();
      _api = api;
      if (info!.name != _robot.name) {
        _robot = _robot.copyWith(name: info!.name);
        await app.upsertRobot(_robot);
      }
      return true;
    } on ApiException catch (e) {
      api.close();
      if (e.kind == ApiErrorKind.unauthorized) connection = Connection.unauthorized;
      return false;
    }
  }

  /// Re-creates the client after the admin key changed (dev flavor).
  void rebuildApi() {
    final old = _api;
    if (old == null) return;
    _api = RobotApi(host: old.host, port: old.port, token: _robot.token, adminKey: app.adminKey);
    old.close();
    _notify();
  }

  Future<void> _refreshExtras() async {
    final api = _api;
    if (api == null) return;
    try {
      animations = await api.animations();
    } on ApiException {
      // Optional.
    }
    await refreshOta();
    _notify();
  }

  Future<void> refreshOta() async {
    try {
      ota = await _api?.otaStatus() ?? ota;
    } on ApiException {
      // Firmware without OTA support yet.
    }
    _notify();
  }

  void _schedulePoll(Duration d) {
    _poll?.cancel();
    if (_disposed) return;
    _poll = Timer(d, _tick);
  }

  Future<void> _tick() async {
    if (_disposed) return;
    if (connection != Connection.online) {
      await start();
      return;
    }
    try {
      status = await _api!.status();
      _failures = 0;
      if (ota.busy) await refreshOta();
    } on ApiException catch (e) {
      if (e.kind == ApiErrorKind.unauthorized) {
        connection = Connection.unauthorized;
      } else if (++_failures >= 3) {
        connection = Connection.offline;
      }
    }
    _notify();
    _schedulePoll(ota.busy ? _otaPollInterval : _pollInterval);
  }

  /// Runs a command; returns the error (for a snackbar) or null.
  Future<ApiException?> run(Future<void> Function(RobotApi api) action) async {
    final api = _api;
    if (api == null) return const ApiException(ApiErrorKind.network);
    try {
      await action(api);
      return null;
    } on ApiException catch (e) {
      if (e.kind == ApiErrorKind.unauthorized) {
        connection = Connection.unauthorized;
        _notify();
      }
      return e;
    }
  }

  /// Applies the robot's new name locally after a successful rename.
  Future<void> applyRename(String name) async {
    _robot = _robot.copyWith(name: name);
    await app.upsertRobot(_robot);
    _notify();
  }

  void _notify() {
    if (!_disposed) notifyListeners();
  }

  @override
  void dispose() {
    _disposed = true;
    _poll?.cancel();
    _api?.close();
    super.dispose();
  }
}
