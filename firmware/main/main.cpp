#include <Arduino.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <driver/spi_common.h>
#include <esp_debug_helpers.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_lcd_panel_io.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#include "ai_vox_engine.h"
#include "audio_device/audio_input_device_i2s_std.h"
#include "audio_device/audio_input_device_pdm.h"
#include "audio_device/audio_output_device_i2s_std.h"
#include "components/espressif/button/button_gpio.h"
#include "components/espressif/button/iot_button.h"
#include "components/espressif/esp_audio_codec/esp_audio_simple_dec.h"
#include "components/espressif/esp_audio_codec/esp_mp3_dec.h"
#include "core/opus_codec_pool.h"
#include <Preferences.h>
#include "components/wifi_configurator/wifi_configurator.h"
#include "web_wifi_configurator.h"
#include "servo_controller.h"
#include "servo_web_server.h"
#include "display.h"
#include "volume_command.h"
#include "network_config_mode_mp3.h"
#include "network_connected_mp3.h"
#include "notification_0_mp3.h"

#ifndef ARDUINO_ESP32_DEV
#error "This example only supports ESP32-Dev board."
#endif

#define AUDIO_INPUT_DEVICE_TYPE_PDM (0)
#define AUDIO_INPUT_DEVICE_TYPE_I2S_STD (1)
#define AUDIO_INPUT_DEVICE_TYPE AUDIO_INPUT_DEVICE_TYPE_PDM

/**
 * 如果开机自动连接WiFi而不需要配置WiFi, 请注释掉下面的宏, 然后请修改下面的WIFI_SSID和WIFI_PASSWORD
 */
// #define WIFI_SSID "your_wifi_ssid"
// #define WIFI_PASSWORD "your_wifi_password"

std::unique_ptr<Display> g_display;

