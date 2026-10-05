import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:http/http.dart' as http;

import '../app_config.dart';
import '../models/robot_models.dart';
import 'json_http.dart';
import 'validators.dart';

/// Compares dotted `major.minor.patch` versions. Returns <0, 0, >0.
int compareVersions(String a, String b) {
  List<int> parts(String v) => v.split('.').map((e) => int.tryParse(e) ?? 0).toList();
  final pa = parts(a), pb = parts(b);
  for (var i = 0; i < 3; i++) {
    final x = i < pa.length ? pa[i] : 0, y = i < pb.length ? pb[i] : 0;
    if (x != y) return x.compareTo(y);
  }
  return 0;
}

bool isTrustedDownload(Uri u) => u.scheme == 'https' && AppConfig.trustedDownloadHosts.contains(u.host) && !u.hasPort;

/// One updatable board ("main" or "cam") in manifest.json (docs/firmware_updates.md).
class FirmwareComponent {
  const FirmwareComponent({
    required this.target,
    required this.version,
    required this.url,
    required this.sha256,
    required this.size,
    this.minFrom,
    this.notes = const {},
  });

  final String target;
  final String version;
  final Uri url;
  final String sha256;
  final int size;
  final String? minFrom;
  final Map<String, String> notes;

  static const int maxImageBytes = 4 * 1024 * 1024;

  String notesFor(String lang) => notes[lang] ?? notes['en'] ?? '';

  /// Whether this is newer than [installed] and can be applied on top of it.
  bool isUpdateFor(String? installed) {
    if (installed == null) return false;
    if (compareVersions(version, installed) <= 0) return false;
    final from = minFrom;
    return from == null || compareVersions(installed, from) >= 0;
  }

  static FirmwareComponent? fromJson(String target, Map<String, dynamic>? j) {
    if (j == null || !OtaStatus.isTarget(target)) return null;
    final version = j.str('version'), url = Uri.tryParse(j.str('url') ?? ''), sha = j.str('sha256');
    final size = j.integer('size'), minFrom = j.str('min_from');
    if (!Validators.version(version) || url == null || !isTrustedDownload(url) || !Validators.sha256Hex(sha)) {
      return null;
    }
    if (size == null || size <= 0 || size > maxImageBytes) return null;
    return FirmwareComponent(
      target: target,
      version: version!,
      url: url,
      sha256: sha!,
      size: size,
      minFrom: Validators.version(minFrom) ? minFrom : null,
      notes: OtaStatus.parseNotes(j.obj('notes')),
    );
  }
}

class FirmwareManifest {
  const FirmwareManifest(this.components);

  final Map<String, FirmwareComponent> components;

  static FirmwareManifest fromJson(Map<String, dynamic> j) {
    if (j.integer('schema') != 1) throw const ApiException(ApiErrorKind.badResponse, 'unsupported manifest schema');
    final comps = j.obj('components') ?? const {};
    final out = <String, FirmwareComponent>{};
    for (final t in const ['main', 'cam']) {
      final c = FirmwareComponent.fromJson(t, comps.obj(t));
      if (c != null) out[t] = c;
    }
    return FirmwareManifest(out);
  }

  /// Components newer than what the robot reports.
  List<FirmwareComponent> updatesFor(FirmwareVersions installed) =>
      components.values.where((c) => c.isUpdateFor(installed.of(c.target))).toList();
}

/// Fetches the release manifest from GitHub over HTTPS. Redirects are followed by hand so every hop
/// is checked against [AppConfig.trustedDownloadHosts].
///
/// TODO(security): verify an ed25519 signature over the manifest (public key in the app and the
/// firmware). Today integrity rests on HTTPS + GitHub account security; the robot re-checks sha256.
class FirmwareService {
  FirmwareService({http.Client? client}) : _client = client ?? http.Client();

  final http.Client _client;

  static const int _maxManifestBytes = 32 * 1024;
  static const int _maxRedirects = 5;

  Future<FirmwareManifest> fetchManifest(Uri url) async {
    var current = url;
    for (var hop = 0; hop <= _maxRedirects; hop++) {
      if (!isTrustedDownload(current)) {
        throw const ApiException(ApiErrorKind.badResponse, 'untrusted manifest host');
      }
      final req = http.Request('GET', current)
        ..followRedirects = false
        ..headers['Accept'] = 'application/json';
      final http.StreamedResponse res;
      try {
        res = await _client.send(req).timeout(const Duration(seconds: 15));
      } on TimeoutException {
        throw const ApiException(ApiErrorKind.timeout);
      } on SocketException {
        throw const ApiException(ApiErrorKind.network);
      } on http.ClientException {
        throw const ApiException(ApiErrorKind.network);
      }
      if (res.isRedirect || (res.statusCode >= 300 && res.statusCode < 400)) {
        final loc = res.headers['location'];
        await res.stream.drain<void>();
        final next = loc == null ? null : current.resolve(loc);
        if (next == null) throw const ApiException(ApiErrorKind.badResponse, 'bad redirect');
        current = next;
        continue;
      }
      if (res.statusCode == 404) {
        await res.stream.drain<void>();
        throw const ApiException(ApiErrorKind.notFound, 'no release published');
      }
      if (res.statusCode != 200) {
        await res.stream.drain<void>();
        throw ApiException(ApiErrorKind.badResponse, 'HTTP ${res.statusCode}');
      }
      final bytes = <int>[];
      await for (final chunk in res.stream) {
        bytes.addAll(chunk);
        if (bytes.length > _maxManifestBytes) throw const ApiException(ApiErrorKind.badResponse, 'manifest too large');
      }
      final Object? decoded;
      try {
        decoded = jsonDecode(utf8.decode(bytes));
      } on FormatException {
        throw const ApiException(ApiErrorKind.badResponse, 'manifest is not JSON');
      }
      if (decoded is! Map<String, dynamic>) throw const ApiException(ApiErrorKind.badResponse, 'bad manifest');
      return FirmwareManifest.fromJson(decoded);
    }
    throw const ApiException(ApiErrorKind.badResponse, 'too many redirects');
  }

  void close() => _client.close();
}
