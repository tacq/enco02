import 'app.dart';
import 'app_config.dart';

/// Developer/admin build: adds the Developer tab (motor limits, speeds, diagnostics, raw console)
/// and the mock-robot pairing shortcut. `flutter run --flavor dev -t lib/main_dev.dart`
Future<void> main() => runEncoApp(const AppConfig(flavor: Flavor.dev));