namespace {
// Wi-Fi configurations
/**
 *  SC_TYPE_ESPTOUCH            protocol: ESPTouch
 *  SC_TYPE_AIRKISS,            protocol: AirKiss
 *  SC_TYPE_ESPTOUCH_AIRKISS,   protocol: ESPTouch and AirKiss
 *  SC_TYPE_ESPTOUCH_V2,        protocol: ESPTouch v2
 */
constexpr smartconfig_type_t kSmartConfigType = SC_TYPE_ESPTOUCH_AIRKISS;  // ESPTouch and AirKiss

// Microphone pin configurations
#if AUDIO_INPUT_DEVICE_TYPE == AUDIO_INPUT_DEVICE_TYPE_I2S_STD
// I2S standard interface
constexpr gpio_num_t kMicPinSck = GPIO_NUM_25;  // SCK (BCK, BCLK): Serial-Data Clock for I²S Interface
constexpr gpio_num_t kMicPinWs = GPIO_NUM_26;   // WS (WR, WCLK): Serial Data-Word Select for I²S Interface
constexpr gpio_num_t kMicPinSd = GPIO_NUM_27;   // SD (DIN，DOUT, DI, DO, DATA): Serial-Data Output for I²S Interface
#elif AUDIO_INPUT_DEVICE_TYPE == AUDIO_INPUT_DEVICE_TYPE_PDM
// PDM interface
constexpr gpio_num_t kMicPinSck = GPIO_NUM_12;  // SCK (BCK, BCLK): Serial-Data Clock for I²S Interface
constexpr gpio_num_t kMicPinSd = GPIO_NUM_13;   // SD (DIN，DOUT, DI, DO, DATA): Serial-Data Output for I²S Interface
#endif

// Speaker pin configurations
constexpr gpio_num_t kSpeakerPinSck = GPIO_NUM_33;  // SCK (BCK, BCLK): Serial-Data Clock for I²S Interface
constexpr gpio_num_t kSpeakerPinWs = GPIO_NUM_32;   // WS (WR, WCLK): Serial Data-Word Select for I²S Interface
constexpr gpio_num_t kSpeakerPinSd = GPIO_NUM_23;   // SD (DIN，DOUT, DI, DO, DATA): Serial-Data Output for I²S Interface

constexpr gpio_num_t kButtonBoot = GPIO_NUM_34;

constexpr gpio_num_t kLedPin = GPIO_NUM_2;

// Display pin configurations for SWIFT-LCD-20 (ST7789 2.0" 240x320)
constexpr gpio_num_t kDisplayBacklightPin = GPIO_NUM_NC;  // Internally pulled high
constexpr gpio_num_t kDisplayMosiPin = GPIO_NUM_16;       // SDA
constexpr gpio_num_t kDisplayClkPin = GPIO_NUM_17;        // SCL
constexpr gpio_num_t kDisplayDcPin = GPIO_NUM_15;         // DC
constexpr gpio_num_t kDisplayRstPin = GPIO_NUM_NC;        // Internally pulled high
constexpr gpio_num_t kDisplayCsPin = GPIO_NUM_14;         // CS

constexpr auto kDisplaySpiMode = 0;
constexpr uint32_t kDisplayWidth = 240;
constexpr uint32_t kDisplayHeight = 320;
constexpr bool kDisplayMirrorX = false;
constexpr bool kDisplayMirrorY = false;
constexpr bool kDisplayInvertColor = true;
constexpr bool kDisplaySwapXY = false;
constexpr auto kDisplayRgbElementOrder = LCD_RGB_ELEMENT_ORDER_RGB;

auto g_observer = std::make_shared<ai_vox::Observer>();
auto g_audio_output_device = std::make_shared<ai_vox::AudioOutputDeviceI2sStd>(kSpeakerPinSck, kSpeakerPinWs, kSpeakerPinSd);
button_handle_t g_button_boot_handle = nullptr;

// Every boot stage is traced so we can see exactly where the internal RAM goes. The mbedTLS
// handshake to api.tenclass.net needs roughly 40KB free with a >16KB contiguous block; if the log
// shows less than that at "engine starting", the connection will fail with ESP_ERR_HTTP_CONNECT.
void LogHeap(const char* stage) {
  printf("[heap] %-22s free: %6u  largest: %6u\n",
         stage,
         static_cast<unsigned>(esp_get_free_heap_size()),
         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

// This firmware is built with -fno-exceptions, so a failed `operator new` goes straight to
// std::terminate() -> abort() -> reboot with no indication of what was being allocated. Hooking the
// allocator means any future out-of-memory condition names itself in the log instead.
//
// The hook runs on whatever task hit the wall, so it must not allocate: `esp_rom_printf` writes
// straight to the UART FIFO, unlike `printf` which goes through newlib's (allocating) stdio. The
// first few failures also dump a call stack - run the printed addresses through
// `xtensa-esp32-elf-addr2line -pfiaC -e .pio/build/enco02_main/firmware.elf ...` to get the exact
// caller. After that the failure is almost always the same one repeating every frame, so the log
// collapses to a periodic one-liner instead of thousands of identical dumps.
void OnHeapAllocFailed(size_t size, uint32_t caps, const char* function_name) {
  static uint32_t fail_count = 0;
  constexpr uint32_t kDetailedReports = 4;
  constexpr uint32_t kSummaryInterval = 200;

  ++fail_count;
  const unsigned free_bytes = static_cast<unsigned>(esp_get_free_heap_size());
  const unsigned largest = static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  if (fail_count > kDetailedReports) {
    if ((fail_count % kSummaryInterval) == 0) {
      esp_rom_printf("[heap] ALLOC FAILED x%u (same size %u) | free: %u, largest: %u\n",
                     static_cast<unsigned>(fail_count),
                     static_cast<unsigned>(size),
                     free_bytes,
                     largest);
    }
    return;
  }

  const char* task_name = pcTaskGetName(nullptr);
  esp_rom_printf("[heap] ALLOC FAILED #%u: %u bytes, caps 0x%x, in %s, task '%s' | free: %u, largest: %u, min ever: %u\n",
                 static_cast<unsigned>(fail_count),
                 static_cast<unsigned>(size),
                 static_cast<unsigned>(caps),
                 function_name ? function_name : "?",
                 task_name ? task_name : "?",
                 free_bytes,
                 largest,
                 static_cast<unsigned>(esp_get_minimum_free_heap_size()));
  esp_backtrace_print(16);
}

// The debug console (WebServer + mDNS) costs ~8.5KB of heap and an extra task. With the TLS session
// and the audio pipeline live there is only ~20KB left on this no-PSRAM board, so it is no longer
// started automatically - long-press the boot button to bring it up when you actually need it.
void EnsureDebugServerStarted() {
  static bool started = false;
  if (started) {
    return;
  }
  if (esp_get_free_heap_size() < 25000) {
    printf("[debug server] not enough heap (free: %u), refusing to start\n", static_cast<unsigned>(esp_get_free_heap_size()));
    return;
  }
  started = true;
  ServoWebServer::GetInstance().Start();
  LogHeap("debug server started");
}

void InitDisplay() {
  printf("init display\n");
  if (kDisplayBacklightPin != GPIO_NUM_NC) {
    pinMode(kDisplayBacklightPin, OUTPUT);
    analogWrite(kDisplayBacklightPin, 255);
  }

  spi_bus_config_t buscfg{
      .mosi_io_num = kDisplayMosiPin,
      .miso_io_num = GPIO_NUM_NC,
      .sclk_io_num = kDisplayClkPin,
      .quadwp_io_num = GPIO_NUM_NC,
      .quadhd_io_num = GPIO_NUM_NC,
      .data4_io_num = GPIO_NUM_NC,
      .data5_io_num = GPIO_NUM_NC,
      .data6_io_num = GPIO_NUM_NC,
      .data7_io_num = GPIO_NUM_NC,
      .data_io_default_level = false,
      .max_transfer_sz = kDisplayWidth * kDisplayHeight * sizeof(uint16_t),
      .flags = 0,
      .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,
      .intr_flags = 0,
  };
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

  esp_lcd_panel_io_handle_t panel_io = nullptr;
  esp_lcd_panel_handle_t panel = nullptr;

  esp_lcd_panel_io_spi_config_t io_config = {};
  io_config.cs_gpio_num = kDisplayCsPin;
  io_config.dc_gpio_num = kDisplayDcPin;
  io_config.spi_mode = kDisplaySpiMode;
  io_config.pclk_hz = 20 * 1000 * 1000;
  io_config.trans_queue_depth = 10;
  io_config.lcd_cmd_bits = 8;
  io_config.lcd_param_bits = 8;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

  esp_lcd_panel_dev_config_t panel_config = {};
  panel_config.reset_gpio_num = kDisplayRstPin;
  panel_config.rgb_ele_order = kDisplayRgbElementOrder;
  panel_config.bits_per_pixel = 16;
  ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

  if (kDisplayRstPin != GPIO_NUM_NC) {
    esp_lcd_panel_reset(panel);
  }

  esp_lcd_panel_init(panel);
  esp_lcd_panel_invert_color(panel, kDisplayInvertColor);
  esp_lcd_panel_swap_xy(panel, kDisplaySwapXY);
  esp_lcd_panel_mirror(panel, kDisplayMirrorX, kDisplayMirrorY);

  g_display = std::make_unique<Display>(panel_io, panel, kDisplayWidth, kDisplayHeight, 0, 0, kDisplayMirrorX, kDisplayMirrorY, kDisplaySwapXY);
  g_display->Start();
}

// Notification sounds are a nicety, never a reason to reboot: this used to abort() whenever the
// decoder could not be created, which on a heap-starved board turned "play the network-connected
// chime" into a boot loop.
void PlayMp3(const uint8_t* data, size_t size) {
  auto ret = esp_mp3_dec_register();
  if (ret != ESP_AUDIO_ERR_OK) {
    printf("Failed to register mp3 decoder: %d, skipping sound\n", ret);
    return;
  }

  esp_audio_simple_dec_handle_t decoder = nullptr;
  esp_audio_simple_dec_cfg_t audio_dec_cfg{
      .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
      .dec_cfg = nullptr,
      .cfg_size = 0,
  };
  ret = esp_audio_simple_dec_open(&audio_dec_cfg, &decoder);
  if (ret != ESP_AUDIO_ERR_OK) {
    printf("Failed to open mp3 decoder: %d, skipping sound\n", ret);
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_MP3);
    return;
  }

  uint8_t* frame_data = static_cast<uint8_t*>(malloc(4096));
  if (frame_data == nullptr) {
    printf("Not enough heap for the mp3 frame buffer, skipping sound\n");
    esp_audio_simple_dec_close(decoder);
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_MP3);
    return;
  }

  g_audio_output_device->OpenOutput(16000);

  esp_audio_simple_dec_raw_t raw = {
      .buffer = const_cast<uint8_t*>(data),
      .len = size,
      .eos = true,
      .consumed = 0,
      .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
  };

  esp_audio_simple_dec_out_t out_frame = {
      .buffer = frame_data,
      .len = 4096,
      .needed_size = 0,
      .decoded_size = 0,
  };

  while (raw.len > 0) {
    const auto ret = esp_audio_simple_dec_process(decoder, &raw, &out_frame);
    if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
      // Handle output buffer not enough case. Keep `frame_data` in sync with the live pointer,
      // otherwise the free() below releases the stale block and leaks the new one.
      auto* grown = static_cast<uint8_t*>(realloc(out_frame.buffer, out_frame.needed_size));
      if (grown == nullptr) {
        break;
      }
      frame_data = grown;
      out_frame.buffer = grown;
      out_frame.len = out_frame.needed_size;
      continue;
    }

    if (ret != ESP_AUDIO_ERR_OK) {
      break;
    }

    g_audio_output_device->Write(reinterpret_cast<int16_t*>(out_frame.buffer), out_frame.decoded_size >> 1);
    raw.len -= raw.consumed;
    raw.buffer += raw.consumed;
  }

