import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/widgets.dart';
import 'package:flutter_localizations/flutter_localizations.dart';
import 'package:intl/intl.dart' as intl;

import 'app_localizations_en.dart';
import 'app_localizations_zh.dart';

// ignore_for_file: type=lint

/// Callers can lookup localized strings with an instance of AppLocalizations
/// returned by `AppLocalizations.of(context)`.
///
/// Applications need to include `AppLocalizations.delegate()` in their app's
/// `localizationDelegates` list, and the locales they support in the app's
/// `supportedLocales` list. For example:
///
/// ```dart
/// import 'l10n/app_localizations.dart';
///
/// return MaterialApp(
///   localizationsDelegates: AppLocalizations.localizationsDelegates,
///   supportedLocales: AppLocalizations.supportedLocales,
///   home: MyApplicationHome(),
/// );
/// ```
///
/// ## Update pubspec.yaml
///
/// Please make sure to update your pubspec.yaml to include the following
/// packages:
///
/// ```yaml
/// dependencies:
///   # Internationalization support.
///   flutter_localizations:
///     sdk: flutter
///   intl: any # Use the pinned version from flutter_localizations
///
///   # Rest of dependencies
/// ```
///
/// ## iOS Applications
///
/// iOS applications define key application metadata, including supported
/// locales, in an Info.plist file that is built into the application bundle.
/// To configure the locales supported by your app, you’ll need to edit this
/// file.
///
/// First, open your project’s ios/Runner.xcworkspace Xcode workspace file.
/// Then, in the Project Navigator, open the Info.plist file under the Runner
/// project’s Runner folder.
///
/// Next, select the Information Property List item, select Add Item from the
/// Editor menu, then select Localizations from the pop-up menu.
///
/// Select and expand the newly-created Localizations item then, for each
/// locale your application supports, add a new item and select the locale
/// you wish to add from the pop-up menu in the Value field. This list should
/// be consistent with the languages listed in the AppLocalizations.supportedLocales
/// property.
abstract class AppLocalizations {
  AppLocalizations(String locale)
    : localeName = intl.Intl.canonicalizedLocale(locale.toString());

  final String localeName;

  static AppLocalizations of(BuildContext context) {
    return Localizations.of<AppLocalizations>(context, AppLocalizations)!;
  }

  static const LocalizationsDelegate<AppLocalizations> delegate =
      _AppLocalizationsDelegate();

  /// A list of this localizations delegate along with the default localizations
  /// delegates.
  ///
  /// Returns a list of localizations delegates containing this delegate along with
  /// GlobalMaterialLocalizations.delegate, GlobalCupertinoLocalizations.delegate,
  /// and GlobalWidgetsLocalizations.delegate.
  ///
  /// Additional delegates can be added by appending to this list in
  /// MaterialApp. This list does not have to be used at all if a custom list
  /// of delegates is preferred or required.
  static const List<LocalizationsDelegate<dynamic>> localizationsDelegates =
      <LocalizationsDelegate<dynamic>>[
        delegate,
        GlobalMaterialLocalizations.delegate,
        GlobalCupertinoLocalizations.delegate,
        GlobalWidgetsLocalizations.delegate,
      ];

  /// A list of this localizations delegate's supported locales.
  static const List<Locale> supportedLocales = <Locale>[
    Locale('en'),
    Locale('zh'),
  ];

  /// No description provided for @appTitle.
  ///
  /// In en, this message translates to:
  /// **'ENCO-02'**
  String get appTitle;

  /// No description provided for @addRobot.
  ///
  /// In en, this message translates to:
  /// **'Add robot'**
  String get addRobot;

  /// No description provided for @welcomeTitle.
  ///
  /// In en, this message translates to:
  /// **'Meet your ENCO-02'**
  String get welcomeTitle;

  /// No description provided for @welcomeBody.
  ///
  /// In en, this message translates to:
  /// **'Power on the robot. When its screen shows a QR code, tap Add robot and scan it to connect the robot to your Wi-Fi.'**
  String get welcomeBody;

