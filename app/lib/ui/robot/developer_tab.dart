import 'dart:convert';

import 'package:flutter/material.dart';

import '../../core/json_http.dart';
import '../../l10n/app_localizations.dart';
import '../../models/robot_models.dart';
import '../../state/app_state.dart';
import '../../state/robot_session.dart';
import '../common.dart';

/// Developer/admin tools (dev flavor only). Every call here also needs the admin key, which the
/// robot checks server-side (docs/robot_api.md, "Developer / admin").
class DeveloperTab extends StatefulWidget {
  const DeveloperTab({super.key, required this.session});

  final RobotSession session;

  @override
  State<DeveloperTab> createState() => _DeveloperTabState();
}

class _DeveloperTabState extends State<DeveloperTab> {
  List<AxisLimit> _limits = HeadAxis.values.map(AxisLimit.defaults).toList();
  List<double> _speeds = const [40, 40, 40];
  final Map<HeadAxis, double> _drag = {};
  Diagnostics? _diag;
  String? _loadError;

  final TextEditingController _keyCtrl = TextEditingController();
  final TextEditingController _path = TextEditingController(text: '/api/status');
  final TextEditingController _params = TextEditingController();
  bool _rawPost = false;
  String _rawOut = '';

  RobotSession get s => widget.session;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) => _load());
  }

  @override
  void dispose() {
    _keyCtrl.dispose();
    _path.dispose();
    _params.dispose();
    super.dispose();
  }

  Future<void> _load() async {
    final api = s.api;
    if (api == null || !api.hasAdminKey) return;
    try {
      final limits = await api.limits();
      final speeds = await api.speeds();
      if (!mounted) return;
      setState(() {
        _limits = limits;
        _speeds = speeds;
        _loadError = null;
      });
    } on ApiException catch (e) {
      if (mounted) setState(() => _loadError = errorText(AppLocalizations.of(context), e));
    }
  }

  Future<void> _do(Future<void> Function() action) async {
    final err = await s.run((_) => action());
    if (mounted) showError(context, err);
  }

  Future<void> _saveKey(String? key) async {
    final app = AppScope.read(context);
    await app.setAdminKey(key);
    s.rebuildApi();
    _keyCtrl.clear();
    await _load();
  }

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context);
    final app = AppScope.of(context);
    final hasKey = app.adminKey != null;
    return ListenableBuilder(
      listenable: s,
      builder: (context, _) => ListView(padding: const EdgeInsets.symmetric(vertical: 8), children: [
        _keyCard(l, hasKey),
        if (_loadError != null)
          Padding(padding: const EdgeInsets.symmetric(horizontal: 24), child: Text(_loadError!)),
        if (hasKey && s.connection == Connection.online) ...[
          _servoCard(l),
          _limitsCard(l),
          _speedCard(l),
          _trackingCard(l),
          _diagCard(l),
          _consoleCard(l),
        ],
        const SizedBox(height: 24),
      ]),
    );
  }

  Widget _keyCard(AppLocalizations l, bool hasKey) {
    return SectionCard(
      title: l.devAdminKey,
      child: hasKey
          ? Row(children: [
              const Icon(Icons.verified_user_outlined),
              const SizedBox(width: 8),
              Expanded(child: Text(l.devAdminKeySet)),
              TextButton(onPressed: () => _saveKey(null), child: Text(l.devClear)),
            ])
          : Row(children: [
              Expanded(
                child: TextField(
                  controller: _keyCtrl,
                  obscureText: true,
                  autocorrect: false,
                  enableSuggestions: false,
                  decoration: InputDecoration(hintText: l.devAdminKeyHint),
                ),
              ),
              const SizedBox(width: 8),
              FilledButton(onPressed: () => _saveKey(_keyCtrl.text), child: Text(l.save)),
            ]),
    );
  }

  Widget _servoCard(AppLocalizations l) {
    return SectionCard(
      title: l.devServos,
      trailing: TextButton(onPressed: () => _do(() => s.api!.center()), child: Text(l.headCenter)),
      child: Column(children: [
        for (final axis in HeadAxis.values) _servoSlider(l, axis),
      ]),
    );
  }

  Widget _servoSlider(AppLocalizations l, HeadAxis axis) {
    final lim = _limits[axis.index];
    final value = (_drag[axis] ?? s.status?.angle(axis) ?? lim.center).clamp(lim.min, lim.max);
    return Row(children: [
      SizedBox(width: 72, child: Text(_axisLabel(l, axis))),
      Expanded(
        child: Slider(
          value: value,
          min: lim.min,
          max: lim.max,
          onChanged: (v) => setState(() => _drag[axis] = v),
          onChangeEnd: (v) {
            _do(() => s.api!.setAngle(axis, v));
            // Keep showing the dragged value until the next poll reports the move.
            Future<void>.delayed(const Duration(seconds: 3), () {
              if (mounted) setState(() => _drag.remove(axis));
            });
          },
        ),
      ),
      SizedBox(width: 48, child: Text('${value.toStringAsFixed(0)}°', textAlign: TextAlign.end)),
    ]);
  }

  Widget _limitsCard(AppLocalizations l) {
    return SectionCard(
      title: l.devLimits,
      child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        Text(l.devLimitsHint, style: Theme.of(context).textTheme.bodySmall),
        for (final axis in HeadAxis.values) ...[
          const SizedBox(height: 12),
          Text('${_axisLabel(l, axis)}  ${_limits[axis.index].min.round()}° – ${_limits[axis.index].max.round()}°, '
              '${l.devCenter} ${_limits[axis.index].center.round()}°'),
          RangeSlider(
            values: RangeValues(_limits[axis.index].min, _limits[axis.index].max),
            max: 180,
            divisions: 180,
            onChanged: (r) => setState(() {
              final cur = _limits[axis.index];
              _limits[axis.index] = cur.copyWith(min: r.start, max: r.end, center: cur.center.clamp(r.start, r.end));
            }),
          ),
          Slider(
            value: _limits[axis.index].center,
            min: _limits[axis.index].min,
            max: _limits[axis.index].max,
            onChanged: (v) => setState(() => _limits[axis.index] = _limits[axis.index].copyWith(center: v)),
          ),
        ],
        const SizedBox(height: 8),
        Row(mainAxisAlignment: MainAxisAlignment.end, children: [
          TextButton(onPressed: _load, child: Text(l.devReload)),
          const SizedBox(width: 8),
          OutlinedButton(onPressed: () => _applyLimits(save: false), child: Text(l.devApply)),
          const SizedBox(width: 8),
          FilledButton(onPressed: () => _applyLimits(save: true), child: Text(l.save)),
        ]),
      ]),
    );
  }

  Future<void> _applyLimits({required bool save}) async {
    for (final lim in _limits) {
      final err = await s.run((api) => api.setLimit(lim, save: save));
      if (err != null) {
        if (mounted) showError(context, err);
        return;
      }
    }
    if (mounted) showInfo(context, AppLocalizations.of(context).devSaved);
  }

  Widget _speedCard(AppLocalizations l) {
    return SectionCard(
      title: l.devSpeeds,
      child: Column(children: [
        for (final axis in HeadAxis.values)
          Row(children: [
            SizedBox(width: 72, child: Text(_axisLabel(l, axis))),
            Expanded(
              child: Slider(
                value: _speeds[axis.index].clamp(10, 120),
                min: 10,
                max: 120,
                divisions: 22,
                label: '${_speeds[axis.index].round()}°/s',
                onChanged: (v) => setState(() => _speeds = [..._speeds]..[axis.index] = v),
                onChangeEnd: (v) async {
                  final err = await s.run((api) async => _speeds = await api.setSpeed(axis, v));
                  if (mounted) {
                    setState(() {});
                    showError(context, err);
                  }
                },
              ),
            ),
            IconButton(tooltip: l.devTest, onPressed: () => _do(() => s.api!.testAxis(axis)), icon: const Icon(Icons.play_arrow)),
          ]),
        Align(
          alignment: Alignment.centerRight,
          child: FilledButton(onPressed: () => _do(() => s.api!.saveSpeeds()), child: Text(l.save)),
        ),
      ]),
    );
  }

  Widget _trackingCard(AppLocalizations l) {
    return SectionCard(
      title: l.devTracking,
      child: Wrap(spacing: 8, runSpacing: 8, children: [
        for (final axis in HeadAxis.values)
          OutlinedButton(onPressed: () => _do(() => s.api!.flipTracking(axis)), child: Text('${l.devFlip} ${_axisLabel(l, axis)}')),
        TextButton(onPressed: () => _do(() => s.api!.resetTrackingDirs()), child: Text(l.devResetDirs)),
      ]),
    );
  }

  Widget _diagCard(AppLocalizations l) {
    final d = _diag;
    return SectionCard(
      title: l.devDiagnostics,
      trailing: IconButton(
        icon: const Icon(Icons.refresh),
        onPressed: () async {
          final api = s.api;
          if (api == null) return;
          try {
            final r = await api.diagnostics();
            if (mounted) setState(() => _diag = r);
          } on ApiException catch (e) {
            if (mounted) showError(context, e);
          }
        },
      ),
      child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        if (d == null) Text(l.devDiagHint),
        for (final e in d?.values.entries ?? const <MapEntry<String, String>>[])
          Row(children: [Expanded(child: Text(e.key)), Text(e.value)]),
        const SizedBox(height: 8),
        Align(
          alignment: Alignment.centerRight,
          child: OutlinedButton.icon(
            icon: const Icon(Icons.restart_alt),
            label: Text(l.devReboot),
            onPressed: () async {
              final ok = await confirm(context, title: l.devReboot, body: l.devRebootConfirm, danger: true);
              if (ok) await _do(() => s.api!.reboot());
            },
          ),
        ),
      ]),
    );
  }

  Widget _consoleCard(AppLocalizations l) {
    return SectionCard(
      title: l.devConsole,
      child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        Row(children: [
          SegmentedButton<bool>(
            segments: const [ButtonSegment(value: false, label: Text('GET')), ButtonSegment(value: true, label: Text('POST'))],
            selected: {_rawPost},
            onSelectionChanged: (v) => setState(() => _rawPost = v.first),
          ),
          const SizedBox(width: 8),
          Expanded(child: TextField(controller: _path, decoration: const InputDecoration(isDense: true))),
        ]),
        const SizedBox(height: 8),
        TextField(controller: _params, decoration: const InputDecoration(isDense: true, hintText: 'key=value&key2=value2')),
        const SizedBox(height: 8),
        FilledButton(onPressed: _sendRaw, child: Text(l.send)),
        if (_rawOut.isNotEmpty) ...[
          const SizedBox(height: 8),
          Container(
            padding: const EdgeInsets.all(8),
            color: Colors.black26,
            child: SelectableText(_rawOut, style: const TextStyle(fontFamily: 'monospace', fontSize: 12)),
          ),
        ],
      ]),
    );
  }

  Future<void> _sendRaw() async {
    final api = s.api;
    if (api == null) return;
    final Map<String, String> params;
    try {
      params = _params.text.trim().isEmpty ? const {} : Uri.splitQueryString(_params.text.trim());
    } on FormatException {
      setState(() => _rawOut = 'bad params');
      return;
    }
    try {
      final r = await api.raw(post: _rawPost, path: _path.text.trim(), params: params);
      setState(() => _rawOut = const JsonEncoder.withIndent('  ').convert(r));
    } on ApiException catch (e) {
      setState(() => _rawOut = '${e.kind.name}${e.message.isEmpty ? '' : ': ${e.message}'}');
    }
  }

  static String _axisLabel(AppLocalizations l, HeadAxis a) => switch (a) {
        HeadAxis.pitch => l.axisPitch,
        HeadAxis.roll => l.axisRoll,
        HeadAxis.yaw => l.axisYaw,
      };
}