  free(frame_data);

  g_audio_output_device->CloseOutput();
  esp_audio_simple_dec_close(decoder);
  esp_audio_dec_unregister(ESP_AUDIO_TYPE_MP3);
}

#ifdef PRINT_HEAP_INFO_INTERVAL
void PrintMemInfo() {
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
    const auto total_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    const auto free_size = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const auto min_free_size = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    printf("SPIRAM total size: %zu B (%zu KB), free size: %zu B (%zu KB), minimum free size: %zu B (%zu KB)\n",
           total_size,
           total_size >> 10,
           free_size,
           free_size >> 10,
           min_free_size,
           min_free_size >> 10);
  }

  if (heap_caps_get_total_size(MALLOC_CAP_INTERNAL) > 0) {
    const auto total_size = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    const auto free_size = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const auto min_free_size = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    printf("IRAM total size: %zu B (%zu KB), free size: %zu B (%zu KB), minimum free size: %zu B (%zu KB)\n",
           total_size,
           total_size >> 10,
           free_size,
           free_size >> 10,
           min_free_size,
           min_free_size >> 10);
  }

  if (heap_caps_get_total_size(MALLOC_CAP_DEFAULT) > 0) {
    const auto total_size = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    const auto free_size = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    const auto min_free_size = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    printf("DRAM total size: %zu B (%zu KB), free size: %zu B (%zu KB), minimum free size: %zu B (%zu KB)\n",
           total_size,
           total_size >> 10,
           free_size,
           free_size >> 10,
           min_free_size,
           min_free_size >> 10);
  }
}
#endif

void ConfigureWifi() {
  printf("configure wifi\n");

  // Arduino's default is static_rx_buf_num=4 / dynamic_rx_buf_num=32, i.e. the Wi-Fi driver
  // malloc()s a ~2.3KB esf_buf for almost every received frame. Late in a session the heap is
  // fragmented down to ~2KB blocks and those allocations start failing inside
  // wDev_IndicateFrame -> esf_buf_alloc_dynamic -> wifi_malloc, which silently drops packets and
  // stalls the TLS stream. Opting into the static pool (8 buffers, reserved during
  // esp_wifi_init() while the heap is still whole) keeps the receive path off the runtime heap.
  // Must be called before anything brings Wi-Fi up.
  WiFi.useStaticBuffers(true);

  auto wifi_configurator = std::make_unique<WifiConfigurator>(WiFi, kSmartConfigType);

  g_display->ShowStatus("网络配置中");
  PlayMp3(kNotification0mp3, sizeof(kNotification0mp3));

#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
  printf("wifi config start with hardcoded wifi: %s, %s\n", WIFI_SSID, WIFI_PASSWORD);
  wifi_configurator->Start(WIFI_SSID, WIFI_PASSWORD);
#else
  Preferences prefs;
  prefs.begin("WiFiConnector", false);
  String saved_ssid = "";
  if (prefs.isKey("ssid")) {
    saved_ssid = prefs.getString("ssid");
  }
  prefs.end();

  if (saved_ssid.length() > 0) {
    printf("wifi config start with saved wifi: %s\n", saved_ssid.c_str());
    g_display->ShowStatus("连接已保存Wi-Fi");
    char msg[128];
    snprintf(msg, sizeof(msg), "正在连接已保存网络:\n%s", saved_ssid.c_str());
    g_display->SetChatMessage(Display::Role::kSystem, msg);
    wifi_configurator->Start();
  } else {
    // Start SoftAP Web Config Mode (xiaozhi-xxxx)
    WebWifiConfigurator web_config;
    String ap_name = web_config.StartApAndServer();

    g_display->ShowStatus("热点配网模式");
    PlayMp3(kNetworkConfigModeMp3, sizeof(kNetworkConfigModeMp3));

    char msg[160];
    snprintf(msg, sizeof(msg), "【Wi-Fi 配网模式】\n1. 连接手机热点:\n   %s\n2. 浏览器打开:\n   192.168.4.1", ap_name.c_str());
    g_display->SetChatMessage(Display::Role::kSystem, msg);

    while (!web_config.IsConfigured()) {
      web_config.HandleClient();
      delay(2);
    }

    g_display->ShowStatus("保存成功，连接中");
    g_display->SetChatMessage(Display::Role::kSystem, "Wi-Fi设置成功！\n正在连接网络，请稍候...");
    web_config.Stop();
    delay(200);

    wifi_configurator->Start();
  }
#endif

  while (true) {
    const auto state = wifi_configurator->WaitStateChanged();
    if (state == WifiConfigurator::State::kConnecting) {
      printf("wifi connecting\n");
      g_display->ShowStatus("网络连接中");
    } else if (state == WifiConfigurator::State::kSmartConfiguring) {
      printf("wifi smart configuring\n");
      g_display->ShowStatus("配网模式");
      PlayMp3(kNetworkConfigModeMp3, sizeof(kNetworkConfigModeMp3));
    } else if (state == WifiConfigurator::State::kFinished) {
      break;
    }
  }

  printf("wifi connected\n");
  printf("- mac address: %s\n", WiFi.macAddress().c_str());
  printf("- bssid:       %s\n", WiFi.BSSIDstr().c_str());
  printf("- ssid:        %s\n", WiFi.SSID().c_str());
  printf("- ip:          %s\n", WiFi.localIP().toString().c_str());
  printf("- gateway:     %s\n", WiFi.gatewayIP().toString().c_str());
  printf("- subnet mask: %s\n", WiFi.subnetMask().toString().c_str());

  g_display->ShowStatus("网络已连接");
  char conn_msg[160];
  snprintf(conn_msg, sizeof(conn_msg), "网络已连接: %s\nIP: %s\n调试页面: http://%s/\n小智 AI 正在启动...",
           WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.localIP().toString().c_str());
  g_display->SetChatMessage(Display::Role::kSystem, conn_msg);
  PlayMp3(kNetworkConnectedMp3, sizeof(kNetworkConnectedMp3));

  // NOTE: the servo web server + mDNS responder are deliberately NOT started here. They cost an
  // extra task and several KB of heap, and the mbedTLS handshake that follows (OTA config fetch,
  // then the WebSocket) needs every byte it can get on this no-PSRAM ESP32. They are started from
  // loop() once the device has successfully reached the cloud - see EnsureDebugServerStarted().
  LogHeap("wifi connected");
}