  /// No description provided for @errNetwork.
  ///
  /// In en, this message translates to:
  /// **'Can\'t reach the robot. Make sure your phone and the robot are on the same Wi-Fi.'**
  String get errNetwork;

  /// No description provided for @errUnauthorized.
  ///
  /// In en, this message translates to:
  /// **'This phone is no longer paired with the robot. Pair it again.'**
  String get errUnauthorized;

  /// No description provided for @errForbidden.
  ///
  /// In en, this message translates to:
  /// **'Admin key missing or wrong.'**
  String get errForbidden;

  /// No description provided for @errBusy.
  ///
  /// In en, this message translates to:
  /// **'The robot is busy right now. Try again in a moment.'**
  String get errBusy;

  /// No description provided for @errUnavailable.
  ///
  /// In en, this message translates to:
  /// **'Not available on this robot\'s firmware.'**
  String get errUnavailable;

  /// No description provided for @errGeneric.
  ///
  /// In en, this message translates to:
  /// **'Something went wrong. Please try again.'**
  String get errGeneric;

  /// No description provided for @ok.
  ///
  /// In en, this message translates to:
  /// **'OK'**
  String get ok;

  /// No description provided for @cancel.
  ///
  /// In en, this message translates to:
  /// **'Cancel'**
  String get cancel;

  /// No description provided for @save.
  ///
  /// In en, this message translates to:
  /// **'Save'**
  String get save;

  /// No description provided for @done.
  ///
  /// In en, this message translates to:
  /// **'Done'**
  String get done;

  /// No description provided for @retry.
  ///
  /// In en, this message translates to:
  /// **'Retry'**
  String get retry;

  /// No description provided for @tryAgain.
  ///
  /// In en, this message translates to:
  /// **'Try again'**
  String get tryAgain;

  /// No description provided for @refresh.
  ///
  /// In en, this message translates to:
  /// **'Refresh'**
  String get refresh;

  /// No description provided for @send.
  ///
  /// In en, this message translates to:
  /// **'Send'**
  String get send;

  /// No description provided for @copy.
  ///
  /// In en, this message translates to:
  /// **'Copy'**
  String get copy;

  /// No description provided for @copied.
  ///
  /// In en, this message translates to:
  /// **'Copied'**
  String get copied;

  /// No description provided for @connect.
  ///
  /// In en, this message translates to:
  /// **'Connect'**
  String get connect;

  /// No description provided for @continueLabel.
  ///
  /// In en, this message translates to:
  /// **'Continue'**
  String get continueLabel;

  /// No description provided for @cancelPairing.
  ///
  /// In en, this message translates to:
  /// **'Cancel setup'**
  String get cancelPairing;

  /// No description provided for @cancelPairingTitle.
  ///
  /// In en, this message translates to:
  /// **'Cancel setup?'**
  String get cancelPairingTitle;

  /// No description provided for @cancelPairingBody.
  ///
  /// In en, this message translates to:
  /// **'The robot stays in setup mode, so you can scan its code again any time.'**
  String get cancelPairingBody;

  /// No description provided for @scanTitle.
  ///
  /// In en, this message translates to:
  /// **'Scan the robot\'s code'**
  String get scanTitle;

  /// No description provided for @scanHint.
  ///
  /// In en, this message translates to:
  /// **'Point the camera at the QR code on the robot\'s screen.'**
  String get scanHint;

  /// No description provided for @scanWhereIsQr.
  ///
  /// In en, this message translates to:
  /// **'No code on screen? A new robot shows it on first start. Otherwise say \"reset network\" or use Change Wi-Fi in the app.'**
  String get scanWhereIsQr;

  /// No description provided for @scanInvalid.
  ///
  /// In en, this message translates to:
  /// **'That\'s not an ENCO-02 setup code.'**
  String get scanInvalid;

  /// No description provided for @cameraUnavailable.
  ///
  /// In en, this message translates to:
  /// **'The camera isn\'t available. Allow camera access in system settings.'**
  String get cameraUnavailable;

  /// No description provided for @devPasteCode.
  ///
  /// In en, this message translates to:
  /// **'Paste setup code'**
  String get devPasteCode;

