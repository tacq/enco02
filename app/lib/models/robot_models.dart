import '../core/json_http.dart';
import '../core/validators.dart';

/// Head axes. `index` (declaration order) is the firmware's axis number: 0 pitch, 1 roll, 2 yaw.
/// `pin` is the GPIO the firmware addresses servos by (servo_controller.h).
enum HeadAxis {
  pitch(0, 40, 120, 90),
  roll(25, 50, 110, 90),
  yaw(26, 20, 120, 70);

  const HeadAxis(this.pin, this.defaultMin, this.defaultMax, this.defaultCenter);

  final int pin;
  final double defaultMin;
  final double defaultMax;
  final double defaultCenter;
}

class FirmwareVersions {
  const FirmwareVersions({this.main, this.cam});

  final String? main;
  final String? cam;

  String? of(String target) => target == 'main' ? main : (target == 'cam' ? cam : null);

  static FirmwareVersions fromJson(Map<String, dynamic>? j) {
    if (j == null) return const FirmwareVersions();
    final m = j.str('main'), c = j.str('cam');
    return FirmwareVersions(main: Validators.version(m) ? m : null, cam: Validators.version(c) ? c : null);
  }
}

class RobotInfo {
  const RobotInfo({
    required this.id,
    required this.name,
    required this.proto,
    required this.firmware,
    required this.characters,
    required this.expressions,
  });

  final String id;
  final String name;
  final int proto;
  final FirmwareVersions firmware;
  final List<String> characters;
  final List<String> expressions;

  static const defaultCharacters = ['k3', 'fox'];
  static const defaultExpressions = [
    'neutral', 'happy', 'sad', 'angry', 'surprised', 'shy', 'pout', 'wink', 'thinking', 'sleepy', //
  ];

  static RobotInfo fromJson(Map<String, dynamic> j) {
    final id = j.str('id');
    if (!Validators.deviceId(id)) throw const ApiException(ApiErrorKind.badResponse, 'bad id');
    final name = j.str('name') ?? id!;
    List<String> names(String key, List<String> fallback) {
      final l = j.list(key).whereType<String>().where(Validators.identifier).take(32).toList();
      return l.isEmpty ? fallback : l;
    }

    return RobotInfo(
      id: id!,
      name: Validators.robotName(name) ? name : id,
      proto: j.integer('proto') ?? 1,
      firmware: FirmwareVersions.fromJson(j.obj('fw')),
      characters: names('characters', defaultCharacters),
      expressions: names('expressions', defaultExpressions),
    );
  }
}

class RobotStatus {
  const RobotStatus({
    required this.angles,
    required this.uiMode,
    required this.volume,
    required this.tracking,
    required this.camPresent,
    this.heap,
    this.rssi,
    this.character,
    this.caption,
    this.headAuto,
    this.uptimeS,
  });

  /// Indexed by [HeadAxis.index].
  final List<double> angles;
  final String uiMode;
  final int volume;
  final bool tracking;
  final bool camPresent;
  final int? heap;
  final int? rssi;
  final String? character;
  final bool? caption;
  final bool? headAuto;
  final int? uptimeS;

  double angle(HeadAxis a) => angles[a.index];

  static RobotStatus fromJson(Map<String, dynamic> j) {
    final mode = j.str('ui_mode');
    final ch = j.str('character');
    return RobotStatus(
      angles: [
        j.number('servo0') ?? HeadAxis.pitch.defaultCenter,
        j.number('servo25') ?? HeadAxis.roll.defaultCenter,
        j.number('servo26') ?? HeadAxis.yaw.defaultCenter,
      ],
      uiMode: mode == 'chat' ? 'chat' : 'face',
      volume: (j.integer('volume') ?? -1).clamp(-1, 100),
      tracking: j.boolean('tracking') ?? false,
      camPresent: j.boolean('cam') ?? false,
      heap: j.integer('heap'),
      rssi: j.integer('rssi'),
      character: Validators.identifier(ch) ? ch : null,
      caption: j.boolean('caption'),
      headAuto: j.boolean('head_auto'),
      uptimeS: j.integer('uptime_s'),
    );
  }
}

class AnimationInfo {
  const AnimationInfo({required this.name, required this.label, required this.idle});

  final String name;
  final String label;
  final bool idle;
}

class AnimationList {
  const AnimationList({required this.anims, required this.playing, required this.auto});

  final List<AnimationInfo> anims;
  final bool playing;
  final bool auto;