// ---------------------------------------------------------------- speaker volume

// How far one relative command ("大声一点") moves the volume. The output stage applies a squared
// curve, so 10 points of the 0-100 scale is roughly one clearly audible step.
constexpr int kVolumeStep = 10;
// Floor for *relative* changes only. Letting "小声一点" walk all the way to zero would leave her
// mute with no audible way to discover it; an explicit set_volume(0) can still silence her.
constexpr int kMinRelativeVolume = 10;
constexpr char kVolumePrefsNamespace[] = "enco";
constexpr char kVolumePrefsKey[] = "volume";

// Set when the live volume no longer matches what is stored in NVS. The commit itself is deferred
// to loop() and only runs while she is not speaking: an NVS write stalls the flash cache for tens
// of milliseconds, which is audible as a click if it lands in the middle of a reply.
bool g_volume_dirty = false;
uint32_t g_last_volume_exec_time = 0;
// Tracked purely so the deferred NVS write can hold off while she is talking.
ai_vox::ChatState g_chat_state = ai_vox::ChatState::kIdle;

uint16_t SetVolume(const int requested) {
  // Clamp in signed arithmetic. set_volume() takes a uint16_t, so doing `volume - step` in the
  // device's own type would wrap 5-10 round to 65531, and "quieter" would come out as full blast.
  const int clamped = std::clamp(requested, 0, static_cast<int>(ai_vox::AudioOutputDevice::kMaxVolume));
  const auto volume = static_cast<uint16_t>(clamped);
  if (volume != g_audio_output_device->volume()) {
    g_audio_output_device->set_volume(volume);
    g_volume_dirty = true;
  }
  if (g_display) {
    g_display->ShowVolume(volume);
  }
  return volume;
}

uint16_t AdjustVolume(const int delta) {
  const int current = static_cast<int>(g_audio_output_device->volume());
  return SetVolume(std::max(current + delta, kMinRelativeVolume));
}

void LoadSavedVolume() {
  Preferences prefs;
  if (!prefs.begin(kVolumePrefsNamespace, true)) {
    return;  // Namespace does not exist yet: first boot, keep the device's built-in default.
  }
  const uint16_t saved = prefs.getUShort(kVolumePrefsKey, g_audio_output_device->volume());
  prefs.end();
  g_audio_output_device->set_volume(std::min<uint16_t>(saved, ai_vox::AudioOutputDevice::kMaxVolume));
  // Not via SetVolume(): the value just came *out* of NVS, so there is nothing to write back.
  if (g_display) {
    g_display->ShowVolume(g_audio_output_device->volume());
  }
  printf("[volume] restored to %u\n", static_cast<unsigned>(g_audio_output_device->volume()));
}

void FlushVolumeToNvs() {
  Preferences prefs;
  if (!prefs.begin(kVolumePrefsNamespace, false)) {
    return;
  }
  prefs.putUShort(kVolumePrefsKey, g_audio_output_device->volume());
  prefs.end();
  g_volume_dirty = false;
  printf("[volume] saved %u\n", static_cast<unsigned>(g_audio_output_device->volume()));
}

// The cloud model decides for itself whether to emit an MCP tool call, and for a bare "大声点" it
// frequently just answers conversationally instead. Matching the user's own transcript is what
// makes these commands actually reliable.
//
// Only the user's words are ever matched here, never the assistant's reply: "好的，音量调大了"
// would otherwise bump the volume a second time. The caller already guarantees this by clearing
// g_last_user_query as soon as a real tool call arrives.
bool CheckAndExecuteVolumeFallback(const std::string& query) {
  if (millis() - g_last_volume_exec_time < 2500) {
    return false;  // Debounce, same as the motion fallback.
  }

  const auto command = ClassifyVolumeCommand(query);
  if (command == VolumeCommand::kNone) {
    return false;
  }

  const int delta = command == VolumeCommand::kUp ? kVolumeStep : -kVolumeStep;
  g_last_volume_exec_time = millis();
  const auto volume = AdjustVolume(delta);
  printf("[Voice Volume Fallback] %s -> %u\n", delta > 0 ? "音量增加" : "音量减小", static_cast<unsigned>(volume));
  return true;
}

uint32_t g_last_motion_exec_time = 0;
std::string g_last_user_query = "";