  /// No description provided for @devUseMock.
  ///
  /// In en, this message translates to:
  /// **'Mock robot at {address}'**
  String devUseMock(String address);

  /// No description provided for @joinTitle.
  ///
  /// In en, this message translates to:
  /// **'Connecting to robot'**
  String get joinTitle;

  /// No description provided for @joinJoining.
  ///
  /// In en, this message translates to:
  /// **'Joining the robot\'s hotspot \"{ssid}\"…\nAllow the connection if your phone asks.'**
  String joinJoining(String ssid);

  /// No description provided for @joinVerifying.
  ///
  /// In en, this message translates to:
  /// **'Saying hello to the robot…'**
  String get joinVerifying;

  /// No description provided for @joinFailed.
  ///
  /// In en, this message translates to:
  /// **'Couldn\'t connect to the robot\'s hotspot.'**
  String get joinFailed;

  /// No description provided for @joinMismatch.
  ///
  /// In en, this message translates to:
  /// **'Connected, but this isn\'t the robot from the code. Scan the code again.'**
  String get joinMismatch;

  /// No description provided for @joinManualSteps.
  ///
  /// In en, this message translates to:
  /// **'Join it by hand in Wi-Fi settings, then come back and tap Retry:'**
  String get joinManualSteps;

  /// No description provided for @wifiTitle.
  ///
  /// In en, this message translates to:
  /// **'Choose your Wi-Fi'**
  String get wifiTitle;

  /// No description provided for @wifiHint.
  ///
  /// In en, this message translates to:
  /// **'Pick the Wi-Fi the robot should use.'**
  String get wifiHint;

  /// No description provided for @wifiAvailable.
  ///
  /// In en, this message translates to:
  /// **'Networks the robot can see'**
  String get wifiAvailable;

  /// No description provided for @wifiNone.
  ///
  /// In en, this message translates to:
  /// **'No networks found. Move the robot closer to your router and refresh.'**
  String get wifiNone;

  /// No description provided for @wifiManual.
  ///
  /// In en, this message translates to:
  /// **'Enter network name'**
  String get wifiManual;

  /// No description provided for @wifiName.
  ///
  /// In en, this message translates to:
  /// **'Wi-Fi name'**
  String get wifiName;

  /// No description provided for @wifiPassword.
  ///
  /// In en, this message translates to:
  /// **'Password'**
  String get wifiPassword;

  /// No description provided for @wifiNameInvalid.
  ///
  /// In en, this message translates to:
  /// **'Enter a network name (up to 32 bytes).'**
  String get wifiNameInvalid;

  /// No description provided for @wifiPasswordInvalid.
  ///
  /// In en, this message translates to:
  /// **'Wi-Fi passwords are 8–63 characters.'**
  String get wifiPasswordInvalid;

  /// No description provided for @wifi24Note.
  ///
  /// In en, this message translates to:
  /// **'ENCO-02 supports 2.4 GHz Wi-Fi only.'**
  String get wifi24Note;

  /// No description provided for @connectTitle.
  ///
  /// In en, this message translates to:
  /// **'Connecting'**
  String get connectTitle;

  /// No description provided for @connectWorking.
  ///
  /// In en, this message translates to:
  /// **'The robot is joining \"{ssid}\"…'**
  String connectWorking(String ssid);

  /// No description provided for @connectFinishing.
  ///
  /// In en, this message translates to:
  /// **'Connected! Finishing setup…'**
  String get connectFinishing;

  /// No description provided for @connectFailedPassword.
  ///
  /// In en, this message translates to:
  /// **'Wrong Wi-Fi password.'**
  String get connectFailedPassword;

  /// No description provided for @connectFailedNotFound.
  ///
  /// In en, this message translates to:
  /// **'The robot can\'t find that network. Is it 2.4 GHz and in range?'**
  String get connectFailedNotFound;

  /// No description provided for @connectFailedTimeout.
  ///
  /// In en, this message translates to:
  /// **'The robot couldn\'t connect in time.'**
  String get connectFailedTimeout;

