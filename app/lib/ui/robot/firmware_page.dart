import 'dart:async';

import 'package:flutter/material.dart';

import '../../app_config.dart';
import '../../core/firmware.dart';
import '../../core/json_http.dart';
import '../../l10n/app_localizations.dart';
import '../../models/robot_models.dart';
import '../../state/robot_session.dart';
import '../common.dart';

/// An update the user can approve, found by the app (manifest) and/or by the robot itself.
class _Update {
  _Update({required this.target, required this.version, required this.notes, this.component});

  final String target;
  final String version;
  final Map<String, String> notes;

  /// Present when the app found it in the manifest; sent along so the robot installs exactly this.
  final FirmwareComponent? component;
}

/// Firmware updates. The robot downloads and installs; the app discovers, asks, and shows progress.
/// See docs/firmware_updates.md.
class FirmwarePage extends StatefulWidget {
  const FirmwarePage({super.key, required this.session});

  final RobotSession session;

  @override
  State<FirmwarePage> createState() => _FirmwarePageState();
}

class _FirmwarePageState extends State<FirmwarePage> {
  final FirmwareService _service = FirmwareService();
  FirmwareManifest? _manifest;
  bool _checking = false;
  bool _checkedOnce = false;
  String? _manifestError;

  RobotSession get s => widget.session;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) _check();
    });
  }

  @override
  void dispose() {
    _service.close();
    super.dispose();
  }

  Future<void> _check() async {
    final l = AppLocalizations.of(context);
    setState(() {
      _checking = true;
      _manifestError = null;
    });
    // Ask the robot to look too; it reports through /api/ota/status.
    unawaited(s.run((api) => api.otaCheck()));
    try {
      _manifest = await _service.fetchManifest(Uri.parse(AppConfig.manifestUrl));
    } on ApiException catch (e) {
      _manifestError = e.kind == ApiErrorKind.notFound ? l.noReleasePublished : errorText(l, e);
    }
    await Future<void>.delayed(const Duration(seconds: 2));
    await s.refreshOta();
    if (mounted) {
      setState(() {
        _checking = false;
        _checkedOnce = true;
      });
    }
  }

  List<_Update> _updates() {
    final installed = s.info?.firmware ?? const FirmwareVersions();
    final byTarget = <String, _Update>{};
    for (final o in s.ota.available) {
      byTarget[o.target] = _Update(target: o.target, version: o.version, notes: o.notes);
    }
    for (final c in _manifest?.updatesFor(installed) ?? const <FirmwareComponent>[]) {
      final existing = byTarget[c.target];
      if (existing == null || compareVersions(c.version, existing.version) >= 0) {
        byTarget[c.target] = _Update(target: c.target, version: c.version, notes: c.notes, component: c);
      }
    }
    return byTarget.values.toList()..sort((a, b) => a.target.compareTo(b.target));
  }

  Future<void> _install(_Update u) async {
    final l = AppLocalizations.of(context);
    final ok = await confirm(
      context,
      title: l.installTitle(_board(l, u.target), u.version),
      body: l.installConfirm,
      ok: l.install,
    );
    if (!ok || !mounted) return;
    final err = await s.run((api) => api.otaStart(u.target, component: u.component));
    if (!mounted) return;
    if (err != null) {
      showError(context, err);
      return;
    }
    await s.refreshOta();
  }

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context);
    final lang = Localizations.localeOf(context).languageCode;
    return Scaffold(
      appBar: AppBar(title: Text(l.firmwareUpdate)),
      body: ListenableBuilder(
        listenable: s,
        builder: (context, _) {
          final fw = s.info?.firmware;
          final ota = s.ota;
          final updates = _updates();
          return ListView(padding: const EdgeInsets.symmetric(vertical: 8), children: [
            SectionCard(
              title: l.firmwareInstalled,
              child: Column(children: [
                _versionRow(l.boardMain, fw?.main),
                _versionRow(l.boardCam, fw?.cam),
              ]),
            ),
            if (ota.state != OtaState.idle && ota.state != OtaState.available) _progressCard(l, ota),
            SectionCard(
              title: l.firmwareAvailable,
              trailing: IconButton(
                tooltip: l.checkForUpdates,
                onPressed: _checking || ota.busy ? null : _check,
                icon: const Icon(Icons.refresh),
              ),
              child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
                if (_checking) const LinearProgressIndicator(),
                if (!_checking && _checkedOnce && updates.isEmpty) Text(l.upToDate),
                if (_manifestError != null && updates.isEmpty)
                  Padding(
                    padding: const EdgeInsets.only(top: 8),
                    child: Text(_manifestError!, style: Theme.of(context).textTheme.bodySmall),
                  ),
                for (final u in updates) ...[
                  const SizedBox(height: 8),
                  Text('${_board(l, u.target)} → ${u.version}', style: Theme.of(context).textTheme.titleSmall),
                  if ((u.notes[lang] ?? u.notes['en'] ?? '').isNotEmpty)
                    Padding(padding: const EdgeInsets.symmetric(vertical: 4), child: Text(u.notes[lang] ?? u.notes['en']!)),
                  Align(
                    alignment: Alignment.centerRight,
                    child: FilledButton.icon(
                      onPressed: ota.busy ? null : () => _install(u),
                      icon: const Icon(Icons.download),
                      label: Text(l.install),
                    ),
                  ),
                ],
              ]),
            ),
            Padding(
              padding: const EdgeInsets.all(16),
              child: Text(l.firmwareHowItWorks, style: Theme.of(context).textTheme.bodySmall),
            ),
          ]);
        },
      ),
    );
  }

  Widget _versionRow(String board, String? v) => ListTile(
        contentPadding: EdgeInsets.zero,
        dense: true,
        title: Text(board),
        trailing: Text(v ?? '-'),
      );

  Widget _progressCard(AppLocalizations l, OtaStatus ota) {
    final label = switch (ota.state) {
      OtaState.checking => l.otaChecking,
      OtaState.downloading => l.otaDownloading,
      OtaState.verifying => l.otaVerifying,
      OtaState.installing => l.otaInstalling,
      OtaState.done => l.otaDone,
      OtaState.failed => l.otaFailed,
      _ => '',
    };
    return SectionCard(
      title: label,
      child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        if (ota.busy) LinearProgressIndicator(value: ota.state == OtaState.downloading ? ota.progress / 100 : null),
        if (ota.busy) Padding(padding: const EdgeInsets.only(top: 8), child: Text(l.otaKeepPowered)),
        if (ota.state == OtaState.failed && ota.error != null) Text(ota.error!),
      ]),
    );
  }

  static String _board(AppLocalizations l, String target) => target == 'cam' ? l.boardCam : l.boardMain;
}
