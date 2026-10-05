// ignore: unused_import
import 'package:intl/intl.dart' as intl;
import 'app_localizations.dart';

// ignore_for_file: type=lint

/// The translations for Chinese (`zh`).
class AppLocalizationsZh extends AppLocalizations {
  AppLocalizationsZh([String locale = 'zh']) : super(locale);

  @override
  String get appTitle => 'ENCO-02';

  @override
  String get addRobot => '添加机器人';

  @override
  String get welcomeTitle => '你好，ENCO-02';

  @override
  String get welcomeBody => '打开机器人电源。屏幕出现二维码后，点“添加机器人”扫码，就能把它连到你的 Wi-Fi。';

  @override
  String get errNetwork => '连不上机器人。请确认手机和机器人连在同一个 Wi-Fi。';

  @override
  String get errUnauthorized => '这台手机已不再与机器人配对，请重新配对。';

  @override
  String get errForbidden => '管理员密钥缺失或错误。';

  @override
  String get errBusy => '机器人正忙，请稍后再试。';

  @override
  String get errUnavailable => '当前机器人固件不支持此功能。';

  @override
  String get errGeneric => '出了点问题，请重试。';

  @override
  String get ok => '确定';

  @override
  String get cancel => '取消';

  @override
  String get save => '保存';

  @override
  String get done => '完成';

  @override
  String get retry => '重试';

  @override
  String get tryAgain => '再试一次';

  @override
  String get refresh => '刷新';

  @override
  String get send => '发送';

  @override
  String get copy => '复制';

  @override
  String get copied => '已复制';

  @override
  String get connect => '连接';

  @override
  String get continueLabel => '继续';

  @override
  String get cancelPairing => '取消设置';

  @override
  String get cancelPairingTitle => '取消设置？';

  @override
  String get cancelPairingBody => '机器人会保持在配网模式，你可以随时重新扫码。';

  @override
  String get scanTitle => '扫描机器人二维码';

  @override
  String get scanHint => '将相机对准机器人屏幕上的二维码。';

  @override
  String get scanWhereIsQr =>
      '屏幕上没有二维码？新机器人首次开机会显示；否则对它说“重置网络”，或在 App 中选择“更换 Wi-Fi”。';

  @override
  String get scanInvalid => '这不是 ENCO-02 的配网二维码。';

  @override
  String get cameraUnavailable => '无法使用相机，请在系统设置中允许相机权限。';

  @override
  String get devPasteCode => '粘贴配网码';

  @override
  String devUseMock(String address) {
    return '模拟机器人 $address';
  }

  @override
  String get joinTitle => '正在连接机器人';

  @override
  String joinJoining(String ssid) {
    return '正在连接机器人热点“$ssid”…\n如手机询问，请允许连接。';
  }

  @override
  String get joinVerifying => '正在和机器人打招呼…';

  @override
  String get joinFailed => '无法连接到机器人热点。';

  @override
  String get joinMismatch => '已连接，但不是二维码对应的机器人，请重新扫码。';

  @override
  String get joinManualSteps => '请在系统 Wi-Fi 设置中手动连接，然后回来点“重试”：';

  @override
  String get wifiTitle => '选择 Wi-Fi';

  @override
  String get wifiHint => '选择机器人要使用的 Wi-Fi。';

  @override
  String get wifiAvailable => '机器人扫描到的网络';

  @override
  String get wifiNone => '没有找到网络。请把机器人移近路由器后刷新。';

  @override
  String get wifiManual => '手动输入网络名称';

  @override
  String get wifiName => 'Wi-Fi 名称';

  @override
  String get wifiPassword => '密码';

  @override
  String get wifiNameInvalid => '请输入网络名称（最多 32 字节）。';

  @override
  String get wifiPasswordInvalid => 'Wi-Fi 密码为 8–63 个字符。';

  @override
  String get wifi24Note => 'ENCO-02 仅支持 2.4 GHz Wi-Fi。';

  @override
  String get connectTitle => '正在联网';

  @override
  String connectWorking(String ssid) {
    return '机器人正在连接“$ssid”…';
  }

  @override
  String get connectFinishing => '已连接！正在完成设置…';

  @override
  String get connectFailedPassword => 'Wi-Fi 密码错误。';

  @override
  String get connectFailedNotFound => '机器人找不到该网络。请确认是 2.4 GHz 且在信号范围内。';

  @override
  String get connectFailedTimeout => '机器人连接超时。';

  @override
  String get pairedTitle => '设置完成';

  @override
  String get pairedBody => '机器人已联网，给它起个名字吧。';

  @override
  String get robotName => '机器人名称';

  @override
  String get robotNameInvalid => '1–24 个字符。';

  @override
  String get tabControl => '控制';

  @override
  String get tabDeveloper => '开发者';

  @override
  String get tabSettings => '设置';

  @override
  String get online => '在线';

  @override
  String get connecting => '连接中';

  @override
  String get offline => '离线';

  @override
  String get offlineBody => '在当前网络中找不到机器人。请确认它已开机，且手机连在同一个 Wi-Fi。';

  @override
  String get unauthorizedBody => '机器人已不再接受这台手机（已被重新配对或重置）。请在设置中移除后重新添加。';

  @override
  String updateAvailableShort(String version) {
    return '有新固件 $version';
  }

