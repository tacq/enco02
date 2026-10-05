import 'package:flutter/material.dart';

import '../../l10n/app_localizations.dart';
import '../../models/robot_models.dart';
import '../../state/robot_session.dart';
import '../common.dart';
import 'firmware_page.dart';

/// Everyday controls: face, head, features, volume, say.
class ControlTab extends StatefulWidget {
  const ControlTab({super.key, required this.session});

  final RobotSession session;

  @override
  State<ControlTab> createState() => _ControlTabState();
}

class _ControlTabState extends State<ControlTab> {
  final Map<String, (Object, DateTime)> _pending = {};
  final TextEditingController _say = TextEditingController();
  double? _volumeDrag;

  static const _step = 10.0;

  RobotSession get s => widget.session;

  @override
  void dispose() {
    _say.dispose();
    super.dispose();
  }

  /// Shows the value just set until the next status poll has had time to confirm it.
  T _value<T>(String key, T fromStatus) {
    final p = _pending[key];
    if (p != null && DateTime.now().difference(p.$2) < const Duration(seconds: 3) && p.$1 is T) return p.$1 as T;
    return fromStatus;
  }

  Future<void> _set<T extends Object>(String key, T value, Future<void> Function() send) async {
    setState(() => _pending[key] = (value, DateTime.now()));
    final err = await s.run((_) => send());
    if (err != null) {
      setState(() => _pending.remove(key));
      if (mounted) showError(context, err);
    }
  }

  Future<void> _do(Future<void> Function() action) async {
    final err = await s.run((_) => action());
    if (mounted) showError(context, err);
  }

