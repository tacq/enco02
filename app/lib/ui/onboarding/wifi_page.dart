import 'package:flutter/material.dart';

import '../../core/json_http.dart';
import '../../core/provisioning_client.dart';
import '../../core/validators.dart';
import '../../l10n/app_localizations.dart';
import '../common.dart';
import 'connecting_page.dart';
import 'pairing_flow.dart';

/// Step 3: pick the home Wi-Fi (scanned by the robot) and enter its password.
class WifiPage extends StatefulWidget {
  const WifiPage({super.key, required this.flow});

  final PairingFlow flow;

  @override
  State<WifiPage> createState() => _WifiPageState();
}

class _WifiPageState extends State<WifiPage> {
  List<WifiNetwork>? _networks;
  ApiException? _error;
  bool _scanning = false;

  @override
  void initState() {
    super.initState();
    _scan();
  }

  Future<void> _scan() async {
    setState(() {
      _scanning = true;
      _error = null;
    });
    try {
      final list = await widget.flow.client.scan();
      if (mounted) setState(() => _networks = list);
    } on ApiException catch (e) {
      if (mounted) setState(() => _error = e);
    } finally {
      if (mounted) setState(() => _scanning = false);
    }
  }

  Future<void> _pick({String? ssid, bool secure = true}) async {
    final creds = await showModalBottomSheet<(String, String)>(
      context: context,
      isScrollControlled: true,
      builder: (_) => _CredentialsSheet(initialSsid: ssid, secure: secure),
    );
    if (creds == null || !mounted) return;
    try {
      await widget.flow.client.sendWifi(creds.$1, creds.$2);
    } on ApiException catch (e) {
      if (mounted) showError(context, e);
      return;
    }
    if (!mounted) return;
    // Pushed (not replaced) so a failed attempt can come back here.
    await Navigator.of(context).push(MaterialPageRoute<void>(builder: (_) => ConnectingPage(flow: widget.flow, ssid: creds.$1)));
  }

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context);
    final nets = _networks;
    return PairingStep(
      flow: widget.flow,
      child: StepScaffold(
        title: l.wifiTitle,
        actions: [
          OutlinedButton.icon(onPressed: () => _pick(), icon: const Icon(Icons.edit), label: Text(l.wifiManual)),
          TextButton(onPressed: () => cancelPairing(context, widget.flow), child: Text(l.cancelPairing)),
        ],
        children: [
          Text(l.wifiHint),
          const SizedBox(height: 8),
          Row(children: [
            Expanded(child: Text(l.wifiAvailable, style: Theme.of(context).textTheme.titleSmall)),
            IconButton(onPressed: _scanning ? null : _scan, icon: const Icon(Icons.refresh), tooltip: l.refresh),
          ]),
          if (_scanning) const LinearProgressIndicator(),
          if (_error != null) Padding(padding: const EdgeInsets.all(8), child: Text(errorText(l, _error!))),
          if (nets != null && nets.isEmpty && !_scanning) Padding(padding: const EdgeInsets.all(8), child: Text(l.wifiNone)),
          for (final n in nets ?? const <WifiNetwork>[])
            ListTile(
              leading: Icon(_signalIcon(n.rssi)),
              title: Text(n.ssid),
              trailing: n.secure ? const Icon(Icons.lock_outline, size: 18) : null,
              onTap: () => _pick(ssid: n.ssid, secure: n.secure),
            ),
        ],
      ),
    );
  }

  IconData _signalIcon(int rssi) {
    if (rssi >= -60) return Icons.network_wifi;
    if (rssi >= -75) return Icons.network_wifi_3_bar;
    return Icons.network_wifi_1_bar;
  }
}

class _CredentialsSheet extends StatefulWidget {
  const _CredentialsSheet({this.initialSsid, required this.secure});

  final String? initialSsid;
  final bool secure;

  @override
  State<_CredentialsSheet> createState() => _CredentialsSheetState();
}

class _CredentialsSheetState extends State<_CredentialsSheet> {
  final _form = GlobalKey<FormState>();
  late final TextEditingController _ssid = TextEditingController(text: widget.initialSsid);
  final TextEditingController _password = TextEditingController();
  bool _obscure = true;

  @override
  void dispose() {
    _ssid.dispose();
    _password.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context);
    return Padding(
      padding: EdgeInsets.fromLTRB(24, 24, 24, 24 + MediaQuery.of(context).viewInsets.bottom),
      child: Form(
        key: _form,
        child: Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.stretch, children: [
          TextFormField(
            controller: _ssid,
            readOnly: widget.initialSsid != null,
            decoration: InputDecoration(labelText: l.wifiName),
            validator: (v) => Validators.ssid(v) ? null : l.wifiNameInvalid,
          ),
          const SizedBox(height: 16),
          if (widget.secure)
            TextFormField(
              controller: _password,
              obscureText: _obscure,
              autocorrect: false,
              enableSuggestions: false,
              autofocus: true,
              decoration: InputDecoration(
                labelText: l.wifiPassword,
                suffixIcon: IconButton(
                  icon: Icon(_obscure ? Icons.visibility : Icons.visibility_off),
                  onPressed: () => setState(() => _obscure = !_obscure),
                ),
              ),
              validator: (v) => Validators.homeWifiPassword(v ?? '') ? null : l.wifiPasswordInvalid,
            ),
          const SizedBox(height: 8),
          Text(l.wifi24Note, style: Theme.of(context).textTheme.bodySmall),
          const SizedBox(height: 16),
          FilledButton(
            onPressed: () {
              if (_form.currentState!.validate()) {
                Navigator.pop(context, (_ssid.text, widget.secure ? _password.text : ''));
              }
            },
            child: Text(l.connect),
          ),
        ]),
      ),
    );
  }
}
