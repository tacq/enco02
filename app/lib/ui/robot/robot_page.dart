import 'package:flutter/material.dart';

import '../../l10n/app_localizations.dart';
import '../../state/app_state.dart';
import '../../state/robot_session.dart';
import 'control_tab.dart';
import 'developer_tab.dart';
import 'settings_tab.dart';

/// One paired robot: Control / (Developer) / Settings.
class RobotPage extends StatefulWidget {
  const RobotPage({super.key, required this.robotId});

  final String robotId;

  @override
  State<RobotPage> createState() => _RobotPageState();
}

class _RobotPageState extends State<RobotPage> {
  RobotSession? _session;
  int _tab = 0;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    if (_session != null) return;
    final app = AppScope.read(context);
    final robot = app.robot(widget.robotId);
    if (robot == null) return;
    _session = RobotSession(app: app, robot: robot)..start();
  }

  @override
  void dispose() {
    _session?.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context);
    final session = _session;
    if (session == null) {
      return Scaffold(appBar: AppBar(), body: Center(child: Text(l.errGeneric)));
    }
    final isDev = AppScope.of(context).config.isDev;
    final tabs = <(String, IconData, Widget)>[
      (l.tabControl, Icons.gamepad_outlined, ControlTab(session: session)),
      if (isDev) (l.tabDeveloper, Icons.build_outlined, DeveloperTab(session: session)),
      (l.tabSettings, Icons.settings_outlined, SettingsTab(session: session)),
    ];
    final index = _tab.clamp(0, tabs.length - 1);
    return ListenableBuilder(
      listenable: session,
      builder: (context, _) => Scaffold(
        appBar: AppBar(
          title: Text(session.robot.name),
          actions: [_ConnectionChip(connection: session.connection), const SizedBox(width: 12)],
        ),
        body: _Body(session: session, child: tabs[index].$3),
        bottomNavigationBar: NavigationBar(
          selectedIndex: index,
          onDestinationSelected: (i) => setState(() => _tab = i),
          destinations: [for (final t in tabs) NavigationDestination(icon: Icon(t.$2), label: t.$1)],
        ),
      ),
    );
  }
}

/// Shows an explanation instead of the tab when the robot can't be used.
class _Body extends StatelessWidget {
  const _Body({required this.session, required this.child});

  final RobotSession session;
  final Widget child;

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context);
    final banner = switch (session.connection) {
      Connection.offline => l.offlineBody,
      Connection.unauthorized => l.unauthorizedBody,
      _ => null,
    };
    if (banner == null) return child;
    return Column(children: [
      MaterialBanner(
        content: Text(banner),
        leading: const Icon(Icons.info_outline),
        actions: [TextButton(onPressed: session.start, child: Text(l.retry))],
      ),
      Expanded(child: child),
    ]);
  }
}

class _ConnectionChip extends StatelessWidget {
  const _ConnectionChip({required this.connection});

  final Connection connection;

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context);
    final (label, color) = switch (connection) {
      Connection.online => (l.online, Colors.greenAccent),
      Connection.connecting => (l.connecting, Colors.amberAccent),
      Connection.offline => (l.offline, Colors.redAccent),
      Connection.unauthorized => (l.offline, Colors.redAccent),
    };
    return Row(mainAxisSize: MainAxisSize.min, children: [
      Icon(Icons.circle, size: 10, color: color),
      const SizedBox(width: 6),
      Text(label, style: Theme.of(context).textTheme.labelMedium),
    ]);
  }
}