  @override
  String get faceTitle => '表情';

  @override
  String get characterK3 => 'K3';

  @override
  String get characterFox => '狐狸';

  @override
  String get exprNeutral => '平静';

  @override
  String get exprHappy => '开心';

  @override
  String get exprSad => '难过';

  @override
  String get exprAngry => '生气';

  @override
  String get exprSurprised => '惊讶';

  @override
  String get exprShy => '害羞';

  @override
  String get exprPout => '嘟嘴';

  @override
  String get exprWink => '眨眼';

  @override
  String get exprThinking => '思考';

  @override
  String get exprSleepy => '困了';

  @override
  String get headTitle => '头部';

  @override
  String get headUp => '抬头';

  @override
  String get headDown => '低头';

  @override
  String get headLeft => '左转';

  @override
  String get headRight => '右转';

  @override
  String get headTiltLeft => '左歪头';

  @override
  String get headTiltRight => '右歪头';

  @override
  String get headCenter => '回正';

  @override
  String get headRandom => '随机动作';

  @override
  String get headAuto => '空闲时自动动头';

  @override
  String get headAutoHint => '待机时做些小动作';

  @override
  String get featuresTitle => '功能';

  @override
  String get tracking => '人脸跟随';

  @override
  String get trackingHint => '转头跟着你';

  @override
  String get trackingNoCam => '未检测到摄像头模块';

  @override
  String get caption => '显示字幕';

  @override
  String get screenMode => '屏幕';

  @override
  String get screenFace => '表情';

  @override
  String get screenChat => '对话';

  @override
  String get volume => '音量';

  @override
  String get sayTitle => '和机器人说话';

  @override
  String get sayHint => '输入想问或想说的话';

  @override
  String get settingsRobot => '机器人';

  @override
  String get settingsNetwork => '网络与配对';

  @override
  String get settingsApp => '应用';

  @override
  String get deviceId => '设备 ID';

  @override
  String get firmwareVersions => '固件';

  @override
  String get boardMain => '主控板';

  @override
  String get boardCam => '摄像头';

  @override
  String get firmwareUpdate => '固件升级';

  @override
  String get changeWifi => '更换 Wi-Fi';

  @override
  String get changeWifiHint => '让机器人连接另一个网络';

  @override
  String get changeWifiConfirm => '机器人会忘记当前 Wi-Fi 并重启进入配网模式，然后请重新扫描屏幕上的二维码。';

  @override
  String get unpair => '移除机器人';

  @override
  String get unpairHint => '在这台手机上忘记此机器人';

  @override
  String get unpairConfirm => '这台手机将无法再控制该机器人。重新扫码即可再次添加。';

  @override
  String get language => '语言';

  @override
  String get languageSystem => '跟随系统';

  @override
  String get about => '关于';

  @override
  String get firmwareInstalled => '当前版本';

  @override
  String get firmwareAvailable => '可用更新';

  @override
  String get checkForUpdates => '检查更新';

  @override
  String get upToDate => '已是最新版本。';

  @override
  String get noReleasePublished => '还没有发布任何版本。';

  @override
  String get install => '安装';

  @override
  String installTitle(String board, String version) {
    return '安装$board $version？';
  }

  @override
  String get installConfirm => '机器人会自己下载并安装更新，完成后自动重启，需要几分钟。请保持通电，可以关闭 App。';

  @override
  String get firmwareHowItWorks => '机器人也会自己检查更新。发现新版本时会开口问你，回答“好的”或在这里点“安装”即可。';

  @override
  String get otaChecking => '正在检查…';

  @override
  String get otaDownloading => '正在下载…';

  @override
  String get otaVerifying => '正在校验…';

  @override
  String get otaInstalling => '正在安装…';

  @override
  String get otaDone => '更新已安装';

  @override
  String get otaFailed => '更新失败';

  @override
  String get otaKeepPowered => '请保持机器人通电。';

  @override
  String get devAdminKey => '管理员密钥';

  @override
  String get devAdminKeyHint => 'home_config.h 中的 APP_ADMIN_KEY';

  @override
  String get devAdminKeySet => '管理员密钥已保存在本机';

  @override
  String get devClear => '清除';

  @override
  String get devServos => '舵机（实时）';

  @override
  String get devLimits => '限位与中位';

  @override
  String get devLimitsHint => '每个轴的安全范围和中立位置。“应用”立即试用，“保存”写入机器人。';

  @override
  String get devCenter => '中位';

  @override
  String get devReload => '重新读取';

  @override
  String get devApply => '应用';

  @override
  String get devSaved => '已保存';

  @override
  String get devSpeeds => '轴速度（°/秒）';

  @override
  String get devTest => '测试动作';

  @override
  String get devTracking => '跟随方向';

  @override
  String get devFlip => '反转';

  @override
  String get devResetDirs => '重置';

  @override
  String get devDiagnostics => '诊断信息';

  @override
  String get devDiagHint => '点刷新读取内存、运行时间等。';

  @override
  String get devReboot => '重启机器人';

  @override
  String get devRebootConfirm => '现在重启机器人？';

  @override
  String get devConsole => '请求控制台';

  @override
  String get axisPitch => '俯仰';

  @override
  String get axisRoll => '偏侧';

  @override
  String get axisYaw => '水平';
}