  /// No description provided for @pairedTitle.
  ///
  /// In en, this message translates to:
  /// **'All set'**
  String get pairedTitle;

  /// No description provided for @pairedBody.
  ///
  /// In en, this message translates to:
  /// **'Your robot is online. Give it a name.'**
  String get pairedBody;

  /// No description provided for @robotName.
  ///
  /// In en, this message translates to:
  /// **'Robot name'**
  String get robotName;

  /// No description provided for @robotNameInvalid.
  ///
  /// In en, this message translates to:
  /// **'1–24 characters.'**
  String get robotNameInvalid;

  /// No description provided for @tabControl.
  ///
  /// In en, this message translates to:
  /// **'Control'**
  String get tabControl;

  /// No description provided for @tabDeveloper.
  ///
  /// In en, this message translates to:
  /// **'Developer'**
  String get tabDeveloper;

  /// No description provided for @tabSettings.
  ///
  /// In en, this message translates to:
  /// **'Settings'**
  String get tabSettings;

  /// No description provided for @online.
  ///
  /// In en, this message translates to:
  /// **'Online'**
  String get online;

  /// No description provided for @connecting.
  ///
  /// In en, this message translates to:
  /// **'Connecting'**
  String get connecting;

  /// No description provided for @offline.
  ///
  /// In en, this message translates to:
  /// **'Offline'**
  String get offline;

  /// No description provided for @offlineBody.
  ///
  /// In en, this message translates to:
  /// **'Can\'t find the robot on this network. Check that it\'s powered on and your phone is on the same Wi-Fi.'**
  String get offlineBody;

  /// No description provided for @unauthorizedBody.
  ///
  /// In en, this message translates to:
  /// **'The robot no longer accepts this phone (it was paired again or reset). Unpair it in Settings, then add it again.'**
  String get unauthorizedBody;

  /// No description provided for @updateAvailableShort.
  ///
  /// In en, this message translates to:
  /// **'Firmware {version} is available'**
  String updateAvailableShort(String version);

  /// No description provided for @faceTitle.
  ///
  /// In en, this message translates to:
  /// **'Face'**
  String get faceTitle;

  /// No description provided for @characterK3.
  ///
  /// In en, this message translates to:
  /// **'K3'**
  String get characterK3;

  /// No description provided for @characterFox.
  ///
  /// In en, this message translates to:
  /// **'Fox'**
  String get characterFox;

  /// No description provided for @exprNeutral.
  ///
  /// In en, this message translates to:
  /// **'Neutral'**
  String get exprNeutral;

  /// No description provided for @exprHappy.
  ///
  /// In en, this message translates to:
  /// **'Happy'**
  String get exprHappy;

  /// No description provided for @exprSad.
  ///
  /// In en, this message translates to:
  /// **'Sad'**
  String get exprSad;

  /// No description provided for @exprAngry.
  ///
  /// In en, this message translates to:
  /// **'Angry'**
  String get exprAngry;

  /// No description provided for @exprSurprised.
  ///
  /// In en, this message translates to:
  /// **'Surprised'**
  String get exprSurprised;

  /// No description provided for @exprShy.
  ///
  /// In en, this message translates to:
  /// **'Shy'**
  String get exprShy;

  /// No description provided for @exprPout.
  ///
  /// In en, this message translates to:
  /// **'Pout'**
  String get exprPout;

  /// No description provided for @exprWink.
  ///
  /// In en, this message translates to:
  /// **'Wink'**
  String get exprWink;

  /// No description provided for @exprThinking.
  ///
  /// In en, this message translates to:
  /// **'Thinking'**
  String get exprThinking;

  /// No description provided for @exprSleepy.
  ///
  /// In en, this message translates to:
  /// **'Sleepy'**
  String get exprSleepy;

  /// No description provided for @headTitle.
  ///
  /// In en, this message translates to:
  /// **'Head'**
  String get headTitle;

  /// No description provided for @headUp.
  ///
  /// In en, this message translates to:
  /// **'Look up'**
  String get headUp;

  /// No description provided for @headDown.
  ///
  /// In en, this message translates to:
  /// **'Look down'**
  String get headDown;

