import 'package:flutter/material.dart';

import '../../core/validators.dart';
import '../../l10n/app_localizations.dart';
import '../../state/app_state.dart';
import '../../state/robot_session.dart';
import '../common.dart';
import '../onboarding/scan_page.dart';
import 'firmware_page.dart';

class SettingsTab extends StatelessWidget {
  const SettingsTab({super.key, required this.session});

  final RobotSession session;

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context);
    final app = AppScope.of(context);
    return ListenableBuilder(
      listenable: session,
      builder: (context, _) {
        final online = session.connection == Connection.online;
        final fw = session.info?.firmware;
        return ListView(children: [
          _header(context, l.settingsRobot),
          ListTile(
            leading: const Icon(Icons.badge_outlined),
            title: Text(l.robotName),
            subtitle: Text(session.robot.name),
            trailing: const Icon(Icons.edit),
            enabled: online,
            onTap: () => _rename(context),
          ),
          ListTile(leading: const Icon(Icons.fingerprint), title: Text(l.deviceId), subtitle: Text(session.robot.id)),
          ListTile(
            leading: const Icon(Icons.memory),
            title: Text(l.firmwareVersions),
            subtitle: Text('${l.boardMain}: ${fw?.main ?? '-'}   ${l.boardCam}: ${fw?.cam ?? '-'}'),
          ),
          ListTile(
            leading: Badge(isLabelVisible: session.ota.available.isNotEmpty, child: const Icon(Icons.system_update)),
            title: Text(l.firmwareUpdate),
            trailing: const Icon(Icons.chevron_right),
            enabled: online,
            onTap: () => Navigator.of(context).push(MaterialPageRoute<void>(builder: (_) => FirmwarePage(session: session))),
          ),
          _header(context, l.settingsNetwork),
          ListTile(
            leading: const Icon(Icons.wifi),
            title: Text(l.changeWifi),
            subtitle: Text(l.changeWifiHint),
            enabled: online,
            onTap: () => _changeWifi(context),
          ),
          ListTile(
            leading: Icon(Icons.link_off, color: Theme.of(context).colorScheme.error),
            title: Text(l.unpair),
            subtitle: Text(l.unpairHint),
            onTap: () => _unpair(context),
          ),
          _header(context, l.settingsApp),
          ListTile(
            leading: const Icon(Icons.language),
            title: Text(l.language),
            trailing: DropdownButton<String>(
              value: app.locale?.languageCode ?? 'system',
              underline: const SizedBox.shrink(),
              items: [
                DropdownMenuItem(value: 'system', child: Text(l.languageSystem)),
                const DropdownMenuItem(value: 'en', child: Text('English')),
                const DropdownMenuItem(value: 'zh', child: Text('简体中文')),
              ],
              onChanged: (v) => app.setLocale(v == null || v == 'system' ? null : Locale(v)),
            ),
          ),
          ListTile(
            leading: const Icon(Icons.info_outline),
            title: Text(l.about),
            subtitle: Text('${l.appTitle} 0.1.0 · ${app.config.isDev ? 'dev' : 'user'}'),
          ),
        ]);
      },
    );
  }

  Widget _header(BuildContext context, String text) => Padding(
        padding: const EdgeInsets.fromLTRB(16, 20, 16, 4),
        child: Text(text, style: Theme.of(context).textTheme.labelLarge?.copyWith(color: Theme.of(context).colorScheme.primary)),
      );

  Future<void> _rename(BuildContext context) async {
    final l = AppLocalizations.of(context);
    final ctrl = TextEditingController(text: session.robot.name);
    final form = GlobalKey<FormState>();
    final name = await showDialog<String>(
      context: context,
      builder: (c) => AlertDialog(
        title: Text(l.robotName),
        content: Form(
          key: form,
          child: TextFormField(
            controller: ctrl,
            autofocus: true,
            validator: (v) => Validators.robotName(v ?? '') ? null : l.robotNameInvalid,
          ),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(c), child: Text(l.cancel)),
          FilledButton(
            onPressed: () {
              if (form.currentState!.validate()) Navigator.pop(c, ctrl.text.trim());
            },
            child: Text(l.save),
          ),
        ],
      ),
    );
    ctrl.dispose();
    if (name == null || !context.mounted) return;
    final err = await session.run((api) => api.rename(name));
    if (err == null) {
      await session.applyRename(name);
    } else if (context.mounted) {
      showError(context, err);
    }
  }

  Future<void> _changeWifi(BuildContext context) async {
    final l = AppLocalizations.of(context);
    final ok = await confirm(context, title: l.changeWifi, body: l.changeWifiConfirm, ok: l.continueLabel);
    if (!ok || !context.mounted) return;
    final app = AppScope.read(context);
    final err = await session.run((api) => api.resetWifi());
    if (!context.mounted) return;
    if (err != null) {
      showError(context, err);
      return;
    }
    // The old token is gone on the robot; pairing again issues a new one.
    await app.removeRobot(session.robot.id);
    if (!context.mounted) return;
    final nav = Navigator.of(context);
    nav.popUntil((r) => r.isFirst);
    nav.push(MaterialPageRoute<void>(builder: (_) => const ScanPage()));
  }

  Future<void> _unpair(BuildContext context) async {
    final l = AppLocalizations.of(context);
    final ok = await confirm(context, title: l.unpair, body: l.unpairConfirm, ok: l.unpair, danger: true);
    if (!ok || !context.mounted) return;
    final app = AppScope.read(context);
    // Best effort: forget locally even if the robot is unreachable.
    await session.run((api) => api.unpair());
    await app.removeRobot(session.robot.id);
    if (context.mounted) Navigator.of(context).popUntil((r) => r.isFirst);
  }
}
