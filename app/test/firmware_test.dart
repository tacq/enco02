import 'dart:convert';

import 'package:enco_app/core/firmware.dart';
import 'package:enco_app/core/json_http.dart';
import 'package:enco_app/models/robot_models.dart';
import 'package:test/test.dart';
import 'package:http/http.dart' as http;
import 'package:http/testing.dart';

Map<String, dynamic> _manifest({String url = 'https://github.com/tacq/enco02/releases/download/v1.2.0/enco02_main.bin'}) => {
      'schema': 1,
      'components': {
        'main': {
          'version': '1.2.0',
          'url': url,
          'sha256': 'a' * 64,
          'size': 2900000,
          'min_from': '1.0.0',
          'notes': {'en': 'Smoother', 'zh': '更顺滑'},
        },
        'cam': {
          'version': '1.0.0',
          'url': 'https://github.com/tacq/enco02/releases/download/v1.2.0/enco02_cam.bin',
          'sha256': 'b' * 64,
          'size': 1300000,
        },
      },
    };

void main() {
  test('compareVersions', () {
    expect(compareVersions('1.2.0', '1.10.0'), lessThan(0));
    expect(compareVersions('2.0.0', '1.99.99'), greaterThan(0));
    expect(compareVersions('1.0.0', '1.0.0'), 0);
  });

  test('manifest parsing and update detection', () {
    final m = FirmwareManifest.fromJson(_manifest());
    expect(m.components.keys, containsAll(['main', 'cam']));
    final updates = m.updatesFor(const FirmwareVersions(main: '1.1.0', cam: '1.0.0'));
    expect(updates.map((c) => c.target), ['main']);
    expect(updates.single.notesFor('zh'), '更顺滑');
    // Below min_from: not offered.
    expect(m.updatesFor(const FirmwareVersions(main: '0.9.0')).isEmpty, isTrue);
  });

  test('untrusted or non-https image URLs are dropped', () {
    for (final url in [
      'http://github.com/x.bin',
      'https://evil.example.com/x.bin',
      'https://github.com:8443/x.bin',
    ]) {
      expect(FirmwareManifest.fromJson(_manifest(url: url)).components.containsKey('main'), isFalse, reason: url);
    }
  });

  test('fetchManifest follows redirects only to trusted hosts', () async {
    final client = MockClient((req) async {
      if (req.url.host == 'github.com') {
        return http.Response('', 302, headers: {'location': 'https://objects.githubusercontent.com/m.json'});
      }
      if (req.url.host == 'objects.githubusercontent.com') return http.Response.bytes(utf8.encode(jsonEncode(_manifest())), 200);
      return http.Response('no', 500);
    });
    final m = await FirmwareService(client: client).fetchManifest(Uri.parse('https://github.com/tacq/enco02/releases/latest/download/manifest.json'));
    expect(m.components['main']!.version, '1.2.0');

    final evil = MockClient((req) async => http.Response('', 302, headers: {'location': 'https://evil.example.com/m.json'}));
    expect(
      () => FirmwareService(client: evil).fetchManifest(Uri.parse('https://github.com/m.json')),
      throwsA(isA<ApiException>()),
    );
  });

  test('OtaStatus parsing is defensive', () {
    final s = OtaStatus.fromJson({
      'state': 'downloading',
      'target': 'main',
      'progress': 250,
      'available': [
        {'target': 'main', 'version': '1.2.0'},
        {'target': 'toaster', 'version': '9.9.9'},
        {'target': 'cam', 'version': 'latest'},
      ],
    });
    expect(s.state, OtaState.downloading);
    expect(s.progress, 100);
    expect(s.busy, isTrue);
    expect(s.available.map((e) => e.target), ['main']);
    expect(OtaStatus.fromJson({'state': 'weird'}).state, OtaState.idle);
  });
}