void CheckAndExecuteMotionFallback(const std::string& query) {
  if (millis() - g_last_motion_exec_time < 2500) {
    return;  // Debounce: motion already executed within 2.5s
  }
  if (query.empty()) return;

  auto contains = [&](const char* kw) {
    return query.find(kw) != std::string::npos;
  };

  if (contains("抬头") || contains("仰头") || contains("往上看") || contains("向上看") || contains("看天花板") || contains("看上面")) {
    printf("[Voice Motion Fallback] 抬头 (10 deg)\n");
    g_last_motion_exec_time = millis();
    if (g_display) {
      g_display->UpdateRobotFaceEmotion("happy");
      g_display->LookDirection("up");
      g_display->ShowStatus("抬头中...");
    }
    ServoController::GetInstance().LookUp(10.0f);
  } else if (contains("低头") || contains("俯视") || contains("往下看") || contains("向下看") || contains("看地面") || contains("看下面")) {
    printf("[Voice Motion Fallback] 低头 (10 deg)\n");
    g_last_motion_exec_time = millis();
    if (g_display) {
      g_display->UpdateRobotFaceEmotion("thinking");
      g_display->LookDirection("down");
      g_display->ShowStatus("低头中...");
    }
    ServoController::GetInstance().LookDown(10.0f);
  } else if (contains("向左歪") || contains("往左歪") || contains("左歪头") || contains("左偏头") || contains("左倾")) {
    printf("[Voice Motion Fallback] 向左歪头 (10 deg)\n");
    g_last_motion_exec_time = millis();
    if (g_display) {
      g_display->UpdateRobotFaceEmotion("winking");
      g_display->LookDirection("left");
      g_display->ShowStatus("向左歪头...");
    }
    ServoController::GetInstance().TiltLeft(10.0f);
  } else if (contains("向右歪") || contains("往右歪") || contains("右歪头") || contains("右偏头") || contains("右倾")) {
    printf("[Voice Motion Fallback] 向右歪头 (10 deg)\n");
    g_last_motion_exec_time = millis();
    if (g_display) {
      g_display->UpdateRobotFaceEmotion("winking");
      g_display->LookDirection("right");
      g_display->ShowStatus("向右歪头...");
    }
    ServoController::GetInstance().TiltRight(10.0f);
  } else if (contains("向左转") || contains("往左转") || contains("左转头") || contains("看左边") || contains("往左看")) {
    printf("[Voice Motion Fallback] 向左转头 (10 deg)\n");
    g_last_motion_exec_time = millis();
    if (g_display) {
      g_display->UpdateRobotFaceEmotion("surprised");
      g_display->LookDirection("left");
      g_display->ShowStatus("向左转头...");
    }
    ServoController::GetInstance().TurnLeft(10.0f);
  } else if (contains("向右转") || contains("往右转") || contains("右转头") || contains("看右边") || contains("往右看")) {
    printf("[Voice Motion Fallback] 向右转头 (10 deg)\n");
    g_last_motion_exec_time = millis();
    if (g_display) {
      g_display->UpdateRobotFaceEmotion("surprised");
      g_display->LookDirection("right");
      g_display->ShowStatus("向右转头...");
    }
    ServoController::GetInstance().TurnRight(10.0f);
  } else if (contains("摇头晃脑") || contains("摇摇头") || contains("摇头") || contains("不要不要")) {
    printf("[Voice Motion Fallback] 摇头晃脑\n");
    g_last_motion_exec_time = millis();
    if (g_display) {
      g_display->UpdateRobotFaceEmotion("laughing");
      g_display->ShowStatus("摇头晃脑...");
    }
    ServoController::GetInstance().TriggerHeadBobble();
  } else if (contains("头摆正") || contains("摆正") || contains("正视") || contains("头复位") || contains("向前看")) {
    printf("[Voice Motion Fallback] 头摆正\n");
    g_last_motion_exec_time = millis();
    if (g_display) {
      g_display->UpdateRobotFaceEmotion("neutral");
      g_display->LookDirection("center");
      g_display->ShowStatus("头已正视");
    }
    ServoController::GetInstance().CenterAll();
  }
}

void InitMcpTools() {
  auto& engine = ai_vox::Engine::GetInstance();

  engine.AddMcpTool("self.head.look_up", "Make robot look up (抬头/仰头/往上看/向上看/看天花板).", {});
  engine.AddMcpTool("self.head.look_down", "Make robot look down (低头/俯视/往下看/向下看/看地面).", {});
  engine.AddMcpTool("self.head.tilt_left", "Tilt robot head left (向左歪头/左偏头/左倾).", {});
  engine.AddMcpTool("self.head.tilt_right", "Tilt robot head right (向右歪头/右偏头/右倾).", {});
  engine.AddMcpTool("self.head.turn_left", "Turn robot head left (向左转头/往左看/左转).", {});
  engine.AddMcpTool("self.head.turn_right", "Turn robot head right (向右转头/往右看/右转).", {});
  engine.AddMcpTool("self.head.bobble", "Cute head bobble and shake (摇头/摇摇头/摇头晃脑/不要/卖萌).", {});
  engine.AddMcpTool("self.head.center", "Reset head to look straight forward (头摆正/正视/头复位/向前看).", {});

  engine.AddMcpTool("self.audio_speaker.set_volume", "Set speaker volume 0-100 (调整音量).", {
    {"volume", ai_vox::ParamSchema<int64_t>{.default_value = std::nullopt, .min = 0, .max = 100}},
  });
  engine.AddMcpTool("self.audio_speaker.get_volume", "Get speaker volume (获取当前音量).", {});
  // Relative siblings of set_volume. The model reaches for these far more readily than it works
  // out an absolute number from get_volume, and they are what a bare "大声一点" should map to.
  engine.AddMcpTool("self.audio_speaker.volume_up",
                    "Increase speaker volume one step (音量增加/调大音量/大声一点/声音大点).", {});
  engine.AddMcpTool("self.audio_speaker.volume_down",
                    "Decrease speaker volume one step (音量减小/调低音量/小声一点/声音小点).", {});

  engine.AddMcpTool("self.screen.set_mode", "Set screen mode (切换屏幕: face 表情, chat 对话).", {
    {"mode", ai_vox::ParamSchema<std::string>{.default_value = "face"}},
  });
  engine.AddMcpTool("self.screen.toggle_mode", "Toggle screen mode between face and chat (切换屏幕显示模式).", {});
}
}  // namespace