  /// No description provided for @headLeft.
  ///
  /// In en, this message translates to:
  /// **'Turn left'**
  String get headLeft;

  /// No description provided for @headRight.
  ///
  /// In en, this message translates to:
  /// **'Turn right'**
  String get headRight;

  /// No description provided for @headTiltLeft.
  ///
  /// In en, this message translates to:
  /// **'Tilt left'**
  String get headTiltLeft;

  /// No description provided for @headTiltRight.
  ///
  /// In en, this message translates to:
  /// **'Tilt right'**
  String get headTiltRight;

  /// No description provided for @headCenter.
  ///
  /// In en, this message translates to:
  /// **'Center'**
  String get headCenter;

  /// No description provided for @headRandom.
  ///
  /// In en, this message translates to:
  /// **'Surprise me'**
  String get headRandom;

  /// No description provided for @headAuto.
  ///
  /// In en, this message translates to:
  /// **'Idle head motion'**
  String get headAuto;

  /// No description provided for @headAutoHint.
  ///
  /// In en, this message translates to:
  /// **'Small random movements while idle'**
  String get headAutoHint;

  /// No description provided for @featuresTitle.
  ///
  /// In en, this message translates to:
  /// **'Features'**
  String get featuresTitle;

  /// No description provided for @tracking.
  ///
  /// In en, this message translates to:
  /// **'Face tracking'**
  String get tracking;

  /// No description provided for @trackingHint.
  ///
  /// In en, this message translates to:
  /// **'Turn the head to follow you'**
  String get trackingHint;

  /// No description provided for @trackingNoCam.
  ///
  /// In en, this message translates to:
  /// **'Camera module not detected'**
  String get trackingNoCam;

  /// No description provided for @caption.
  ///
  /// In en, this message translates to:
  /// **'Show captions'**
  String get caption;

  /// No description provided for @screenMode.
  ///
  /// In en, this message translates to:
  /// **'Screen'**
  String get screenMode;

  /// No description provided for @screenFace.
  ///
  /// In en, this message translates to:
  /// **'Face'**
  String get screenFace;

  /// No description provided for @screenChat.
  ///
  /// In en, this message translates to:
  /// **'Chat'**
  String get screenChat;

  /// No description provided for @volume.
  ///
  /// In en, this message translates to:
  /// **'Volume'**
  String get volume;

  /// No description provided for @sayTitle.
  ///
  /// In en, this message translates to:
  /// **'Talk to the robot'**
  String get sayTitle;

  /// No description provided for @sayHint.
  ///
  /// In en, this message translates to:
  /// **'Type something to ask or say'**
  String get sayHint;

  /// No description provided for @settingsRobot.
  ///
  /// In en, this message translates to:
  /// **'Robot'**
  String get settingsRobot;

  /// No description provided for @settingsNetwork.
  ///
  /// In en, this message translates to:
  /// **'Network & pairing'**
  String get settingsNetwork;

  /// No description provided for @settingsApp.
  ///
  /// In en, this message translates to:
  /// **'App'**
  String get settingsApp;

  /// No description provided for @deviceId.
  ///
  /// In en, this message translates to:
  /// **'Device ID'**
  String get deviceId;

  /// No description provided for @firmwareVersions.
  ///
  /// In en, this message translates to:
  /// **'Firmware'**
  String get firmwareVersions;

  /// No description provided for @boardMain.
  ///
  /// In en, this message translates to:
  /// **'Main board'**
  String get boardMain;

  /// No description provided for @boardCam.
  ///
  /// In en, this message translates to:
  /// **'Camera'**
  String get boardCam;

  /// No description provided for @firmwareUpdate.
  ///
  /// In en, this message translates to:
  /// **'Firmware update'**
  String get firmwareUpdate;

  /// No description provided for @changeWifi.
  ///
  /// In en, this message translates to:
  /// **'Change Wi-Fi'**
  String get changeWifi;

  /// No description provided for @changeWifiHint.
  ///
  /// In en, this message translates to:
  /// **'Move the robot to another network'**
  String get changeWifiHint;

