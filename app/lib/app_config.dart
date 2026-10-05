/// Build flavor. `user` is the store app; `dev` adds the Developer tab and the mock-robot pairing
/// shortcut. Selected by the entrypoint (main_user.dart / main_dev.dart).
enum Flavor { user, dev }

class AppConfig {
  const AppConfig({required this.flavor});

  final Flavor flavor;

  bool get isDev => flavor == Flavor.dev;

  /// Published with every GitHub release, see docs/firmware_updates.md.
  static const String manifestUrl = 'https://github.com/tacq/enco02/releases/latest/download/manifest.json';

  /// Hosts a manifest or firmware binary may be fetched from. Anything else is rejected.
  static const Set<String> trustedDownloadHosts = {
    'github.com',
    'objects.githubusercontent.com',
    'release-assets.githubusercontent.com',
  };

  /// SoftAP address while the robot is in pairing mode.
  static const String pairingHost = '192.168.4.1';
  static const int pairingPort = 80;

  /// UDP port for LAN discovery (robot_api.md section 2).
  static const int discoveryPort = 48902;

  /// The local mock robot (tool/mock_robot.dart), dev flavor only.
  static const String mockHost = '127.0.0.1';
  static const int mockPort = 8080;
}
