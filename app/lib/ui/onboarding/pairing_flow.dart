import 'package:flutter/material.dart';

import '../../core/hotspot.dart';
import '../../core/provisioning_client.dart';
import '../../core/qr_payload.dart';
import '../../l10n/app_localizations.dart';
import '../common.dart';

/// Everything a pairing session needs, handed from one step to the next.
class PairingFlow {
  PairingFlow(this.target, {required bool allowLoopback})
      : client = ProvisioningClient(target, allowLoopback: allowLoopback);

  final PairingTarget target;
  final ProvisioningClient client;
  final Hotspot hotspot = Hotspot();
  bool _closed = false;

  Future<void> close() async {
    if (_closed) return;
    _closed = true;
    client.close();
    if (target.joinHotspot) {
      try {
        await hotspot.release();
      } on Exception {
        // Best effort: the OS drops the hotspot on its own once it is out of range.
      }
    }
  }
}

/// Asks before abandoning a pairing session, then returns to the home screen.
Future<void> cancelPairing(BuildContext context, PairingFlow flow) async {
  final l = AppLocalizations.of(context);
  final ok = await confirm(context, title: l.cancelPairingTitle, body: l.cancelPairingBody, ok: l.cancelPairing);
  if (!ok || !context.mounted) return;
  await flow.close();
  if (context.mounted) Navigator.of(context).popUntil((r) => r.isFirst);
}

/// Wraps a pairing step so the system back gesture goes through [cancelPairing].
class PairingStep extends StatelessWidget {
  const PairingStep({super.key, required this.flow, required this.child});

  final PairingFlow flow;
  final Widget child;

  @override
  Widget build(BuildContext context) {
    return PopScope(
      canPop: false,
      onPopInvokedWithResult: (didPop, _) {
        if (!didPop) cancelPairing(context, flow);
      },
      child: child,
    );
  }
}