  /// No description provided for @changeWifiConfirm.
  ///
  /// In en, this message translates to:
  /// **'The robot will forget its Wi-Fi and restart in setup mode. Then scan the code on its screen again.'**
  String get changeWifiConfirm;

  /// No description provided for @unpair.
  ///
  /// In en, this message translates to:
  /// **'Remove robot'**
  String get unpair;

  /// No description provided for @unpairHint.
  ///
  /// In en, this message translates to:
  /// **'Forget this robot on this phone'**
  String get unpairHint;

  /// No description provided for @unpairConfirm.
  ///
  /// In en, this message translates to:
  /// **'This phone will no longer control the robot. You can add it again by scanning its code.'**
  String get unpairConfirm;

  /// No description provided for @language.
  ///
  /// In en, this message translates to:
  /// **'Language'**
  String get language;

  /// No description provided for @languageSystem.
  ///
  /// In en, this message translates to:
  /// **'System'**
  String get languageSystem;

  /// No description provided for @about.
  ///
  /// In en, this message translates to:
  /// **'About'**
  String get about;

  /// No description provided for @firmwareInstalled.
  ///
  /// In en, this message translates to:
  /// **'Installed'**
  String get firmwareInstalled;

  /// No description provided for @firmwareAvailable.
  ///
  /// In en, this message translates to:
  /// **'Updates'**
  String get firmwareAvailable;

  /// No description provided for @checkForUpdates.
  ///
  /// In en, this message translates to:
  /// **'Check for updates'**
  String get checkForUpdates;

  /// No description provided for @upToDate.
  ///
  /// In en, this message translates to:
  /// **'Everything is up to date.'**
  String get upToDate;

  /// No description provided for @noReleasePublished.
  ///
  /// In en, this message translates to:
  /// **'No release has been published yet.'**
  String get noReleasePublished;

  /// No description provided for @install.
  ///
  /// In en, this message translates to:
  /// **'Install'**
  String get install;

  /// No description provided for @installTitle.
  ///
  /// In en, this message translates to:
  /// **'Install {board} {version}?'**
  String installTitle(String board, String version);

  /// No description provided for @installConfirm.
  ///
  /// In en, this message translates to:
  /// **'The robot downloads and installs the update by itself, then restarts. This takes a few minutes. Keep it plugged in. You can close the app.'**
  String get installConfirm;

  /// No description provided for @firmwareHowItWorks.
  ///
  /// In en, this message translates to:
  /// **'The robot also checks for updates on its own. When it finds one, it will ask you out loud. Answer \"yes\", or tap Install here.'**
  String get firmwareHowItWorks;

  /// No description provided for @otaChecking.
  ///
  /// In en, this message translates to:
  /// **'Checking…'**
  String get otaChecking;

  /// No description provided for @otaDownloading.
  ///
  /// In en, this message translates to:
  /// **'Downloading…'**
  String get otaDownloading;

  /// No description provided for @otaVerifying.
  ///
  /// In en, this message translates to:
  /// **'Verifying…'**
  String get otaVerifying;

  /// No description provided for @otaInstalling.
  ///
  /// In en, this message translates to:
  /// **'Installing…'**
  String get otaInstalling;

  /// No description provided for @otaDone.
  ///
  /// In en, this message translates to:
  /// **'Update installed'**
  String get otaDone;

  /// No description provided for @otaFailed.
  ///
  /// In en, this message translates to:
  /// **'Update failed'**
  String get otaFailed;

  /// No description provided for @otaKeepPowered.
  ///
  /// In en, this message translates to:
  /// **'Keep the robot powered on.'**
  String get otaKeepPowered;

  /// No description provided for @devAdminKey.
  ///
  /// In en, this message translates to:
  /// **'Admin key'**
  String get devAdminKey;

  /// No description provided for @devAdminKeyHint.
  ///
  /// In en, this message translates to:
  /// **'APP_ADMIN_KEY from home_config.h'**
  String get devAdminKeyHint;

  /// No description provided for @devAdminKeySet.
  ///
  /// In en, this message translates to:
  /// **'Admin key saved on this phone'**
  String get devAdminKeySet;

