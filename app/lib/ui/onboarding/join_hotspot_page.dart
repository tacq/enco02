import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../../core/json_http.dart';
import '../../l10n/app_localizations.dart';
import '../common.dart';
import 'pairing_flow.dart';
import 'wifi_page.dart';

/// Step 2: join the robot's hotspot and confirm it is the robot from the QR code.
class JoinHotspotPage extends StatefulWidget {
  const JoinHotspotPage({super.key, required this.flow});

  final PairingFlow flow;

  @override
  State<JoinHotspotPage> createState() => _JoinHotspotPageState();
}

enum _Phase { joining, verifying, failed }

class _JoinHotspotPageState extends State<JoinHotspotPage> {
  _Phase _phase = _Phase.joining;
  bool _mismatch = false;

  @override
  void initState() {
    super.initState();
    _run();
  }

  Future<void> _run() async {
    setState(() {
      _phase = _Phase.joining;
      _mismatch = false;
    });
    final flow = widget.flow;
    if (flow.target.joinHotspot) {
      bool joined;
      try {
        joined = await flow.hotspot.join(flow.target.payload.apSsid, flow.target.payload.apPassword);
      } on Exception {
        joined = false;
      }
      if (!mounted) return;
      // The user may also have joined by hand from system settings, so carry on and let the
      // verification below decide.
      if (!joined) debugPrint('hotspot join not confirmed, trying anyway');
    }
    setState(() => _phase = _Phase.verifying);
    // The phone needs a moment to get a DHCP lease from the robot.
    for (var attempt = 0; attempt < 5; attempt++) {
      try {
        await flow.client.verifyDevice();
        if (!mounted) return;
        Navigator.of(context).pushReplacement(MaterialPageRoute<void>(builder: (_) => WifiPage(flow: flow)));
        return;
      } on ApiException catch (e) {
        if (e.kind == ApiErrorKind.badResponse || e.kind == ApiErrorKind.unauthorized) {
          _mismatch = true;
          break;
        }
        await Future<void>.delayed(const Duration(milliseconds: 1500));
        if (!mounted) return;
      }
    }
    if (mounted) setState(() => _phase = _Phase.failed);
  }

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context);
    final p = widget.flow.target.payload;
    return PairingStep(
      flow: widget.flow,
      child: StepScaffold(
        title: l.joinTitle,
        actions: [
          if (_phase == _Phase.failed) FilledButton(onPressed: _run, child: Text(l.retry)),
          TextButton(onPressed: () => cancelPairing(context, widget.flow), child: Text(l.cancelPairing)),
        ],
        children: [
          if (_phase != _Phase.failed) ...[
            const SizedBox(height: 48),
            const Center(child: CircularProgressIndicator()),
            const SizedBox(height: 24),
            Text(_phase == _Phase.joining ? l.joinJoining(p.apSsid) : l.joinVerifying, textAlign: TextAlign.center),
          ] else ...[
            Icon(Icons.wifi_off, size: 64, color: Theme.of(context).colorScheme.error),
            const SizedBox(height: 16),
            Text(_mismatch ? l.joinMismatch : l.joinFailed, textAlign: TextAlign.center),
            const SizedBox(height: 24),
            Text(l.joinManualSteps, style: Theme.of(context).textTheme.titleSmall),
            const SizedBox(height: 8),
            _CopyRow(label: l.wifiName, value: p.apSsid),
            _CopyRow(label: l.wifiPassword, value: p.apPassword),
          ],
        ],
      ),
    );
  }
}

class _CopyRow extends StatelessWidget {
  const _CopyRow({required this.label, required this.value});

  final String label;
  final String value;

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context);
    return ListTile(
      contentPadding: EdgeInsets.zero,
      title: Text(label),
      subtitle: SelectableText(value),
      trailing: IconButton(
        tooltip: l.copy,
        icon: const Icon(Icons.copy),
        onPressed: () {
          Clipboard.setData(ClipboardData(text: value));
          showInfo(context, l.copied);
        },
      ),
    );
  }
}
