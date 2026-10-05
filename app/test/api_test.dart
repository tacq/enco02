import 'dart:convert';

import 'package:enco_app/core/json_http.dart';
import 'package:enco_app/core/provisioning_client.dart';
import 'package:enco_app/core/qr_payload.dart';
import 'package:enco_app/core/robot_api.dart';
import 'package:enco_app/models/robot_models.dart';
import 'package:test/test.dart';
import 'package:http/http.dart' as http;
import 'package:http/testing.dart';

const _token = 'TOKEN_abcdefghijklmnopqrstuvwxyz012345';

void main() {
  group('RobotApi', () {
    test('sends bearer token and form bodies, never secrets in URLs', () async {
      final seen = <http.Request>[];
      final api = RobotApi(
        host: '192.168.1.42',
        port: 80,
        token: _token,
        client: MockClient((req) async {
          seen.add(req);
          return http.Response('{"ok":true}', 200);
        }),
      );
      await api.setAngle(HeadAxis.yaw, 75);
      await api.say('你好');
      final r = seen.first;
      expect(r.method, 'POST');
      expect(r.headers['Authorization'], 'Bearer $_token');
      expect(r.url.toString(), 'http://192.168.1.42/api/servo');
      expect(r.bodyFields, {'pin': '26', 'angle': '75.0'});
      expect(seen[1].bodyFields['text'], '你好');
      for (final req in seen) {
        expect(req.url.toString().contains(_token), isFalse);
      }
    });

    test('admin calls fail closed without a key', () async {
      var called = false;
      final api = RobotApi(
        host: '192.168.1.42',
        port: 80,
        token: _token,
        client: MockClient((req) async {
          called = true;
          return http.Response('{}', 200);
        }),
      );
      await expectLater(api.limits(), throwsA(isA<ApiException>().having((e) => e.kind, 'kind', ApiErrorKind.forbidden)));
      expect(called, isFalse);
    });

    test('admin calls carry X-Admin-Key', () async {
      final api = RobotApi(
        host: '192.168.1.42',
        port: 80,
        token: _token,
        adminKey: 'adm',
        client: MockClient((req) async {
          expect(req.headers['X-Admin-Key'], 'adm');
          return http.Response(jsonEncode({'axes': [{'axis': 0, 'min': 45, 'max': 115, 'center': 88}]}), 200);
        }),
      );
      final limits = await api.limits();
      expect(limits[0].min, 45);
      expect(limits[2].max, HeadAxis.yaw.defaultMax);
    });

    test('maps HTTP errors', () async {
      Future<ApiErrorKind> kindFor(int code) async {
        final api = RobotApi(host: '10.0.0.2', port: 80, token: _token, client: MockClient((_) async => http.Response('{"ok":false,"error":"x"}', code)));
        try {
          await api.center();
        } on ApiException catch (e) {
          return e.kind;
        }
        throw StateError('no error');
      }

      expect(await kindFor(401), ApiErrorKind.unauthorized);
      expect(await kindFor(403), ApiErrorKind.forbidden);
      expect(await kindFor(409), ApiErrorKind.busy);
      expect(await kindFor(503), ApiErrorKind.unavailable);
    });

    test('raw console only allows /api paths', () async {
      final api = RobotApi(host: '10.0.0.2', port: 80, token: _token, adminKey: 'k', client: MockClient((_) async => http.Response('{}', 200)));
      expect(() => api.raw(post: false, path: '/prov/info', params: const {}), throwsA(isA<ApiException>()));
      expect(() => api.raw(post: false, path: '/api/../prov', params: const {}), throwsA(isA<ApiException>()));
      expect(await api.raw(post: false, path: '/api/status', params: const {}), isEmpty);
    });
  });

  group('ProvisioningClient', () {
    final payload = QrPayload.tryParse('enco://pair?v=1&id=ENCO-7F3A&ap=ENCO-7F3A&pw=secretpw12&s=AbCdEfGhIjKlMnOpQr_-')!;
    final target = PairingTarget(payload: payload, host: '192.168.4.1', port: 80, joinHotspot: true);

    test('rejects a pairing result pointing outside the LAN', () async {
      final client = ProvisioningClient(target, client: MockClient((req) async {
        expect(req.headers['X-Pair-Secret'], payload.pairingSecret);
        return http.Response(jsonEncode({'state': 'connected', 'ip': '8.8.8.8', 'port': 80, 'token': _token}), 200);
      }));
      await expectLater(client.result(), throwsA(isA<ApiException>()));
    });

    test('accepts a good result', () async {
      final client = ProvisioningClient(target, client: MockClient((req) async {
        return http.Response(jsonEncode({'state': 'connected', 'ip': '192.168.1.42', 'port': 80, 'token': _token}), 200);
      }));
      final r = await client.result();
      expect(r.state, ProvisionState.connected);
      expect(r.ip, '192.168.1.42');
    });

    test('verifyDevice detects a different robot', () async {
      final client = ProvisioningClient(target, client: MockClient((_) async => http.Response('{"id":"ENCO-OTHER"}', 200)));
      await expectLater(client.verifyDevice(), throwsA(isA<ApiException>()));
    });

    test('scan dedupes, validates and sorts', () async {
      final client = ProvisioningClient(target, client: MockClient((_) async => http.Response(jsonEncode({
            'networks': [
              {'ssid': 'B', 'rssi': -70, 'secure': true},
              {'ssid': 'A', 'rssi': -40, 'secure': true},
              {'ssid': 'A', 'rssi': -90, 'secure': true},
              {'ssid': '', 'rssi': -10},
              {'ssid': 42},
            ]
          }), 200)));
      final nets = await client.scan();
      expect(nets.map((n) => n.ssid), ['A', 'B']);
    });
  });
}