  /// No description provided for @devClear.
  ///
  /// In en, this message translates to:
  /// **'Clear'**
  String get devClear;

  /// No description provided for @devServos.
  ///
  /// In en, this message translates to:
  /// **'Servos (live)'**
  String get devServos;

  /// No description provided for @devLimits.
  ///
  /// In en, this message translates to:
  /// **'Limits & center'**
  String get devLimits;

  /// No description provided for @devLimitsHint.
  ///
  /// In en, this message translates to:
  /// **'Safe range and neutral position per axis. Apply tries them; Save stores them on the robot.'**
  String get devLimitsHint;

  /// No description provided for @devCenter.
  ///
  /// In en, this message translates to:
  /// **'center'**
  String get devCenter;

  /// No description provided for @devReload.
  ///
  /// In en, this message translates to:
  /// **'Reload'**
  String get devReload;

  /// No description provided for @devApply.
  ///
  /// In en, this message translates to:
  /// **'Apply'**
  String get devApply;

  /// No description provided for @devSaved.
  ///
  /// In en, this message translates to:
  /// **'Saved'**
  String get devSaved;

  /// No description provided for @devSpeeds.
  ///
  /// In en, this message translates to:
  /// **'Axis speed (°/s)'**
  String get devSpeeds;

  /// No description provided for @devTest.
  ///
  /// In en, this message translates to:
  /// **'Test move'**
  String get devTest;

  /// No description provided for @devTracking.
  ///
  /// In en, this message translates to:
  /// **'Tracking direction'**
  String get devTracking;

  /// No description provided for @devFlip.
  ///
  /// In en, this message translates to:
  /// **'Flip'**
  String get devFlip;

  /// No description provided for @devResetDirs.
  ///
  /// In en, this message translates to:
  /// **'Reset'**
  String get devResetDirs;

  /// No description provided for @devDiagnostics.
  ///
  /// In en, this message translates to:
  /// **'Diagnostics'**
  String get devDiagnostics;

  /// No description provided for @devDiagHint.
  ///
  /// In en, this message translates to:
  /// **'Tap refresh to read heap, uptime and more.'**
  String get devDiagHint;

  /// No description provided for @devReboot.
  ///
  /// In en, this message translates to:
  /// **'Reboot robot'**
  String get devReboot;

  /// No description provided for @devRebootConfirm.
  ///
  /// In en, this message translates to:
  /// **'Restart the robot now?'**
  String get devRebootConfirm;

  /// No description provided for @devConsole.
  ///
  /// In en, this message translates to:
  /// **'Request console'**
  String get devConsole;

  /// No description provided for @axisPitch.
  ///
  /// In en, this message translates to:
  /// **'Pitch'**
  String get axisPitch;

  /// No description provided for @axisRoll.
  ///
  /// In en, this message translates to:
  /// **'Roll'**
  String get axisRoll;

  /// No description provided for @axisYaw.
  ///
  /// In en, this message translates to:
  /// **'Yaw'**
  String get axisYaw;
}

class _AppLocalizationsDelegate
    extends LocalizationsDelegate<AppLocalizations> {
  const _AppLocalizationsDelegate();

  @override
  Future<AppLocalizations> load(Locale locale) {
    return SynchronousFuture<AppLocalizations>(lookupAppLocalizations(locale));
  }

  @override
  bool isSupported(Locale locale) =>
      <String>['en', 'zh'].contains(locale.languageCode);

  @override
  bool shouldReload(_AppLocalizationsDelegate old) => false;
}

AppLocalizations lookupAppLocalizations(Locale locale) {
  // Lookup logic when only language code is specified.
  switch (locale.languageCode) {
    case 'en':
      return AppLocalizationsEn();
    case 'zh':
      return AppLocalizationsZh();
  }

  throw FlutterError(
    'AppLocalizations.delegate failed to load unsupported locale "$locale". This is likely '
    'an issue with the localizations generation tool. Please file an issue '
    'on GitHub with a reproducible sample app and the gen-l10n configuration '
    'that was used.',
  );
}
