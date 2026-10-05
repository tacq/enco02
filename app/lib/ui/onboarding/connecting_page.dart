import 'dart:async';

import 'package:flutter/material.dart';

import '../../core/json_http.dart';
import '../../core/provisioning_client.dart';
import '../../l10n/app_localizations.dart';
import '../../models/saved_robot.dart';
import '../../state/app_state.dart';
import 'name_page.dart';
import 'pairing_flow.dart';

/// Step 4: wait for the robot to join the home Wi-Fi, collect its address + token, finish.
class ConnectingPage extends StatefulWidget {
  const ConnectingPage({super.key, required this.flow, required this.ssid});

  final PairingFlow flow;
  final String ssid;

  @override
  State<ConnectingPage> createState() => _ConnectingPageState();
}

class _ConnectingPageState extends State<ConnectingPage> {
  static const _timeout = Duration(seconds: 60);
  String? _failure;
  bool _finishing = false;

  @override
  void initState() {
    super.initState();
    _poll();
  }

  Future<void> _poll() async {
    final deadline = DateTime.now().add(_timeout);
    var lostContact = 0;
    while (mounted && DateTime.now().isBefore(deadline)) {
      await Future<void>.delayed(const Duration(seconds: 1));
      if (!mounted) return;
      final ProvisionResult r;
      try {
        r = await widget.flow.client.result();
      } on ApiException {
        // While the robot's radio retunes to the home channel the AP briefly drops.
        if (++lostContact > 15) break;
        continue;
      }
      if (r.state == ProvisionState.connected) {
        await _finish(r);
        return;
      }
      if (r.state == ProvisionState.failed) {
        _fail(r.reason);
        return;
      }
    }
    _fail('timeout');
  }

  void _fail(String? reason) {
    if (!mounted) return;
    final l = AppLocalizations.of(context);
    setState(() {
      _failure = switch (reason) {
        'auth_failed' => l.connectFailedPassword,
        'no_ap' => l.connectFailedNotFound,
        _ => l.connectFailedTimeout,
      };
    });
  }

  Future<void> _finish(ProvisionResult r) async {
    setState(() => _finishing = true);
    final flow = widget.flow;
    final app = AppScope.read(context);
    try {
      await flow.client.finish();
    } on ApiException {
      // The robot still persists on its own once connected; finish only speeds it up.
    }
    final robot = SavedRobot(
      id: flow.target.payload.deviceId,
      name: app.robot(flow.target.payload.deviceId)?.name ?? flow.target.payload.deviceId,
      host: r.ip!,
      port: r.port!,
      token: r.token!,
    );
    await app.upsertRobot(robot);
    await flow.close();
    if (!mounted) return;
    Navigator.of(context).pushAndRemoveUntil(
      MaterialPageRoute<void>(builder: (_) => NamePage(robot: robot)),
      (route) => route.isFirst,
    );
  }

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context);
    final failed = _failure != null;
    return Scaffold(
      appBar: AppBar(title: Text(l.connectTitle), automaticallyImplyLeading: failed),
      body: SafeArea(
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: Column(mainAxisAlignment: MainAxisAlignment.center, crossAxisAlignment: CrossAxisAlignment.stretch, children: [
            if (!failed) ...[
              const Center(child: CircularProgressIndicator()),
              const SizedBox(height: 24),
              Text(_finishing ? l.connectFinishing : l.connectWorking(widget.ssid), textAlign: TextAlign.center),
            ] else ...[
              Icon(Icons.error_outline, size: 64, color: Theme.of(context).colorScheme.error),
              const SizedBox(height: 16),
              Text(_failure!, textAlign: TextAlign.center),
              const SizedBox(height: 24),
              FilledButton(onPressed: () => Navigator.of(context).pop(), child: Text(l.tryAgain)),
            ],
          ]),
        ),
      ),
    );
  }
}
