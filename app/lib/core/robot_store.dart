import 'dart:convert';

import 'package:flutter_secure_storage/flutter_secure_storage.dart';

import '../models/saved_robot.dart';

/// Persists paired robots (with their tokens), the admin key and the language choice in the
/// platform keystore (iOS Keychain / Android Keystore-backed storage). Nothing goes to plain prefs.
class RobotStore {
  RobotStore({FlutterSecureStorage? storage, required this.allowLoopback})
      : _storage = storage ?? const FlutterSecureStorage();

  final FlutterSecureStorage _storage;
  final bool allowLoopback;

  static const _kRobots = 'robots.v1';
  static const _kAdminKey = 'admin_key.v1';
  static const _kLocale = 'locale.v1';

  Future<List<SavedRobot>> loadRobots() async {
    final raw = await _storage.read(key: _kRobots);
    if (raw == null) return [];
    try {
      final list = jsonDecode(raw);
      if (list is! List) return [];
      return list.map((e) => SavedRobot.fromJson(e, allowLoopback: allowLoopback)).whereType<SavedRobot>().toList();
    } on FormatException {
      return [];
    }
  }

  Future<void> saveRobots(List<SavedRobot> robots) =>
      _storage.write(key: _kRobots, value: jsonEncode(robots.map((r) => r.toJson()).toList()));

  Future<String?> loadAdminKey() => _storage.read(key: _kAdminKey);

  Future<void> saveAdminKey(String? key) =>
      key == null || key.isEmpty ? _storage.delete(key: _kAdminKey) : _storage.write(key: _kAdminKey, value: key);

  /// `null` = follow the system language.
  Future<String?> loadLocale() => _storage.read(key: _kLocale);

  Future<void> saveLocale(String? code) =>
      code == null ? _storage.delete(key: _kLocale) : _storage.write(key: _kLocale, value: code);
}