  static AnimationList fromJson(Map<String, dynamic> j) {
    final anims = <AnimationInfo>[];
    for (final e in j.list('anims').whereType<Map<String, dynamic>>().take(64)) {
      final name = e.str('name');
      if (!Validators.identifier(name)) continue;
      final label = e.str('label') ?? name!;
      anims.add(AnimationInfo(name: name!, label: label.length > 32 ? name : label, idle: e.boolean('idle') ?? false));
    }
    return AnimationList(anims: anims, playing: j.boolean('playing') ?? false, auto: j.boolean('auto') ?? false);
  }
}

class AxisLimit {
  const AxisLimit({required this.axis, required this.min, required this.max, required this.center});

  final HeadAxis axis;
  final double min;
  final double max;
  final double center;

  bool get isValid => min >= 0 && max <= 180 && min < max && center >= min && center <= max;

  AxisLimit copyWith({double? min, double? max, double? center}) =>
      AxisLimit(axis: axis, min: min ?? this.min, max: max ?? this.max, center: center ?? this.center);

  static AxisLimit defaults(HeadAxis a) => AxisLimit(axis: a, min: a.defaultMin, max: a.defaultMax, center: a.defaultCenter);

  static List<AxisLimit> listFromJson(Map<String, dynamic> j) {
    final out = HeadAxis.values.map(defaults).toList();
    for (final e in j.list('axes').whereType<Map<String, dynamic>>()) {
      final i = e.integer('axis');
      if (i == null || i < 0 || i >= HeadAxis.values.length) continue;
      final l = AxisLimit(
        axis: HeadAxis.values[i],
        min: e.number('min') ?? out[i].min,
        max: e.number('max') ?? out[i].max,
        center: e.number('center') ?? out[i].center,
      );
      if (l.isValid) out[i] = l;
    }
    return out;
  }
}

enum OtaState { idle, checking, available, downloading, verifying, installing, done, failed }

class OtaOffer {
  const OtaOffer({required this.target, required this.version, this.size, this.notes = const {}});

  final String target;
  final String version;
  final int? size;

  /// Language code -> release notes.
  final Map<String, String> notes;

  String notesFor(String lang) => notes[lang] ?? notes['en'] ?? '';
}

class OtaStatus {
  const OtaStatus({required this.state, this.target, this.progress = 0, this.error, this.available = const []});

  final OtaState state;
  final String? target;
  final int progress;
  final String? error;
  final List<OtaOffer> available;

  bool get busy =>
      state == OtaState.checking ||
      state == OtaState.downloading ||
      state == OtaState.verifying ||
      state == OtaState.installing;

  static const idle = OtaStatus(state: OtaState.idle);

  static OtaStatus fromJson(Map<String, dynamic> j) {
    final s = j.str('state');
    final state = OtaState.values.firstWhere((e) => e.name == s, orElse: () => OtaState.idle);
    final t = j.str('target');
    final err = j.str('error');
    return OtaStatus(
      state: state,
      target: isTarget(t) ? t : null,
      progress: (j.integer('progress') ?? 0).clamp(0, 100),
      error: err == null ? null : (err.length > 120 ? err.substring(0, 120) : err),
      available: [
        for (final e in j.list('available').whereType<Map<String, dynamic>>().take(4))
          if (isTarget(e.str('target')) && Validators.version(e.str('version')))
            OtaOffer(
              target: e.str('target')!,
              version: e.str('version')!,
              size: e.integer('size'),
              notes: parseNotes(e.obj('notes')),
            ),
      ],
    );
  }

  static bool isTarget(String? t) => t == 'main' || t == 'cam';

  static Map<String, String> parseNotes(Map<String, dynamic>? n) {
    if (n == null) return const {};
    final out = <String, String>{};
    for (final lang in const ['en', 'zh']) {
      final v = n.str(lang);
      if (v != null) out[lang] = v.length > 2000 ? v.substring(0, 2000) : v;
    }
    return out;
  }
}

class Diagnostics {
  const Diagnostics(this.values);

  /// Flat, display-only key/value pairs (numbers, bools, short strings).
  final Map<String, String> values;

  static Diagnostics fromJson(Map<String, dynamic> j) {
    final out = <String, String>{};
    for (final e in j.entries.take(40)) {
      final v = e.value;
      if (v is num || v is bool || (v is String && v.length <= 64)) out[e.key] = '$v';
    }
    return Diagnostics(out);
  }
}
