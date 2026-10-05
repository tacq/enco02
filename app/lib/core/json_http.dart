import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:http/http.dart' as http;

enum ApiErrorKind { network, timeout, unauthorized, forbidden, notFound, busy, unavailable, badRequest, badResponse }

/// Error surfaced to the UI. `message` is the robot's short reason (never contains secrets); the UI
/// maps [kind] to a localized, generic text.
class ApiException implements Exception {
  const ApiException(this.kind, [this.message = '']);

  final ApiErrorKind kind;
  final String message;

  @override
  String toString() => 'ApiException($kind${message.isEmpty ? '' : ': $message'})';
}

/// Small JSON-over-HTTP helper shared by the pairing and robot clients.
///
/// TODO(security): this is plain HTTP on the LAN / robot SoftAP. TLS on an ESP32 without PSRAM is
/// not affordable (≈40 KB heap per session). Mitigations: the SoftAP is WPA2 with a per-session
/// password from the QR code, LAN requests carry an unguessable per-pairing bearer token, and the
/// host is restricted to private IPv4 ranges (Validators.lanHost).
class JsonHttp {
  JsonHttp({required this.host, required this.port, http.Client? client, this.timeout = const Duration(seconds: 5)})
      : _client = client ?? http.Client();

  final String host;
  final int port;
  final Duration timeout;
  final http.Client _client;

  /// Robot responses are tiny; anything larger is a sign of the wrong device.
  static const int maxBodyBytes = 64 * 1024;

  Uri uri(String path, [Map<String, String>? query]) =>
      Uri(scheme: 'http', host: host, port: port, path: path, queryParameters: (query?.isEmpty ?? true) ? null : query);

  Future<Map<String, dynamic>> get(String path, {Map<String, String>? query, Map<String, String>? headers, Duration? timeout}) {
    return _send(() => _client.get(uri(path, query), headers: headers), timeout);
  }

  Future<Map<String, dynamic>> post(String path, {Map<String, String>? form, Map<String, String>? headers, Duration? timeout}) {
    return _send(() => _client.post(uri(path), headers: headers, body: form ?? const <String, String>{}), timeout);
  }

  Future<Map<String, dynamic>> _send(Future<http.Response> Function() call, Duration? t) async {
    final http.Response res;
    try {
      res = await call().timeout(t ?? timeout);
    } on TimeoutException {
      throw const ApiException(ApiErrorKind.timeout);
    } on SocketException {
      throw const ApiException(ApiErrorKind.network);
    } on http.ClientException {
      throw const ApiException(ApiErrorKind.network);
    }
    if (res.bodyBytes.length > maxBodyBytes) {
      throw const ApiException(ApiErrorKind.badResponse, 'response too large');
    }
    Map<String, dynamic> body = const {};
    if (res.bodyBytes.isNotEmpty) {
      try {
        final decoded = jsonDecode(utf8.decode(res.bodyBytes, allowMalformed: true));
        if (decoded is Map<String, dynamic>) body = decoded;
      } on FormatException {
        if (res.statusCode >= 200 && res.statusCode < 300) {
          throw const ApiException(ApiErrorKind.badResponse, 'not JSON');
        }
      }
    }
    if (res.statusCode >= 200 && res.statusCode < 300) return body;
    final reason = _shortReason(body['error']);
    switch (res.statusCode) {
      case 400:
        throw ApiException(ApiErrorKind.badRequest, reason);
      case 401:
        throw ApiException(ApiErrorKind.unauthorized, reason);
      case 403:
        throw ApiException(ApiErrorKind.forbidden, reason);
      case 404:
        throw ApiException(ApiErrorKind.notFound, reason);
      case 409:
        throw ApiException(ApiErrorKind.busy, reason);
      case 503:
        throw ApiException(ApiErrorKind.unavailable, reason);
      default:
        throw ApiException(ApiErrorKind.badResponse, 'HTTP ${res.statusCode}');
    }
  }

  static String _shortReason(Object? v) {
    if (v is! String) return '';
    return v.length > 120 ? v.substring(0, 120) : v;
  }

  void close() => _client.close();
}

/// Typed accessors that tolerate missing fields but never accept the wrong type.
extension JsonRead on Map<String, dynamic> {
  String? str(String k) => this[k] is String ? this[k] as String : null;
  int? integer(String k) {
    final v = this[k];
    if (v is int) return v;
    if (v is double && v.isFinite) return v.round();
    return null;
  }

  double? number(String k) {
    final v = this[k];
    if (v is num && v.isFinite) return v.toDouble();
    return null;
  }

  bool? boolean(String k) => this[k] is bool ? this[k] as bool : null;
  Map<String, dynamic>? obj(String k) => this[k] is Map<String, dynamic> ? this[k] as Map<String, dynamic> : null;
  List<dynamic> list(String k) => this[k] is List ? this[k] as List : const [];
}