void setup() {
  Serial.begin(115200);
  printf("setup\n");

  // Arduino's initArduino() calls esp_log_level_set("*", CORE_DEBUG_LEVEL), which defaults to
  // ESP_LOG_NONE and silences the IDF stack completely. Turn errors back on: without this,
  // esp-tls / esp_websocket_client failures surface only as an opaque WEBSOCKET_EVENT_ERROR.
  esp_log_level_set("*", ESP_LOG_ERROR);

  // Name the culprit if the heap ever runs out instead of just aborting.
  heap_caps_register_failed_alloc_callback(OnHeapAllocFailed);

  // Reserve the Opus codec state first, before LVGL, WiFi and mbedTLS have had a chance to carve up
  // the internal heap. It needs a ~24KB *contiguous* block, which simply does not exist any more by
  // the time the first "listen" starts on this no-PSRAM ESP32 - that failed allocation was the
  // cause of the original reboot loop.
  opus_codec_pool::Preallocate();

  // Immediately initialize MG92B servos to 90 degrees
  ServoController::GetInstance().Init();

  pinMode(kLedPin, OUTPUT);
  digitalWrite(kLedPin, LOW);

  printf("init button\n");
  const button_config_t btn_cfg = {
      .long_press_time = 1000,
      .short_press_time = 50,
  };

  const button_gpio_config_t gpio_cfg = {
      .gpio_num = kButtonBoot,
      .active_level = 0,
      .enable_power_save = false,
      .disable_pull = false,
  };

  ESP_ERROR_CHECK(iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &g_button_boot_handle));
  LogHeap("after button");

  InitDisplay();
  LogHeap("after display+lvgl");
  g_display->ShowStatus("初始化");
  // Before the engine starts, so the first thing she says is already at the user's chosen level.
  LoadSavedVolume();
  ConfigureWifi();
  InitMcpTools();
  LogHeap("after mcp tools");

#if AUDIO_INPUT_DEVICE_TYPE == AUDIO_INPUT_DEVICE_TYPE_I2S_STD
  auto audio_input_device = std::make_shared<ai_vox::AudioInputDeviceI2sStd>(kMicPinSck, kMicPinWs, kMicPinSd);
#elif AUDIO_INPUT_DEVICE_TYPE == AUDIO_INPUT_DEVICE_TYPE_PDM
  auto audio_input_device = std::make_shared<ai_vox::PdmAudioInputDevice>(kMicPinSck, kMicPinSd);
#endif
  auto& ai_vox_engine = ai_vox::Engine::GetInstance();
  ai_vox_engine.SetObserver(g_observer);
  ai_vox_engine.SetOtaUrl("https://api.tenclass.net/xiaozhi/ota/");
  ai_vox_engine.ConfigWebsocket("wss://api.tenclass.net/xiaozhi/v1/",
                                {
                                    {"Authorization", "Bearer test-token"},
                                });
  printf("engine starting\n");
  g_display->ShowStatus("AI引擎启动中");
  LogHeap("before engine start");

  ai_vox_engine.Start(audio_input_device, g_audio_output_device);

  printf("engine started\n");
  LogHeap("after engine start");

  ESP_ERROR_CHECK(iot_button_register_cb(
      g_button_boot_handle,
      BUTTON_PRESS_DOWN,
      nullptr,
      [](void* button_handle, void* usr_data) {
        printf("boot button pressed\n");
        ai_vox::Engine::GetInstance().Advance();
      },
      nullptr));

  ESP_ERROR_CHECK(iot_button_register_cb(
      g_button_boot_handle,
      BUTTON_DOUBLE_CLICK,
      nullptr,
      [](void* button_handle, void* usr_data) {
        printf("boot button double clicked: toggle UI mode\n");
        if (g_display) {
          g_display->ToggleUiMode();
        }
      },
      nullptr));

  ESP_ERROR_CHECK(iot_button_register_cb(
      g_button_boot_handle,
      BUTTON_LONG_PRESS_START,
      nullptr,
      [](void* button_handle, void* usr_data) {
        printf("boot button long pressed: starting debug console\n");
        EnsureDebugServerStarted();
      },
      nullptr));

  g_display->ShowStatus("AI引擎已启动");
}

// Every task stack is DRAM permanently taken away from the heap, and on this no-PSRAM board the
// audio engines alone hold a shared 24KB stack that was sized by guesswork. `unused` below is the
// stack high-water mark: whatever is reported there can be handed straight back to the heap by
// shrinking the corresponding stack constant. Only tasks that currently exist are listed, so the
// audio rows appear while listening/speaking and disappear in between.
void ReportMemory() {
  static const char* const kInterestingTasks[] = {
      "AiVoxMain",
      "AiVoxNetwork",
      "AudioInput",
      "AudioOutput",
      "servo_anim",
      "taskLVGL",
      "loopTask",
      "websocket_task",
  };

  printf("[mem] free: %u, largest: %u, min ever: %u\n",
         static_cast<unsigned>(esp_get_free_heap_size()),
         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
         static_cast<unsigned>(esp_get_minimum_free_heap_size()));
  for (const char* name : kInterestingTasks) {
    const TaskHandle_t handle = xTaskGetHandle(name);
    if (handle == nullptr) {
      continue;
    }
    printf("[mem]   task %-15s unused stack: %u\n", name, static_cast<unsigned>(uxTaskGetStackHighWaterMark(handle)));
  }
}

