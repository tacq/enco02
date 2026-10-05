import '../core/validators.dart';

/// A paired robot as remembered by the app. Persisted (including the token) in secure storage only.
class SavedRobot {
  const SavedRobot({
    required this.id,
    required this.name,
    required this.host,
    required this.port,
    required this.token,
  });

  final String id;
  final String name;
  final String host;
  final int port;
  final String token;

  SavedRobot copyWith({String? name, String? host, int? port, String? token}) => SavedRobot(
        id: id,
        name: name ?? this.name,
        host: host ?? this.host,
        port: port ?? this.port,
        token: token ?? this.token,
      );

  Map<String, dynamic> toJson() => {'id': id, 'name': name, 'host': host, 'port': port, 'token': token};

  static SavedRobot? fromJson(Object? j, {required bool allowLoopback}) {
    if (j is! Map) return null;
    final id = j['id'], name = j['name'], host = j['host'], port = j['port'], token = j['token'];
    if (id is! String || name is! String || host is! String || port is! int || token is! String) return null;
    if (!Validators.deviceId(id) ||
        !Validators.lanHost(host, allowLoopback: allowLoopback) ||
        !Validators.port(port) ||
        !Validators.deviceToken(token)) {
      return null;
    }
    return SavedRobot(id: id, name: Validators.robotName(name) ? name : id, host: host, port: port, token: token);
  }
}
