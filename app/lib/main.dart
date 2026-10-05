import 'app.dart';
import 'app_config.dart';

/// Default `flutter run` target = the user app. Use main_dev.dart for the developer build.
Future<void> main() => runEncoApp(const AppConfig(flavor: Flavor.user));
