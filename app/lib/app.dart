import 'package:flutter/material.dart';
import 'package:flutter_localizations/flutter_localizations.dart';

import 'app_config.dart';
import 'l10n/app_localizations.dart';
import 'state/app_state.dart';
import 'ui/home_page.dart';

Future<void> runEncoApp(AppConfig config) async {
  WidgetsFlutterBinding.ensureInitialized();
  final state = AppState(config: config);
  await state.load();
  runApp(AppScope(state: state, child: const EncoApp()));
}

class EncoApp extends StatelessWidget {
  const EncoApp({super.key});

  @override
  Widget build(BuildContext context) {
    final state = AppScope.of(context);
    return MaterialApp(
      onGenerateTitle: (c) => AppLocalizations.of(c).appTitle,
      debugShowCheckedModeBanner: state.config.isDev,
      theme: _theme(),
      locale: state.locale,
      supportedLocales: AppLocalizations.supportedLocales,
      localizationsDelegates: const [
        AppLocalizations.delegate,
        GlobalMaterialLocalizations.delegate,
        GlobalWidgetsLocalizations.delegate,
        GlobalCupertinoLocalizations.delegate,
      ],
      home: const HomePage(),
    );
  }

  ThemeData _theme() {
    final scheme = ColorScheme.fromSeed(
      seedColor: const Color(0xFF38BDF8),
      brightness: Brightness.dark,
      surface: const Color(0xFF0F172A),
    );
    return ThemeData(
      colorScheme: scheme,
      useMaterial3: true,
      scaffoldBackgroundColor: const Color(0xFF0B1120),
      cardTheme: const CardThemeData(
        color: Color(0xFF1E293B),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.all(Radius.circular(16))),
      ),
      inputDecorationTheme: const InputDecorationTheme(border: OutlineInputBorder()),
    );
  }
}
