import 'package:flutter/material.dart';
import 'package:mobile_scanner/mobile_scanner.dart';

import '../../app_config.dart';
import '../../core/qr_payload.dart';
import '../../l10n/app_localizations.dart';
import '../../state/app_state.dart';
import '../common.dart';
import 'join_hotspot_page.dart';
import 'pairing_flow.dart';

/// Step 1: scan the QR code on the robot's screen.
class ScanPage extends StatefulWidget {
  const ScanPage({super.key});

  @override
  State<ScanPage> createState() => _ScanPageState();
}

class _ScanPageState extends State<ScanPage> {
  final MobileScannerController _controller = MobileScannerController(formats: const [BarcodeFormat.qrCode]);
  bool _handled = false;
  DateTime _lastInvalid = DateTime.fromMillisecondsSinceEpoch(0);

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  void _onDetect(BarcodeCapture capture) {
    if (_handled) return;
    for (final b in capture.barcodes) {
      final payload = QrPayload.tryParse(b.rawValue);
      if (payload != null) {
        _start(PairingTarget(
          payload: payload,
          host: AppConfig.pairingHost,
          port: AppConfig.pairingPort,
          joinHotspot: true,
        ));
        return;
      }
    }
    // Throttle: the scanner reports the same wrong code many times a second.
    final now = DateTime.now();
    if (now.difference(_lastInvalid) > const Duration(seconds: 3)) {
      _lastInvalid = now;
      showInfo(context, AppLocalizations.of(context).scanInvalid);
    }
  }

  Future<void> _start(PairingTarget target) async {
    _handled = true;
    await _controller.stop();
    if (!mounted) return;
    final flow = PairingFlow(target, allowLoopback: AppScope.read(context).config.isDev);
    await Navigator.of(context).push(MaterialPageRoute<void>(builder: (_) => JoinHotspotPage(flow: flow)));
    // Back here only if the user returned without finishing.
    _handled = false;
    if (mounted) await _controller.start();
  }

  Future<void> _pasteCode() async {
    final l = AppLocalizations.of(context);
    final text = TextEditingController();
    var mock = true;
    final result = await showDialog<(String, bool)>(
      context: context,
      builder: (c) => StatefulBuilder(
        builder: (c, setLocal) => AlertDialog(
          title: Text(l.devPasteCode),
          content: Column(mainAxisSize: MainAxisSize.min, children: [
            TextField(controller: text, decoration: const InputDecoration(hintText: 'enco://pair?v=1&...'), maxLines: 3),
            CheckboxListTile(
              contentPadding: EdgeInsets.zero,
              value: mock,
              onChanged: (v) => setLocal(() => mock = v ?? true),
              title: Text(l.devUseMock('${AppConfig.mockHost}:${AppConfig.mockPort}')),
            ),
          ]),
          actions: [
            TextButton(onPressed: () => Navigator.pop(c), child: Text(l.cancel)),
            FilledButton(onPressed: () => Navigator.pop(c, (text.text, mock)), child: Text(l.ok)),
          ],
        ),
      ),
    );
    text.dispose();
    if (result == null || !mounted) return;
    final payload = QrPayload.tryParse(result.$1);
    if (payload == null) {
      showInfo(context, l.scanInvalid);
      return;
    }
    await _start(PairingTarget(
      payload: payload,
      host: result.$2 ? AppConfig.mockHost : AppConfig.pairingHost,
      port: result.$2 ? AppConfig.mockPort : AppConfig.pairingPort,
      joinHotspot: !result.$2,
    ));
  }

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context);
    final isDev = AppScope.of(context).config.isDev;
    return Scaffold(
      appBar: AppBar(
        title: Text(l.scanTitle),
        actions: [
          if (isDev) IconButton(tooltip: l.devPasteCode, onPressed: _pasteCode, icon: const Icon(Icons.content_paste)),
        ],
      ),
      body: Column(
        children: [
          Padding(
            padding: const EdgeInsets.all(16),
            child: Text(l.scanHint, textAlign: TextAlign.center),
          ),
          Expanded(
            child: ClipRRect(
              borderRadius: BorderRadius.circular(16),
              child: MobileScanner(
                controller: _controller,
                onDetect: _onDetect,
                errorBuilder: (context, error) => Center(
                  child: Padding(
                    padding: const EdgeInsets.all(24),
                    child: Text(l.cameraUnavailable, textAlign: TextAlign.center),
                  ),
                ),
              ),
            ),
          ),
          Padding(
            padding: const EdgeInsets.all(16),
            child: Text(l.scanWhereIsQr, textAlign: TextAlign.center, style: Theme.of(context).textTheme.bodySmall),
          ),
        ],
      ),
    );
  }
}
