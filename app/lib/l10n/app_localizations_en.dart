// ignore: unused_import
import 'package:intl/intl.dart' as intl;
import 'app_localizations.dart';

// ignore_for_file: type=lint

/// The translations for English (`en`).
class AppLocalizationsEn extends AppLocalizations {
  AppLocalizationsEn([String locale = 'en']) : super(locale);

  @override
  String get appTitle => 'ENCO-02';

  @override
  String get addRobot => 'Add robot';

  @override
  String get welcomeTitle => 'Meet your ENCO-02';

  @override
  String get welcomeBody =>
      'Power on the robot. When its screen shows a QR code, tap Add robot and scan it to connect the robot to your Wi-Fi.';

  @override
  String get errNetwork =>
      'Can\'t reach the robot. Make sure your phone and the robot are on the same Wi-Fi.';

  @override
  String get errUnauthorized =>
      'This phone is no longer paired with the robot. Pair it again.';

  @override
  String get errForbidden => 'Admin key missing or wrong.';

  @override
  String get errBusy => 'The robot is busy right now. Try again in a moment.';

  @override
  String get errUnavailable => 'Not available on this robot\'s firmware.';

  @override
  String get errGeneric => 'Something went wrong. Please try again.';

  @override
  String get ok => 'OK';

  @override
  String get cancel => 'Cancel';

  @override
  String get save => 'Save';

  @override
  String get done => 'Done';

  @override
  String get retry => 'Retry';

  @override
  String get tryAgain => 'Try again';

  @override
  String get refresh => 'Refresh';

  @override
  String get send => 'Send';

  @override
  String get copy => 'Copy';

  @override
  String get copied => 'Copied';

  @override
  String get connect => 'Connect';

  @override
  String get continueLabel => 'Continue';

  @override
  String get cancelPairing => 'Cancel setup';

  @override
  String get cancelPairingTitle => 'Cancel setup?';

  @override
  String get cancelPairingBody =>
      'The robot stays in setup mode, so you can scan its code again any time.';

  @override
  String get scanTitle => 'Scan the robot\'s code';

  @override
  String get scanHint =>
      'Point the camera at the QR code on the robot\'s screen.';

  @override
  String get scanWhereIsQr =>
      'No code on screen? A new robot shows it on first start. Otherwise say \"reset network\" or use Change Wi-Fi in the app.';

  @override
  String get scanInvalid => 'That\'s not an ENCO-02 setup code.';

  @override
  String get cameraUnavailable =>
      'The camera isn\'t available. Allow camera access in system settings.';

  @override
  String get devPasteCode => 'Paste setup code';

  @override
  String devUseMock(String address) {
    return 'Mock robot at $address';
  }

  @override
  String get joinTitle => 'Connecting to robot';

  @override
  String joinJoining(String ssid) {
    return 'Joining the robot\'s hotspot \"$ssid\"…\nAllow the connection if your phone asks.';
  }

  @override
  String get joinVerifying => 'Saying hello to the robot…';

  @override
  String get joinFailed => 'Couldn\'t connect to the robot\'s hotspot.';

  @override
  String get joinMismatch =>
      'Connected, but this isn\'t the robot from the code. Scan the code again.';

  @override
  String get joinManualSteps =>
      'Join it by hand in Wi-Fi settings, then come back and tap Retry:';

  @override
  String get wifiTitle => 'Choose your Wi-Fi';

  @override
  String get wifiHint => 'Pick the Wi-Fi the robot should use.';

  @override
  String get wifiAvailable => 'Networks the robot can see';

  @override
  String get wifiNone =>
      'No networks found. Move the robot closer to your router and refresh.';

  @override
  String get wifiManual => 'Enter network name';

  @override
  String get wifiName => 'Wi-Fi name';

  @override
  String get wifiPassword => 'Password';

  @override
  String get wifiNameInvalid => 'Enter a network name (up to 32 bytes).';

  @override
  String get wifiPasswordInvalid => 'Wi-Fi passwords are 8–63 characters.';

  @override
  String get wifi24Note => 'ENCO-02 supports 2.4 GHz Wi-Fi only.';

  @override
  String get connectTitle => 'Connecting';

  @override
  String connectWorking(String ssid) {
    return 'The robot is joining \"$ssid\"…';
  }

  @override
  String get connectFinishing => 'Connected! Finishing setup…';

  @override
  String get connectFailedPassword => 'Wrong Wi-Fi password.';

  @override
  String get connectFailedNotFound =>
      'The robot can\'t find that network. Is it 2.4 GHz and in range?';

  @override
  String get connectFailedTimeout => 'The robot couldn\'t connect in time.';

  @override
  String get pairedTitle => 'All set';

  @override
  String get pairedBody => 'Your robot is online. Give it a name.';

  @override
  String get robotName => 'Robot name';

  @override
  String get robotNameInvalid => '1–24 characters.';

  @override
  String get tabControl => 'Control';

  @override
  String get tabDeveloper => 'Developer';

  @override
  String get tabSettings => 'Settings';

  @override
  String get online => 'Online';

  @override
  String get connecting => 'Connecting';

  @override
  String get offline => 'Offline';

  @override
  String get offlineBody =>
      'Can\'t find the robot on this network. Check that it\'s powered on and your phone is on the same Wi-Fi.';

  @override
  String get unauthorizedBody =>
      'The robot no longer accepts this phone (it was paired again or reset). Unpair it in Settings, then add it again.';

  @override
  String updateAvailableShort(String version) {
    return 'Firmware $version is available';
  }

