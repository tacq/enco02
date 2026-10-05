import 'package:enco_app/core/qr_payload.dart';
import 'package:enco_app/core/validators.dart';
import 'package:test/test.dart';

void main() {
  const good = 'enco://pair?v=1&id=ENCO-7F3A&ap=ENCO-7F3A&pw=secretpw12&s=AbCdEfGhIjKlMnOpQr_-';

  group('QrPayload', () {
    test('parses a valid code', () {
      final p = QrPayload.tryParse(good)!;
      expect(p.deviceId, 'ENCO-7F3A');
      expect(p.apSsid, 'ENCO-7F3A');
      expect(p.apPassword, 'secretpw12');
      expect(p.pairingSecret, 'AbCdEfGhIjKlMnOpQr_-');
    });

    test('round-trips', () {
      final p = QrPayload.tryParse(good)!;
      expect(QrPayload.tryParse(p.toUri())!.pairingSecret, p.pairingSecret);
    });

    test('rejects other schemes, versions and bad fields', () {
      for (final bad in [
        null,
        '',
        'https://example.com',
        good.replaceFirst('enco://', 'http://'),
        good.replaceFirst('v=1', 'v=2'),
        good.replaceFirst('id=ENCO-7F3A', 'id=../etc'),
        good.replaceFirst('pw=secretpw12', 'pw=short'),
        good.replaceFirst('s=AbCdEfGhIjKlMnOpQr_-', 's=tooshort'),
        good.replaceFirst('s=AbCdEfGhIjKlMnOpQr_-', 's=has%20space%20in%20it%20xxxx'),
        'enco://pair?v=1&id=ENCO-7F3A&ap=ENCO-7F3A&pw=secretpw12',
        'x' * 600,
      ]) {
        expect(QrPayload.tryParse(bad), isNull, reason: '$bad');
      }
    });
  });

  group('Validators.lanHost', () {
    test('accepts private ranges', () {
      for (final ip in ['10.0.0.5', '172.16.1.1', '172.31.255.1', '192.168.1.42', '169.254.3.3']) {
        expect(Validators.lanHost(ip), isTrue, reason: ip);
      }
    });

    test('rejects public, loopback (unless allowed), names and IPv6', () {
      for (final ip in ['8.8.8.8', '172.32.0.1', '127.0.0.1', 'example.com', '::1', '192.168.1.300', '']) {
        expect(Validators.lanHost(ip), isFalse, reason: ip);
      }
      expect(Validators.lanHost('127.0.0.1', allowLoopback: true), isTrue);
    });
  });

  group('Validators misc', () {
    test('ssid', () {
      expect(Validators.ssid('Home'), isTrue);
      expect(Validators.ssid('家里的网络'), isTrue);
      expect(Validators.ssid(''), isFalse);
      expect(Validators.ssid('a' * 33), isFalse);
      expect(Validators.ssid('bad\nname'), isFalse);
    });

    test('home wifi password', () {
      expect(Validators.homeWifiPassword(''), isTrue);
      expect(Validators.homeWifiPassword('12345678'), isTrue);
      expect(Validators.homeWifiPassword('1234567'), isFalse);
      expect(Validators.homeWifiPassword('x' * 64), isFalse);
    });

    test('robot name', () {
      expect(Validators.robotName('小智'), isTrue);
      expect(Validators.robotName('   '), isFalse);
      expect(Validators.robotName('名' * 9), isFalse); // 27 bytes
    });
  });
}
