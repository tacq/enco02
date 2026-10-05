import 'app.dart';
import 'app_config.dart';

/// Store build (normal users). `flutter run --flavor user -t lib/main_user.dart`
Future<void> main() => runEncoApp(const AppConfig(flavor: Flavor.user));
