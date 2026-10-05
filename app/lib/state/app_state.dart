import 'package:flutter/widgets.dart';

import '../app_config.dart';
import '../core/robot_store.dart';
import '../models/saved_robot.dart';

/// App-wide state: paired robots, language override, developer admin key.
class AppState extends ChangeNotifier {
  AppState({required this.config, RobotStore? store})
      : store = store ?? RobotStore(allowLoopback: config.isDev);

  final AppConfig config;
  final RobotStore store;

  List<SavedRobot> _robots = [];
  Locale? _locale;
  String? _adminKey;
  bool _loaded = false;

  List<SavedRobot> get robots => List.unmodifiable(_robots);
  Locale? get locale => _locale;
  String? get adminKey => config.isDev ? _adminKey : null;
  bool get loaded => _loaded;

  Future<void> load() async {
    _robots = await store.loadRobots();
    final code = await store.loadLocale();
    _locale = (code == 'en' || code == 'zh') ? Locale(code!) : null;
    // The admin key is only ever read in the dev flavor.
    _adminKey = config.isDev ? await store.loadAdminKey() : null;
    _loaded = true;
    notifyListeners();
  }

  SavedRobot? robot(String id) {
    for (final r in _robots) {
      if (r.id == id) return r;
    }
    return null;
  }

  /// Adds or replaces (re-pairing the same robot gives it a new token).
  Future<void> upsertRobot(SavedRobot r) async {
    _robots = [..._robots.where((e) => e.id != r.id), r];
    await store.saveRobots(_robots);
    notifyListeners();
  }

  Future<void> removeRobot(String id) async {
    _robots = _robots.where((e) => e.id != id).toList();
    await store.saveRobots(_robots);
    notifyListeners();
  }

  Future<void> setLocale(Locale? l) async {
    _locale = l;
    await store.saveLocale(l?.languageCode);
    notifyListeners();
  }

  Future<void> setAdminKey(String? key) async {
    if (!config.isDev) return;
    _adminKey = (key == null || key.trim().isEmpty) ? null : key.trim();
    await store.saveAdminKey(_adminKey);
    notifyListeners();
  }
}

/// Makes [AppState] available to the widget tree and rebuilds dependents when it changes.
class AppScope extends InheritedNotifier<AppState> {
  const AppScope({super.key, required AppState state, required super.child}) : super(notifier: state);

  static AppState of(BuildContext context) => context.dependOnInheritedWidgetOfExactType<AppScope>()!.notifier!;

  /// Read without subscribing to rebuilds (for callbacks).
  static AppState read(BuildContext context) => context.getInheritedWidgetOfExactType<AppScope>()!.notifier!;
}