  void _nudge(HeadAxis axis, double delta) {
    final current = s.status?.angle(axis) ?? axis.defaultCenter;
    final target = (current + delta).clamp(axis.defaultMin, axis.defaultMax);
    _do(() => s.api!.setAngle(axis, target));
  }

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context);
    return ListenableBuilder(
      listenable: s,
      builder: (context, _) {
        final st = s.status;
        final info = s.info;
        final enabled = s.connection == Connection.online && s.api != null;
        return AbsorbPointer(
          absorbing: !enabled,
          child: Opacity(
            opacity: enabled ? 1 : 0.5,
            child: ListView(padding: const EdgeInsets.symmetric(vertical: 8), children: [
              if (s.ota.available.isNotEmpty) _updateBanner(l),
              _faceCard(l, info, st),
              _headCard(l, st),
              _featuresCard(l, st),
              _volumeCard(l, st),
              _sayCard(l),
              const SizedBox(height: 24),
            ]),
          ),
        );
      },
    );
  }

  Widget _updateBanner(AppLocalizations l) {
    final offer = s.ota.available.first;
    return Card(
      margin: const EdgeInsets.fromLTRB(16, 8, 16, 8),
      color: Theme.of(context).colorScheme.primaryContainer,
      child: ListTile(
        leading: const Icon(Icons.system_update),
        title: Text(l.updateAvailableShort(offer.version)),
        trailing: const Icon(Icons.chevron_right),
        onTap: () => Navigator.of(context).push(MaterialPageRoute<void>(builder: (_) => FirmwarePage(session: s))),
      ),
    );
  }

  Widget _faceCard(AppLocalizations l, RobotInfo? info, RobotStatus? st) {
    final characters = info?.characters ?? RobotInfo.defaultCharacters;
    final expressions = info?.expressions ?? RobotInfo.defaultExpressions;
    final current = _value<String>('character', st?.character ?? characters.first);
    return SectionCard(
      title: l.faceTitle,
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        if (characters.length > 1)
          SegmentedButton<String>(
            segments: [for (final c in characters) ButtonSegment(value: c, label: Text(_characterLabel(l, c)))],
            selected: {characters.contains(current) ? current : characters.first},
            onSelectionChanged: (v) => _set('character', v.first, () => s.api!.setCharacter(v.first)),
          ),
        const SizedBox(height: 12),
        Wrap(spacing: 8, runSpacing: 8, children: [
          for (final e in expressions)
            ActionChip(label: Text(_expressionLabel(l, e)), onPressed: () => _do(() => s.api!.setExpression(e))),
        ]),
      ]),
    );
  }

  Widget _headCard(AppLocalizations l, RobotStatus? st) {
    final anims = s.animations?.anims ?? const <AnimationInfo>[];
    final auto = _value<bool>('head_auto', st?.headAuto ?? s.animations?.auto ?? false);
    Widget pad(IconData icon, String tip, VoidCallback onTap) =>
        IconButton.filledTonal(iconSize: 32, tooltip: tip, onPressed: onTap, icon: Icon(icon));
    return SectionCard(
      title: l.headTitle,
      child: Column(children: [
        Row(mainAxisAlignment: MainAxisAlignment.spaceEvenly, children: [
          pad(Icons.rotate_left, l.headTiltLeft, () => _nudge(HeadAxis.roll, -_step)),
          Column(children: [
            pad(Icons.keyboard_arrow_up, l.headUp, () => _nudge(HeadAxis.pitch, -_step)),
            Row(children: [
              pad(Icons.keyboard_arrow_left, l.headLeft, () => _nudge(HeadAxis.yaw, -_step)),
              const SizedBox(width: 8),
              pad(Icons.center_focus_strong, l.headCenter, () => _do(() => s.api!.center())),
              const SizedBox(width: 8),
              pad(Icons.keyboard_arrow_right, l.headRight, () => _nudge(HeadAxis.yaw, _step)),
            ]),
            pad(Icons.keyboard_arrow_down, l.headDown, () => _nudge(HeadAxis.pitch, _step)),
          ]),
          pad(Icons.rotate_right, l.headTiltRight, () => _nudge(HeadAxis.roll, _step)),
        ]),
        const SizedBox(height: 16),
        Wrap(spacing: 8, runSpacing: 8, children: [
          for (final a in anims) ActionChip(label: Text(a.label), onPressed: () => _do(() => s.api!.playAnimation(a.name))),
          ActionChip(
            avatar: const Icon(Icons.shuffle, size: 18),
            label: Text(l.headRandom),
            onPressed: () => _do(() => s.api!.playRandomAnimation()),
          ),
        ]),
        SwitchListTile(
          contentPadding: EdgeInsets.zero,
          title: Text(l.headAuto),
          subtitle: Text(l.headAutoHint),
          value: auto,
          onChanged: (v) => _set('head_auto', v, () => s.api!.setHeadAuto(v)),
        ),
      ]),
    );
  }

  Widget _featuresCard(AppLocalizations l, RobotStatus? st) {
    final tracking = _value<bool>('tracking', st?.tracking ?? false);
    final caption = _value<bool>('caption', st?.caption ?? true);
    final mode = _value<String>('ui_mode', st?.uiMode ?? 'face');
    return SectionCard(
      title: l.featuresTitle,
      child: Column(children: [
        SwitchListTile(
          contentPadding: EdgeInsets.zero,
          title: Text(l.tracking),
          subtitle: Text(st?.camPresent == false ? l.trackingNoCam : l.trackingHint),
          value: tracking,
          onChanged: st?.camPresent == false ? null : (v) => _set('tracking', v, () => s.api!.setTracking(v)),
        ),
        SwitchListTile(
          contentPadding: EdgeInsets.zero,
          title: Text(l.caption),
          value: caption,
          onChanged: (v) => _set('caption', v, () => s.api!.setCaption(v)),
        ),
        const SizedBox(height: 8),
        Row(children: [
          Expanded(child: Text(l.screenMode)),
          SegmentedButton<String>(
            segments: [
              ButtonSegment(value: 'face', label: Text(l.screenFace)),
              ButtonSegment(value: 'chat', label: Text(l.screenChat)),
            ],
            selected: {mode},
            onSelectionChanged: (v) => _set('ui_mode', v.first, () => s.api!.setUiMode(v.first)),
          ),
        ]),
      ]),
    );
  }

  Widget _volumeCard(AppLocalizations l, RobotStatus? st) {
    final volume = (_volumeDrag ?? _value<int>('volume', st?.volume ?? 50).toDouble()).clamp(0.0, 100.0);
    return SectionCard(
      title: l.volume,
      trailing: Text('${volume.round()}'),
      child: Row(children: [
        const Icon(Icons.volume_down),
        Expanded(
          child: Slider(
            value: volume,
            max: 100,
            divisions: 20,
            onChanged: (v) => setState(() => _volumeDrag = v),
            onChangeEnd: (v) {
              setState(() => _volumeDrag = null);
              _set('volume', v.round(), () => s.api!.setVolume(v.round()));
            },
          ),
        ),
        const Icon(Icons.volume_up),
      ]),
    );
  }

  Widget _sayCard(AppLocalizations l) {
    return SectionCard(
      title: l.sayTitle,
      child: Row(children: [
        Expanded(
          child: TextField(
            controller: _say,
            maxLength: 120,
            decoration: InputDecoration(hintText: l.sayHint, counterText: ''),
            onSubmitted: (_) => _sendSay(),
          ),
        ),
        const SizedBox(width: 8),
        IconButton.filled(onPressed: _sendSay, icon: const Icon(Icons.send), tooltip: l.send),
      ]),
    );
  }

  Future<void> _sendSay() async {
    final text = _say.text.trim();
    if (text.isEmpty) return;
    final err = await s.run((api) => api.say(text));
    if (!mounted) return;
    if (err == null) {
      _say.clear();
    } else {
      showError(context, err);
    }
  }

  static String _characterLabel(AppLocalizations l, String id) => switch (id) {
        'k3' => l.characterK3,
        'fox' => l.characterFox,
        _ => id,
      };

  static String _expressionLabel(AppLocalizations l, String id) => switch (id) {
        'neutral' => l.exprNeutral,
        'happy' => l.exprHappy,
        'sad' => l.exprSad,
        'angry' => l.exprAngry,
        'surprised' => l.exprSurprised,
        'shy' => l.exprShy,
        'pout' => l.exprPout,
        'wink' => l.exprWink,
        'thinking' => l.exprThinking,
        'sleepy' => l.exprSleepy,
        _ => id,
      };
}
