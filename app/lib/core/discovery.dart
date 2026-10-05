import 'dart:async';
import 'dart:convert';
import 'dart:io';

import '../app_config.dart';
import 'validators.dart';

class DiscoveredRobot {
  const DiscoveredRobot({required this.id, required this.name, required this.host, required this.port});

  final String id;
  final String name;
  final String host;
  final int port;
}

/// Finds a robot on the LAN after its DHCP address changed (docs/robot_api.md section 2).
///
/// iOS 14+ only lets apps send broadcast with the com.apple.developer.networking.multicast
/// entitlement, which Apple grants on request. Without it this returns nothing on iOS and the user
/// re-pairs. TODO: request the entitlement before App Store submission.
class Discovery {
  Discovery({this.allowLoopback = false});

  final bool allowLoopback;

  Future<DiscoveredRobot?> find(String deviceId, {Duration timeout = const Duration(seconds: 3)}) async {
    final all = await scan(deviceId: deviceId, timeout: timeout, stopAtFirst: true);
    return all.isEmpty ? null : all.first;
  }

  Future<List<DiscoveredRobot>> scan({String deviceId = '*', Duration timeout = const Duration(seconds: 3), bool stopAtFirst = false}) async {
    final RawDatagramSocket socket;
    try {
      socket = await RawDatagramSocket.bind(InternetAddress.anyIPv4, 0);
    } on SocketException {
      return const [];
    }
    final found = <String, DiscoveredRobot>{};
    final done = Completer<void>();
    socket.broadcastEnabled = true;
    final sub = socket.listen((event) {
      if (event != RawSocketEvent.read) return;
      final dg = socket.receive();
      if (dg == null || dg.data.length > 512) return;
      final r = _parse(dg);
      if (r == null || (deviceId != '*' && r.id != deviceId)) return;
      found[r.id] = r;
      if (stopAtFirst && !done.isCompleted) done.complete();
    });
    final query = utf8.encode(jsonEncode({'q': 'enco', 'id': deviceId}));
    try {
      // Twice: the first broadcast is often dropped while the radio wakes up.
      socket.send(query, InternetAddress('255.255.255.255'), AppConfig.discoveryPort);
      await Future<void>.delayed(const Duration(milliseconds: 300));
      socket.send(query, InternetAddress('255.255.255.255'), AppConfig.discoveryPort);
    } on SocketException {
      // No network / not permitted.
    }
    await Future.any([done.future, Future<void>.delayed(timeout)]);
    await sub.cancel();
    socket.close();
    return found.values.toList();
  }

  DiscoveredRobot? _parse(Datagram dg) {
    try {
      final j = jsonDecode(utf8.decode(dg.data));
      if (j is! Map<String, dynamic>) return null;
      final id = j['id'], name = j['name'], ip = j['ip'], port = j['port'] ?? 80;
      if (id is! String || ip is! String || port is! int) return null;
      // The reply must come from the address it claims - otherwise any host could redirect us.
      if (ip != dg.address.address) return null;
      if (!Validators.deviceId(id) || !Validators.lanHost(ip, allowLoopback: allowLoopback) || !Validators.port(port)) {
        return null;
      }
      final n = name is String && Validators.robotName(name) ? name : id;
      return DiscoveredRobot(id: id, name: n, host: ip, port: port);
    } on FormatException {
      return null;
    }
  }
}