void loop() {
  static uint32_t s_last_mem_report = 0;
  if (s_last_mem_report == 0 || millis() - s_last_mem_report >= 10000) {
    s_last_mem_report = millis();
    ReportMemory();
  }
#ifdef PRINT_HEAP_INFO_INTERVAL
  static uint32_t s_print_heap_info_time = 0;
  if (s_print_heap_info_time == 0 || millis() - s_print_heap_info_time >= PRINT_HEAP_INFO_INTERVAL) {
    s_print_heap_info_time = millis();
    PrintMemInfo();
  }
#endif

  auto& engine = ai_vox::Engine::GetInstance();

  // Commit a changed volume once the dust has settled. Waiting for a quiet moment keeps the
  // flash-cache stall an NVS write causes out of the audio path, and the delay also coalesces a
  // burst of "再大声一点...再大声一点" into a single write.
  if (g_volume_dirty && g_chat_state != ai_vox::ChatState::kSpeaking && millis() - g_last_volume_exec_time >= 3000) {
    FlushVolumeToNvs();
  }

  const auto events = g_observer->PopEvents();

  for (auto& event : events) {
    if (auto text_received_event = std::get_if<ai_vox::TextReceivedEvent>(&event)) {
      printf("on text received: %s\n", text_received_event->content.c_str());
    } else if (auto activation_event = std::get_if<ai_vox::ActivationEvent>(&event)) {
      printf("activation code: %s, message: %s\n", activation_event->code.c_str(), activation_event->message.c_str());
      g_display->ShowStatus("激活设备");
      g_display->SetChatMessage(Display::Role::kSystem, activation_event->message);
    } else if (auto state_changed_event = std::get_if<ai_vox::StateChangedEvent>(&event)) {
      // Recorded before the switch, which does not have a case for every state: the deferred NVS
      // write below needs to know whether audio is currently playing.
      g_chat_state = state_changed_event->new_state;
      switch (state_changed_event->new_state) {
        case ai_vox::ChatState::kIdle: {
          printf("Idle\n");
          break;
        }
        case ai_vox::ChatState::kInitted: {
          printf("Initted\n");
          g_display->ShowStatus("初始化完成");
          break;
        }
        case ai_vox::ChatState::kLoading: {
          printf("Loading...\n");
          g_display->ShowStatus("加载协议中");
          break;
        }
        case ai_vox::ChatState::kLoadingFailed: {
          printf("Loading failed, please retry\n");
          // Almost always an out-of-memory failure in the mbedTLS handshake rather than a network
          // problem, so record how much heap was actually available.
          LogHeap("protocol load FAILED");
          g_display->ShowStatus("加载协议失败，请重试");
          break;
        }
        case ai_vox::ChatState::kStandby: {
          printf("Standby -> auto advance to connect & listen\n");
          g_display->ShowStatus("待命");
          LogHeap("standby");
          // Automatically advance to connect and start listening without requiring button press
          engine.Advance();
          break;
        }
        case ai_vox::ChatState::kConnecting: {
          printf("Connecting...\n");
          g_display->ShowStatus("连接中...");
          break;
        }
        case ai_vox::ChatState::kListening: {
          printf("Listening...\n");
          g_display->ShowStatus("聆听中");
          LogHeap("listening");
          break;
        }
        case ai_vox::ChatState::kSpeaking: {
          printf("Speaking...\n");
          g_display->ShowStatus("说话中");
          break;
        }
        default: {
          break;
        }
      }
    } else if (auto emotion_event = std::get_if<ai_vox::EmotionEvent>(&event)) {
      printf("emotion: %s\n", emotion_event->emotion.c_str());
      g_display->SetEmotion(emotion_event->emotion);
    } else if (auto chat_message_event = std::get_if<ai_vox::ChatMessageEvent>(&event)) {
      switch (chat_message_event->role) {
        case ai_vox::ChatRole::kAssistant: {
          printf("role: assistant, content: %s\n", chat_message_event->content.c_str());
          g_display->SetChatMessage(Display::Role::kAssistant, chat_message_event->content);
          // If MCP tool was not called for this query, trigger speech fallback motion
          if (!g_last_user_query.empty()) {
            // Volume first, and only on what the user actually said. A phrase that turned out to
            // be a volume command is not also a head command, so don't let both fire.
            if (!CheckAndExecuteVolumeFallback(g_last_user_query)) {
              CheckAndExecuteMotionFallback(g_last_user_query);
            }
            g_last_user_query.clear();
          } else {
            CheckAndExecuteMotionFallback(chat_message_event->content);
          }
          break;
        }
        case ai_vox::ChatRole::kUser: {
          printf("role: user, content: %s\n", chat_message_event->content.c_str());
          g_display->SetChatMessage(Display::Role::kUser, chat_message_event->content);
          g_last_user_query = chat_message_event->content;
          break;
        }
      }
    } else if (auto mcp_tool_call_event = std::get_if<ai_vox::McpToolCallEvent>(&event)) {
      printf("on mcp tool call: %s\n", mcp_tool_call_event->ToString().c_str());
      g_last_user_query.clear();  // MCP tool call received, clear query to avoid duplicate fallback
      const std::string& name = mcp_tool_call_event->name;

      auto matches = [&](const char* target, const char* alt = nullptr) {
        if (name == target) return true;
        if (alt != nullptr && name == alt) return true;
        size_t t_len = strlen(target);
        if (name.length() >= t_len && name.compare(name.length() - t_len, t_len, target) == 0) return true;
        if (alt != nullptr) {
          size_t a_len = strlen(alt);
          if (name.length() >= a_len && name.compare(name.length() - a_len, a_len, alt) == 0) return true;
        }
        return false;
      };

      if (matches("self.audio_speaker.set_volume", "set_volume")) {
        const auto volume_ptr = mcp_tool_call_event->param<int64_t>("volume");
        if (volume_ptr != nullptr) {
          printf("on mcp tool call: set_volume, volume: %" PRId64 "\n", *volume_ptr);
          // Via SetVolume() rather than the device directly, so the screen and NVS keep up.
          SetVolume(static_cast<int>(*volume_ptr));
          g_last_volume_exec_time = millis();
          engine.SendMcpCallResponse(mcp_tool_call_event->id, true);
        } else {
          engine.SendMcpCallError(mcp_tool_call_event->id, "Missing valid argument: volume");
        }
      } else if (matches("self.audio_speaker.volume_up", "volume_up") ||
                 matches("self.audio_speaker.volume_down", "volume_down")) {
        const bool up = name.find("volume_up") != std::string::npos;
        // The step is undeclared but honoured if the model sends one, matching the head tools.
        // Its sign is ignored: models routinely send step=-10 to volume_down, and applying that
        // literally would turn "quieter" into "louder".
        int step = kVolumeStep;
        const auto step_ptr = mcp_tool_call_event->param<int64_t>("step");
        if (step_ptr != nullptr && *step_ptr != 0) {
          step = std::abs(static_cast<int>(*step_ptr));
        }
        const auto volume = AdjustVolume(up ? step : -step);
        printf("on mcp tool call: %s (%d) -> %u\n", up ? "volume_up" : "volume_down", step, static_cast<unsigned>(volume));
        g_last_volume_exec_time = millis();
        engine.SendMcpCallResponse(mcp_tool_call_event->id, static_cast<int64_t>(volume));
      } else if (matches("self.audio_speaker.get_volume", "get_volume")) {
        const auto volume = g_audio_output_device->volume();
        printf("on mcp tool call: get_volume, volume: %" PRIu16 "\n", volume);
        engine.SendMcpCallResponse(mcp_tool_call_event->id, volume);
      } else if (matches("self.head.look_up", "look_up") || name.find("head_up") != std::string::npos) {
        float step = 10.0f;
        const auto step_ptr = mcp_tool_call_event->param<int64_t>("step");
        if (step_ptr != nullptr) step = static_cast<float>(*step_ptr);
        printf("on mcp tool call: look_up (%.1f deg)\n", step);
        engine.SendMcpCallResponse(mcp_tool_call_event->id, true);
        g_last_motion_exec_time = millis();
        if (g_display) {
          g_display->UpdateRobotFaceEmotion("happy");
          g_display->LookDirection("up");
          g_display->ShowStatus("抬头中...");
        }
        ServoController::GetInstance().LookUp(step);
      } else if (matches("self.head.look_down", "look_down") || name.find("head_down") != std::string::npos) {
        float step = 10.0f;
        const auto step_ptr = mcp_tool_call_event->param<int64_t>("step");
        if (step_ptr != nullptr) step = static_cast<float>(*step_ptr);
        printf("on mcp tool call: look_down (%.1f deg)\n", step);
        engine.SendMcpCallResponse(mcp_tool_call_event->id, true);
        g_last_motion_exec_time = millis();
        if (g_display) {
          g_display->UpdateRobotFaceEmotion("thinking");
          g_display->LookDirection("down");
          g_display->ShowStatus("低头中...");
        }
        ServoController::GetInstance().LookDown(step);
      } else if (matches("self.head.tilt_left", "tilt_left")) {
        float step = 10.0f;
        const auto step_ptr = mcp_tool_call_event->param<int64_t>("step");
        if (step_ptr != nullptr) step = static_cast<float>(*step_ptr);
        printf("on mcp tool call: tilt_left (%.1f deg)\n", step);
        engine.SendMcpCallResponse(mcp_tool_call_event->id, true);
        g_last_motion_exec_time = millis();
        if (g_display) {
          g_display->UpdateRobotFaceEmotion("winking");
          g_display->LookDirection("left");
          g_display->ShowStatus("向左歪头...");
        }
        ServoController::GetInstance().TiltLeft(step);
      } else if (matches("self.head.tilt_right", "tilt_right")) {
        float step = 10.0f;
        const auto step_ptr = mcp_tool_call_event->param<int64_t>("step");
        if (step_ptr != nullptr) step = static_cast<float>(*step_ptr);
        printf("on mcp tool call: tilt_right (%.1f deg)\n", step);
        engine.SendMcpCallResponse(mcp_tool_call_event->id, true);
        g_last_motion_exec_time = millis();
        if (g_display) {
          g_display->UpdateRobotFaceEmotion("winking");
          g_display->LookDirection("right");
          g_display->ShowStatus("向右歪头...");
        }
        ServoController::GetInstance().TiltRight(step);
      } else if (matches("self.head.turn_left", "turn_left")) {
        float step = 10.0f;
        const auto step_ptr = mcp_tool_call_event->param<int64_t>("step");
        if (step_ptr != nullptr) step = static_cast<float>(*step_ptr);
        printf("on mcp tool call: turn_left (%.1f deg)\n", step);
        engine.SendMcpCallResponse(mcp_tool_call_event->id, true);
        g_last_motion_exec_time = millis();
        if (g_display) {
          g_display->UpdateRobotFaceEmotion("surprised");
          g_display->LookDirection("left");
          g_display->ShowStatus("向左转头...");
        }
        ServoController::GetInstance().TurnLeft(step);
      } else if (matches("self.head.turn_right", "turn_right")) {
        float step = 10.0f;
        const auto step_ptr = mcp_tool_call_event->param<int64_t>("step");
        if (step_ptr != nullptr) step = static_cast<float>(*step_ptr);
        printf("on mcp tool call: turn_right (%.1f deg)\n", step);
        engine.SendMcpCallResponse(mcp_tool_call_event->id, true);
        g_last_motion_exec_time = millis();
        if (g_display) {
          g_display->UpdateRobotFaceEmotion("surprised");
          g_display->LookDirection("right");
          g_display->ShowStatus("向右转头...");
        }
        ServoController::GetInstance().TurnRight(step);
      } else if (matches("self.head.bobble", "bobble") || matches("self.servo.bobble", "shake")) {
        printf("on mcp tool call: bobble\n");
        engine.SendMcpCallResponse(mcp_tool_call_event->id, true);
        g_last_motion_exec_time = millis();
        if (g_display) {
          g_display->UpdateRobotFaceEmotion("laughing");
          g_display->ShowStatus("摇头晃脑...");
        }
        ServoController::GetInstance().TriggerHeadBobble();
      } else if (matches("self.head.center", "center") || matches("self.servo.center", "reset")) {
        printf("on mcp tool call: center\n");
        engine.SendMcpCallResponse(mcp_tool_call_event->id, true);
        g_last_motion_exec_time = millis();
        if (g_display) {
          g_display->UpdateRobotFaceEmotion("neutral");
          g_display->LookDirection("center");
          g_display->ShowStatus("头已正视");
        }
        ServoController::GetInstance().CenterAll();
      } else if (matches("self.servo.set_angle", "set_angle")) {
        const auto pin_ptr = mcp_tool_call_event->param<int64_t>("pin");
        const auto angle_ptr = mcp_tool_call_event->param<int64_t>("angle");
        if (pin_ptr != nullptr && angle_ptr != nullptr) {
          printf("on mcp tool call: self.servo.set_angle, pin: %lld, angle: %lld\n", *pin_ptr, *angle_ptr);
          engine.SendMcpCallResponse(mcp_tool_call_event->id, true);
          g_last_motion_exec_time = millis();
          ServoController::GetInstance().MoveAngleSmooth(static_cast<int>(*pin_ptr), static_cast<float>(*angle_ptr));
        } else {
          engine.SendMcpCallError(mcp_tool_call_event->id, "Missing pin or angle");
        }
      } else if (matches("self.screen.set_mode", "set_mode")) {
        const auto mode_ptr = mcp_tool_call_event->param<std::string>("mode");
        if (mode_ptr != nullptr && (*mode_ptr == "chat" || *mode_ptr == "text")) {
          g_display->SetUiMode(Display::UiMode::kChatText);
        } else {
          g_display->SetUiMode(Display::UiMode::kRobotFace);
        }
        engine.SendMcpCallResponse(mcp_tool_call_event->id, true);
      } else if (matches("self.screen.toggle_mode", "toggle_mode")) {
        g_display->ToggleUiMode();
        engine.SendMcpCallResponse(mcp_tool_call_event->id, true);
      }
    }
  }

  // Handle Web UI requests for servo testing
  ServoWebServer::GetInstance().HandleClient();
  delay(2);
}