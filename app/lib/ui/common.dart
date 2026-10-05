import 'package:flutter/material.dart';

import '../core/json_http.dart';
import '../l10n/app_localizations.dart';

/// Generic, localized text for an API error. Robot-provided details are not shown to users.
String errorText(AppLocalizations l, ApiException e) {
  switch (e.kind) {
    case ApiErrorKind.network:
    case ApiErrorKind.timeout:
      return l.errNetwork;
    case ApiErrorKind.unauthorized:
      return l.errUnauthorized;
    case ApiErrorKind.forbidden:
      return l.errForbidden;
    case ApiErrorKind.busy:
      return l.errBusy;
    case ApiErrorKind.unavailable:
    case ApiErrorKind.notFound:
      return l.errUnavailable;
    case ApiErrorKind.badRequest:
    case ApiErrorKind.badResponse:
      return l.errGeneric;
  }
}

void showError(BuildContext context, ApiException? e) {
  if (e == null || !context.mounted) return;
  ScaffoldMessenger.of(context)
    ..hideCurrentSnackBar()
    ..showSnackBar(SnackBar(content: Text(errorText(AppLocalizations.of(context), e))));
}

void showInfo(BuildContext context, String text) {
  if (!context.mounted) return;
  ScaffoldMessenger.of(context)
    ..hideCurrentSnackBar()
    ..showSnackBar(SnackBar(content: Text(text)));
}

Future<bool> confirm(BuildContext context, {required String title, required String body, String? ok, bool danger = false}) async {
  final l = AppLocalizations.of(context);
  final res = await showDialog<bool>(
    context: context,
    builder: (c) => AlertDialog(
      title: Text(title),
      content: Text(body),
      actions: [
        TextButton(onPressed: () => Navigator.pop(c, false), child: Text(l.cancel)),
        FilledButton(
          style: danger ? FilledButton.styleFrom(backgroundColor: Theme.of(c).colorScheme.error) : null,
          onPressed: () => Navigator.pop(c, true),
          child: Text(ok ?? l.ok),
        ),
      ],
    ),
  );
  return res ?? false;
}

class SectionCard extends StatelessWidget {
  const SectionCard({super.key, required this.title, required this.child, this.trailing});

  final String title;
  final Widget child;
  final Widget? trailing;

  @override
  Widget build(BuildContext context) {
    return Card(
      margin: const EdgeInsets.fromLTRB(16, 8, 16, 8),
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(children: [
              Expanded(child: Text(title, style: Theme.of(context).textTheme.titleMedium)),
              ?trailing,
            ]),
            const SizedBox(height: 12),
            child,
          ],
        ),
      ),
    );
  }
}

/// Full-width step layout used by the pairing screens.
class StepScaffold extends StatelessWidget {
  const StepScaffold({super.key, required this.title, required this.children, this.actions = const []});

  final String title;
  final List<Widget> children;
  final List<Widget> actions;

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: Text(title)),
      body: SafeArea(
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Expanded(child: ListView(children: children)),
              for (final a in actions) Padding(padding: const EdgeInsets.only(top: 8), child: a),
            ],
          ),
        ),
      ),
    );
  }
}