  @override
  String get faceTitle => 'Face';

  @override
  String get characterK3 => 'K3';

  @override
  String get characterFox => 'Fox';

  @override
  String get exprNeutral => 'Neutral';

  @override
  String get exprHappy => 'Happy';

  @override
  String get exprSad => 'Sad';

  @override
  String get exprAngry => 'Angry';

  @override
  String get exprSurprised => 'Surprised';

  @override
  String get exprShy => 'Shy';

  @override
  String get exprPout => 'Pout';

  @override
  String get exprWink => 'Wink';

  @override
  String get exprThinking => 'Thinking';

  @override
  String get exprSleepy => 'Sleepy';

  @override
  String get headTitle => 'Head';

  @override
  String get headUp => 'Look up';

  @override
  String get headDown => 'Look down';

  @override
  String get headLeft => 'Turn left';

  @override
  String get headRight => 'Turn right';

  @override
  String get headTiltLeft => 'Tilt left';

  @override
  String get headTiltRight => 'Tilt right';

  @override
  String get headCenter => 'Center';

  @override
  String get headRandom => 'Surprise me';

  @override
  String get headAuto => 'Idle head motion';

  @override
  String get headAutoHint => 'Small random movements while idle';

  @override
  String get featuresTitle => 'Features';

  @override
  String get tracking => 'Face tracking';

  @override
  String get trackingHint => 'Turn the head to follow you';

  @override
  String get trackingNoCam => 'Camera module not detected';

  @override
  String get caption => 'Show captions';

  @override
  String get screenMode => 'Screen';

  @override
  String get screenFace => 'Face';

  @override
  String get screenChat => 'Chat';

  @override
  String get volume => 'Volume';

  @override
  String get sayTitle => 'Talk to the robot';

  @override
  String get sayHint => 'Type something to ask or say';

  @override
  String get settingsRobot => 'Robot';

  @override
  String get settingsNetwork => 'Network & pairing';

  @override
  String get settingsApp => 'App';

  @override
  String get deviceId => 'Device ID';

  @override
  String get firmwareVersions => 'Firmware';

  @override
  String get boardMain => 'Main board';

  @override
  String get boardCam => 'Camera';

  @override
  String get firmwareUpdate => 'Firmware update';

  @override
  String get changeWifi => 'Change Wi-Fi';

  @override
  String get changeWifiHint => 'Move the robot to another network';

  @override
  String get changeWifiConfirm =>
      'The robot will forget its Wi-Fi and restart in setup mode. Then scan the code on its screen again.';

  @override
  String get unpair => 'Remove robot';

  @override
  String get unpairHint => 'Forget this robot on this phone';

  @override
  String get unpairConfirm =>
      'This phone will no longer control the robot. You can add it again by scanning its code.';

  @override
  String get language => 'Language';

  @override
  String get languageSystem => 'System';

  @override
  String get about => 'About';

  @override
  String get firmwareInstalled => 'Installed';

  @override
  String get firmwareAvailable => 'Updates';

  @override
  String get checkForUpdates => 'Check for updates';

  @override
  String get upToDate => 'Everything is up to date.';

  @override
  String get noReleasePublished => 'No release has been published yet.';

  @override
  String get install => 'Install';

  @override
  String installTitle(String board, String version) {
    return 'Install $board $version?';
  }

  @override
  String get installConfirm =>
      'The robot downloads and installs the update by itself, then restarts. This takes a few minutes. Keep it plugged in. You can close the app.';

  @override
  String get firmwareHowItWorks =>
      'The robot also checks for updates on its own. When it finds one, it will ask you out loud. Answer \"yes\", or tap Install here.';

  @override
  String get otaChecking => 'Checking…';

  @override
  String get otaDownloading => 'Downloading…';

  @override
  String get otaVerifying => 'Verifying…';

  @override
  String get otaInstalling => 'Installing…';

  @override
  String get otaDone => 'Update installed';

  @override
  String get otaFailed => 'Update failed';

  @override
  String get otaKeepPowered => 'Keep the robot powered on.';

  @override
  String get devAdminKey => 'Admin key';

  @override
  String get devAdminKeyHint => 'APP_ADMIN_KEY from home_config.h';

  @override
  String get devAdminKeySet => 'Admin key saved on this phone';

  @override
  String get devClear => 'Clear';

  @override
  String get devServos => 'Servos (live)';

  @override
  String get devLimits => 'Limits & center';

  @override
  String get devLimitsHint =>
      'Safe range and neutral position per axis. Apply tries them; Save stores them on the robot.';

  @override
  String get devCenter => 'center';

  @override
  String get devReload => 'Reload';

  @override
  String get devApply => 'Apply';

  @override
  String get devSaved => 'Saved';

  @override
  String get devSpeeds => 'Axis speed (°/s)';

  @override
  String get devTest => 'Test move';

  @override
  String get devTracking => 'Tracking direction';

  @override
  String get devFlip => 'Flip';

  @override
  String get devResetDirs => 'Reset';

  @override
  String get devDiagnostics => 'Diagnostics';

  @override
  String get devDiagHint => 'Tap refresh to read heap, uptime and more.';

  @override
  String get devReboot => 'Reboot robot';

  @override
  String get devRebootConfirm => 'Restart the robot now?';

  @override
  String get devConsole => 'Request console';

  @override
  String get axisPitch => 'Pitch';

  @override
  String get axisRoll => 'Roll';

  @override
  String get axisYaw => 'Yaw';
}
