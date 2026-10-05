import 'package:flutter/material.dart';

import '../../core/json_http.dart';
import '../../core/robot_api.dart';
import '../../core/validators.dart';
import '../../l10n/app_localizations.dart';
import '../../models/saved_robot.dart';
import '../../state/app_state.dart';
import '../robot/robot_page.dart';

/// Step 5: paired. Give the robot a name, then open its control page.
class NamePage extends StatefulWidget {
  const NamePage({super.key, required this.robot});

  final SavedRobot robot;

  @override
  State<NamePage> createState() => _NamePageState();
}

class _NamePageState extends State<NamePage> {
  late final TextEditingController _name =
      TextEditingController(text: widget.robot.name == widget.robot.id ? 'ENCO-02' : widget.robot.name);
  final _form = GlobalKey<FormState>();
  bool _saving = false;

  @override
  void dispose() {
    _name.dispose();
    super.dispose();
  }

  Future<void> _save() async {
    if (!_form.currentState!.validate()) return;
    setState(() => _saving = true);
    final name = _name.text.trim();
    final app = AppScope.read(context);
    final r = widget.robot;
    // Best effort: the phone may still be switching back to the home network.
    final api = RobotApi(host: r.host, port: r.port, token: r.token);
    try {
      await api.rename(name);
    } on ApiException {
      // Kept locally; the robot's own name is re-synced from /api/info on the next connect.
    } finally {
      api.close();
    }
    await app.upsertRobot(r.copyWith(name: name));
    if (!mounted) return;
    Navigator.of(context).pushReplacement(MaterialPageRoute<void>(builder: (_) => RobotPage(robotId: r.id)));
  }

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context);
    return Scaffold(
      appBar: AppBar(title: Text(l.pairedTitle), automaticallyImplyLeading: false),
      body: SafeArea(
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: Form(
            key: _form,
            child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
              const SizedBox(height: 24),
              Icon(Icons.check_circle_outline, size: 96, color: Theme.of(context).colorScheme.primary),
              const SizedBox(height: 16),
              Text(l.pairedBody, textAlign: TextAlign.center),
              const SizedBox(height: 32),
              TextFormField(
                controller: _name,
                decoration: InputDecoration(labelText: l.robotName),
                validator: (v) => Validators.robotName(v ?? '') ? null : l.robotNameInvalid,
              ),
              const Spacer(),
              FilledButton(onPressed: _saving ? null : _save, child: Text(l.done)),
            ]),
          ),
        ),
      ),
    );
  }
}
