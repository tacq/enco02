import 'package:flutter/material.dart';

import '../l10n/app_localizations.dart';
import '../models/saved_robot.dart';
import '../state/app_state.dart';
import 'onboarding/scan_page.dart';
import 'robot/robot_page.dart';

class HomePage extends StatelessWidget {
  const HomePage({super.key});

  @override
  Widget build(BuildContext context) {
    final state = AppScope.of(context);
    final l = AppLocalizations.of(context);
    final robots = state.robots;
    return Scaffold(
      appBar: AppBar(
        title: Text(state.config.isDev ? '${l.appTitle} · DEV' : l.appTitle),
      ),
      body: robots.isEmpty ? const _Welcome() : _RobotList(robots: robots),
      floatingActionButton: robots.isEmpty
          ? null
          : FloatingActionButton.extended(
              onPressed: () => _addRobot(context),
              icon: const Icon(Icons.add),
              label: Text(l.addRobot),
            ),
    );
  }
}

void _addRobot(BuildContext context) {
  Navigator.of(context).push(MaterialPageRoute<void>(builder: (_) => const ScanPage()));
}

class _Welcome extends StatelessWidget {
  const _Welcome();

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context);
    final theme = Theme.of(context);
    return SafeArea(
      child: Padding(
        padding: const EdgeInsets.all(32),
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Icon(Icons.smart_toy_outlined, size: 120, color: theme.colorScheme.primary),
            const SizedBox(height: 24),
            Text(l.welcomeTitle, textAlign: TextAlign.center, style: theme.textTheme.headlineSmall),
            const SizedBox(height: 12),
            Text(l.welcomeBody, textAlign: TextAlign.center, style: theme.textTheme.bodyMedium),
            const SizedBox(height: 40),
            FilledButton.icon(
              onPressed: () => _addRobot(context),
              icon: const Icon(Icons.qr_code_scanner),
              label: Text(l.addRobot),
            ),
          ],
        ),
      ),
    );
  }
}

class _RobotList extends StatelessWidget {
  const _RobotList({required this.robots});

  final List<SavedRobot> robots;

  @override
  Widget build(BuildContext context) {
    return ListView(
      padding: const EdgeInsets.symmetric(vertical: 8),
      children: [
        for (final r in robots)
          Card(
            margin: const EdgeInsets.symmetric(horizontal: 16, vertical: 6),
            child: ListTile(
              leading: const CircleAvatar(child: Icon(Icons.smart_toy)),
              title: Text(r.name),
              subtitle: Text(r.id),
              trailing: const Icon(Icons.chevron_right),
              onTap: () => Navigator.of(context).push(
                MaterialPageRoute<void>(builder: (_) => RobotPage(robotId: r.id)),
              ),
            ),
          ),
      ],
    );
  }
}
