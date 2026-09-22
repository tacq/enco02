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
#include "cam_link.h"
#include "servo_controller.h"
#include "servo_web_server.h"
#include "display.h"
#include "volume_command.h"
#include "timer_command.h"
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

// ---------------------------------------------------------------------------
// Countdown timer
//
// Runs entirely on the device. The websocket session is torn down between turns, so a timer the
// cloud model "remembered" would not survive the user simply not talking for five minutes - which
// is exactly what someone who just set a five minute timer is about to do.
// ---------------------------------------------------------------------------

bool g_timer_running = false;
uint32_t g_timer_deadline = 0;         // millis() value at which it fires
uint32_t g_timer_total_seconds = 0;    // what was asked for, so the announcement can name it
int32_t g_timer_shown_remaining = -1;  // last value painted, so identical seconds are not repainted
// Set when the deadline passes. Announcing is a separate step from firing because the sentence can
// only be injected while the engine is listening, which it may well not be at that exact moment.
bool g_timer_finished = false;
bool g_timer_announced = false;
uint32_t g_timer_finished_at = 0;
uint32_t g_timer_last_announce_try = 0;
uint32_t g_last_timer_exec_time = 0;

// How long "时间到" stays in the status bar before the bar goes back to normal on its own.
constexpr uint32_t kTimerFinishedHoldMs = 60000;
constexpr uint32_t kTimerAnnounceRetryMs = 1500;
// Stop retrying eventually: an alarm half a minute late is worse than no alarm, and the screen has
// been saying 时间到 the whole time anyway.
constexpr uint32_t kTimerAnnounceGiveUpMs = 30000;
// The announcement is delivered down the wake-word channel, and the server polices what that is
// allowed to carry: a full sentence comes back as
//   {"type":"alert","status":"ERROR","message":"Detect is only for wake words, do not send long
//    texts."}
// and is never spoken. So the injected phrase has to stay wake-word sized; anything longer than
// this many UTF-8 characters falls back to a fixed short one.
constexpr size_t kTimerAnnounceMaxChars = 10;

// ---------------------------------------------------------------------------
// Event notification card
// ---------------------------------------------------------------------------
// The card itself lives in Display (ShowAlert/HideAlert); this is just its lifetime. One slot, not
// a queue: Display::ShowAlert() deliberately rewrites the existing card rather than stacking a
// second one, so a second event supersedes the first and a single deadline is all that is needed.
//
// Everything that raises a card goes through ShowNotification() so that nothing can put one up
// without also arranging for it to come down - the first cut of this wired ShowAlert() straight
// into the timer and the card then sat on screen forever.
bool g_alert_visible = false;
uint32_t g_alert_expires_at = 0;
constexpr uint32_t kAlertDefaultHoldMs = 60000;
// Two minutes. Long enough for a reminder the user walked away from, short enough that a stale card
// is not still claiming the screen an hour later.
constexpr uint32_t kAlertMaxHoldMs = 120000;

// The only sanctioned way to raise the card. hold_ms is clamped rather than rejected so a model
// that asks for a day-long notification gets a reasonable one instead of none.
void ShowNotification(const char* title, const char* body, uint32_t hold_ms) {
  if (g_display == nullptr) {
    return;
  }
  if (hold_ms == 0 || hold_ms > kAlertMaxHoldMs) {
    hold_ms = kAlertMaxHoldMs;
  }
  // Display::ShowAlert() can decline (viewfinder up, or free heap under its guard). Mark the slot
  // occupied anyway: HideAlert() is a no-op on a card that was never built, and the alternative is
  // a flag that says "nothing on screen" while a previous card is in fact still up.
  g_display->ShowAlert(title, body);
  g_alert_visible = true;
  g_alert_expires_at = millis() + hold_ms;
  printf("[alert] %s | %s (%us)\n", title, body, static_cast<unsigned>(hold_ms / 1000));
}

void ClearNotification() {
  if (g_display != nullptr) {
    g_display->HideAlert();
  }
  g_alert_visible = false;
}

// Called from loop(), next to TimerTick().
void NotificationTick() {
  // Signed subtraction, so this survives the 49-day millis() wrap like the countdown does.
  if (g_alert_visible && static_cast<int32_t>(g_alert_expires_at - millis()) <= 0) {
    ClearNotification();
  }
}

// Characters, not bytes: a Chinese character is three bytes, so strlen() would put even a five
// character phrase well over any sane wake-word budget.
size_t Utf8Length(const std::string& s) {
  size_t count = 0;
  for (const char ch : s) {
    if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80) {  // Skip continuation bytes.
      ++count;
    }
  }
  return count;
}

// "5分钟" / "1小时30分钟". Only ever spoken, never displayed.
std::string DescribeDuration(const uint32_t total_seconds) {
  const unsigned hours = static_cast<unsigned>(total_seconds / 3600);
  const unsigned minutes = static_cast<unsigned>((total_seconds % 3600) / 60);
  const unsigned seconds = static_cast<unsigned>(total_seconds % 60);
  char text[48];
  if (hours > 0 && minutes > 0) {
    snprintf(text, sizeof(text), "%u小时%u分钟", hours, minutes);
  } else if (hours > 0) {
    snprintf(text, sizeof(text), "%u小时", hours);
  } else if (minutes > 0 && seconds > 0) {
    snprintf(text, sizeof(text), "%u分%u秒", minutes, seconds);
  } else if (minutes > 0) {
    snprintf(text, sizeof(text), "%u分钟", minutes);
  } else {
    snprintf(text, sizeof(text), "%u秒", seconds);
  }
  return text;
}

void StartTimer(const uint32_t seconds) {
  const uint32_t clamped = std::clamp<uint32_t>(seconds, 1, kTimerMaxSeconds);
  g_timer_running = true;
  g_timer_finished = false;
  g_timer_announced = false;
  g_timer_total_seconds = clamped;
  g_timer_deadline = millis() + clamped * 1000;
  g_timer_shown_remaining = static_cast<int32_t>(clamped);
  g_last_timer_exec_time = millis();
  if (g_display) {
    g_display->ShowTimer(clamped);
  }
  printf("[timer] started: %u seconds\n", static_cast<unsigned>(clamped));
}

// Returns whether there was anything to cancel, so the tool call can answer honestly.
bool CancelTimer() {
  const bool was_active = g_timer_running || g_timer_finished;
  g_timer_running = false;
  g_timer_finished = false;
  g_timer_announced = true;  // Nothing left to say.
  g_timer_shown_remaining = -1;
  g_last_timer_exec_time = millis();
  if (g_display) {
    g_display->HideTimer();
  }
  // Cancel can land inside the post-fire hold window, so the card may still be up.
  ClearNotification();
  if (was_active) {
    printf("[timer] cancelled\n");
  }
  return was_active;
}

uint32_t TimerRemainingSeconds() {
  if (!g_timer_running) {
    return 0;
  }
  const int32_t remaining_ms = static_cast<int32_t>(g_timer_deadline - millis());
  if (remaining_ms <= 0) {
    return 0;
  }
  return static_cast<uint32_t>((remaining_ms + 999) / 1000);
}

// Driven from loop(). Deliberately not from the display's own LVGL timer: that one early-returns
// unless the character view is on screen, so a countdown hung off it would freeze in chat mode.
void TimerTick() {
  if (g_timer_running) {
    // Signed subtraction, so this still works across the 49-day millis() wrap.
    const int32_t remaining_ms = static_cast<int32_t>(g_timer_deadline - millis());
    if (remaining_ms <= 0) {
      g_timer_running = false;
      g_timer_finished = true;
      g_timer_announced = false;
      g_timer_finished_at = millis();
      g_timer_last_announce_try = 0;
      g_timer_shown_remaining = -1;
      printf("[timer] fired after %u seconds\n", static_cast<unsigned>(g_timer_total_seconds));
      if (g_display) {
        g_display->ShowTimerFinished();
        g_display->UpdateRobotFaceEmotion("surprised");
      }
      // The event card. The status bar only has room for 时间到, which does not say *which*
      // timer - and "定个五分钟的" followed by "再定个十分钟的" is an ordinary thing to ask for.
      // DescribeDuration() is the same string the spoken announcement below uses, so the screen
      // and the speaker name the timer identically.
      // No emoji in the title: font_puhui_16_4 is a CJK + Latin subset with no pictographs, and
      // LV_USE_FONT_PLACEHOLDER is on, so a "⏱" would render as a hollow box.
      //
      // Held for exactly as long as the status bar keeps saying 时间到, so the two halves of the
      // same alarm appear and disappear together.
      const std::string body = DescribeDuration(g_timer_total_seconds) + "的定时已结束";
      ShowNotification("定时提醒 // 触发", body.c_str(), kTimerFinishedHoldMs);
      // No local MP3 chime here. Firing almost always happens with a websocket session open, and
      // spinning up the mp3 decoder at that moment wants a 2.3KB contiguous block the heap does not
      // have - measured: "ALLOC FAILED #2: 2312 bytes, free: 5960, largest: 1396", which took the
      // low-water mark down to 4.6KB and came uncomfortably close to taking the connection with it.
      // The screen says 时间到 immediately, and the spoken announcement below follows a second later.
    } else {
      // Round up, so a five minute timer reads 05:00 for its first second rather than 04:59.
      const int32_t remaining = (remaining_ms + 999) / 1000;
      if (remaining != g_timer_shown_remaining) {
        g_timer_shown_remaining = remaining;
        if (g_display) {
          g_display->ShowTimer(static_cast<uint32_t>(remaining));
        }
      }
    }
  }

  if (g_timer_finished && !g_timer_announced &&
      (g_timer_last_announce_try == 0 || millis() - g_timer_last_announce_try >= kTimerAnnounceRetryMs)) {
    g_timer_last_announce_try = millis();
    if (millis() - g_timer_finished_at >= kTimerAnnounceGiveUpMs) {
      printf("[timer] gave up on the spoken announcement\n");
      g_timer_announced = true;
    } else {
      // Phrased as something the user said: SendWakeText() hands the server a transcript, and the
      // reply to it is what actually comes out of the speaker.
      //
      // It has to read like a wake word, not a sentence - see kTimerAnnounceMaxChars. "5分钟时间到"
      // is within budget and still tells the assistant which timer went off, so its reply names the
      // duration back to the user.
      std::string text = DescribeDuration(g_timer_total_seconds) + "时间到";
      if (Utf8Length(text) > kTimerAnnounceMaxChars) {
        text = "定时时间到";  // Very long durations; drop the duration rather than be rejected.
      }
      if (ai_vox::Engine::GetInstance().SendWakeText(text)) {
        printf("[timer] announcing: %s\n", text.c_str());
        g_timer_announced = true;
      }
    }
  }

  if (g_timer_finished && millis() - g_timer_finished_at >= kTimerFinishedHoldMs) {
    g_timer_finished = false;
    if (g_display) {
      g_display->HideTimer();
    }
    // The card is not touched here: it was raised with this same hold, so NotificationTick() takes
    // it down on the same tick. Doing it in both places would tear down a *newer* card that some
    // other event raised in the meantime.
  }
}

// Speech fallback, for the same reason as the volume one: asked for a timer in plain words, the
// model frequently just says "好的，五分钟后提醒你" without ever emitting a tool call - and then
// nothing would actually be counting down.
bool CheckAndExecuteTimerFallback(const std::string& query) {
  if (millis() - g_last_timer_exec_time < 2500) {
    return false;  // Debounce, same as the volume and motion fallbacks.
  }

  const auto command = ClassifyTimerCommand(query);
  switch (command.kind) {
    case TimerCommandKind::kStart: {
      printf("[Voice Timer Fallback] start %u seconds\n", static_cast<unsigned>(command.seconds));
      StartTimer(command.seconds);
      return true;
    }
    case TimerCommandKind::kCancel: {
      printf("[Voice Timer Fallback] cancel\n");
      CancelTimer();
      return true;
    }
    case TimerCommandKind::kQuery: {
      // Nothing to do - the countdown is already on screen and the assistant's own reply covers it.
      // Still claim the utterance if a timer is running, so the head fallback does not also fire.
      return g_timer_running;
    }
    default: {
      return false;
    }
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

  // The countdown lives on the device, so these have to be tools rather than something the model
  // keeps in its head - the session does not outlive the turn that created it.
  engine.AddMcpTool("self.timer.start",
                    "Start a countdown timer shown on screen (设置定时器/倒计时/几分钟后提醒我). "
                    "seconds is the total duration in seconds.",
                    {
                        {"seconds", ai_vox::ParamSchema<int64_t>{.default_value = std::nullopt, .min = 1, .max = static_cast<int64_t>(kTimerMaxSeconds)}},
                    });
  engine.AddMcpTool("self.timer.cancel", "Cancel the running countdown timer (取消定时器/停止倒计时/不用提醒了).", {});
  engine.AddMcpTool("self.timer.query", "Seconds left on the countdown timer, 0 if none (还剩多久/定时器还有多长时间).", {});

  // Event notifications. The timer raises one of these by itself when it fires; this is the same
  // card exposed to the model, so anything it decides is worth interrupting the user about - a
  // reminder, a calendar item, a result it looked up - can be put on the screen instead of only
  // being spoken, which is gone the moment it is said.
  //
  // title and body are painted verbatim and must be short: the card is 150x184 px in a 16px font,
  // which is roughly 8 characters of title and four lines of body before it clips.
  //
  // The descriptions below are terse on purpose. Tool text is not free on this board - it is held
  // as a std::string and serialised again into the tool-list JSON sent at session start, and the
  // first, chattier draft of these two measurably took the heap low-water mark from 6,652 to 4,632,
  // which is the same 2KB the UI rework had just recovered. WiFi wants 2308-byte blocks during TTS,
  // so that margin is not decorative. Anything explanatory belongs in a comment like this one.
  //
  // No hold_seconds parameter for the same reason: a third schema entry costs more than it buys
  // when every card wants the same minute on screen anyway.
  engine.AddMcpTool("self.screen.notify",
                    "Show a short note on the robot's screen (提醒我/记一下/通知我). "
                    "title <=8 chars, body <=4 short lines. Clears itself after a minute.",
                    {
                        {"title", ai_vox::ParamSchema<std::string>{.default_value = "提醒"}},
                        {"body", ai_vox::ParamSchema<std::string>{.default_value = std::nullopt}},
                    });
  engine.AddMcpTool("self.screen.notify_clear", "Remove the note from the screen (知道了/取消提醒).", {});



  // The camera is a second board. This device cannot hold a JPEG - with the
  // assistant speaking its largest free block is ~2KB - so the cam does the
  // capture, the upload and the recognition itself and hands back one short
  // sentence. That sentence becomes this tool's result, and the model upstream
  // turns it into an answer. The image never crosses this board.
  engine.AddMcpTool("self.camera.look",
                    "Look through the robot's eye camera and describe what is in front of it or in the user's hand "
                    "(这是什么/我手里拿的是什么/看看我手里是什么/你看到了什么/看一下/帮我看看/前面是什么/what is in my hand). "
                    "Returns a short description. Set question to what you actually want to know about the scene.",
                    {
                        {"question", ai_vox::ParamSchema<std::string>{.default_value = ""}},
                    });
  // Tracking is OFF at boot and after every restart. These two are the only way
  // to turn it on by voice; the other way is to hold up one finger, which the
  // cam spots by itself and reports as a G line.
  engine.AddMcpTool("self.camera.track_on",
                    "Start following the user's head with the robot's head, using the camera "
                    "(开启跟踪/开始跟踪/打开跟踪/跟踪模式/看着我/跟着我/别走神/start tracking/follow me). "
                    "The head turns, nods and tilts to match the user until tracking is stopped.",
                    {});
  engine.AddMcpTool("self.camera.track_off",
                    "Stop following the user and hold the head still "
                    "(关闭跟踪/停止跟踪/结束跟踪/不要跟踪了/别看我了/不用跟着我/头别动/stop tracking).",
                    {});
  // The viewfinder. Separate from look: this one shows the user what the robot
  // sees and leaves it on screen, rather than taking a single picture and going
  // away again. It replaces the character on the display while it is up.
  engine.AddMcpTool("self.camera.view_on",
                    "Show the live camera picture on the robot's screen "
                    "(打开摄像头/开摄像头/显示摄像头/看看摄像头画面/我想看看你看到什么/show the camera).",
                    {});
  engine.AddMcpTool("self.camera.view_off",
                    "Close the camera picture and go back to the robot's face "
                    "(关闭摄像头/关掉摄像头/退出摄像头/不看了/close the camera).",
                    {});
}

// Reads the quoted value of `key` from `text`, searching in [from, limit).
// Returns false if the key is not there or the value is not a string.
//
// A scanner rather than a cJSON parse: this runs against every inbound text
// frame, and the heap on this board has been logged down to a 2,036 byte
// largest free block. The two values we want are a URL and a UUID, neither of
// which can contain an escape, so there is nothing to unescape.
bool ReadJsonString(const std::string& text, size_t from, size_t limit, const char* key, std::string* out) {
  const size_t k = text.find(key, from);
  if (k == std::string::npos || k >= limit) {
    return false;
  }
  size_t i = text.find('"', k + strlen(key));  // opening quote of the value
  if (i == std::string::npos || i >= limit) {
    return false;
  }
  ++i;
  const size_t end = text.find('"', i);
  if (end == std::string::npos) {
    return false;
  }
  out->assign(text, i, end - i);
  return !out->empty();
}

// The server tells us where its own vision service is, in the params of the
// MCP `initialize` call:
//
//   {"type":"mcp","payload":{"jsonrpc":"2.0","method":"initialize","params":{
//      ... "capabilities":{"vision":{"url":"http://.../vision/explain",
//                                    "token":"<uuid>"}}}}}
//
// EngineImpl::OnMcpJsonObj parses `initialize` only far enough to build its
// reply - it never looks at the incoming params - so this is thrown away on
// every connect. Picking it up here means the camera board needs no API key of
// its own: the account that already pays for the assistant pays for the
// picture too, and the URL is plain http, so the cam needs no TLS either.
//
// Read off the raw text frame because main.cpp already receives every one
// verbatim for logging. That keeps the engine untouched.
void MaybeAdoptVisionEndpoint(const std::string& text) {
  // Cheap reject for the tts/stt/llm frames, which are the overwhelming
  // majority and allocate nothing on this path.
  if (text.find("\"initialize\"") == std::string::npos) {
    return;
  }
  const size_t v = text.find("\"vision\"");
  if (v == std::string::npos) {
    return;
  }
  // Bound the search so a "token" belonging to some later capability cannot be
  // paired with the vision url.
  const size_t limit = std::min(v + 400, text.size());

  std::string url;
  if (!ReadJsonString(text, v, limit, "\"url\"", &url)) {
    return;
  }
  std::string token;
  ReadJsonString(text, v, limit, "\"token\"", &token);  // optional
  CamLink::GetInstance().SetVisionEndpoint(url.c_str(), token.c_str());
}

// --- Camera viewfinder -------------------------------------------------------
//
// Two halves that have to move together: Display owns the HUD and the JPEG
// decoder's destination, CamLink owns the wire. Opening is ordered so that a
// failure at either step leaves the screen as it was - there is no half-open
// state where the chrome is up but no picture ever arrives.

// Set when the viewfinder was opened by a "这是什么" rather than by the user
// asking for the camera. A transient view closes itself once the answer is in;
// one the user opened stays up until they say so.
bool g_cam_view_transient = false;
// Video deliberately stopped with the last frame left on the panel. Not a
// fault, so the watchdog below must not treat it as one.
bool g_cam_view_frozen = false;
// When to act on the view next - close it, or thaw it. 0 when nothing pending.
uint32_t g_cam_view_deadline_ms = 0;
uint32_t g_cam_view_stat_ms = 0;
uint16_t g_cam_view_last_frames = 0;

// How long the live picture is shown before the shutter. Enough for a few
// frames to land so the user can see what is in shot and move their hand if it
// is not, and short enough not to feel like the robot is hesitating.
constexpr uint32_t kLookPreviewMs = 700;

// A look waiting for its preview to finish. The tool call has already been
// accepted at this point, so whatever happens, this id must eventually be
// answered.
struct PendingLook {
  bool active = false;
  int64_t id = 0;
  std::string question;
  uint32_t fire_at_ms = 0;
};
PendingLook g_pending_look;

// `why`, when given, receives a caller-facing reason for a false return. The
// distinction matters: the assistant reads it aloud, and "not enough memory"
// sent the user hunting for a heap problem when the real fault was the cam
// missing a baud handshake.
bool OpenCameraView(bool transient, const char** why = nullptr) {
  if (!g_display) {
    if (why) *why = "Display not ready";
    return false;
  }
  auto& cam = CamLink::GetInstance();
  if (!cam.IsPresent()) {
    if (why) *why = "Camera not connected";
    return false;
  }
  if (g_display->InCameraView()) {
    // Already up. A look that arrives while the user is watching the live feed
    // must not mark the view transient, or it would close their camera for them.
    g_cam_view_deadline_ms = 0;
    return true;
  }
  if (!g_display->EnterCameraView()) {
    printf("camera view: display refused (free heap %u)\n", static_cast<unsigned>(esp_get_free_heap_size()));
    if (why) *why = "Not enough memory for the camera view";
    return false;
  }
  if (!cam.BeginVideo()) {
    g_display->ExitCameraView();
    if (why) *why = "Camera link did not start the video stream";
    return false;
  }
  g_cam_view_transient = transient;
  g_cam_view_frozen = false;
  g_cam_view_deadline_ms = 0;
  g_cam_view_stat_ms = 0;
  g_cam_view_last_frames = 0;
  return true;
}

void CloseCameraView() {
  if (!g_display) {
    return;
  }
  CamLink::GetInstance().EndVideo();
  if (g_display->InCameraView()) {
    g_display->ExitCameraView();
  }
  g_cam_view_transient = false;
  g_cam_view_frozen = false;
  g_cam_view_deadline_ms = 0;
}

// Stops the stream and leaves the last frame on the panel - which is the photo,
// as far as the user is concerned.
//
// This is not cosmetic. The cam stops servicing its UART for the several
// seconds it spends uploading and waiting on the vision service, so a `B 0`
// sent after the `V` would sit unread while the two boards ran at different
// rates, and the answer would come back at 115200 into a port listening at
// 921600. Dropping the link speed first is what makes the reply readable.
void FreezeCameraView() {
  CamLink::GetInstance().EndVideo();
  g_cam_view_frozen = true;
  if (g_display) {
    g_display->SetCameraCapturing(true);
  }
}

void ThawCameraView() {
  if (!g_display || !g_display->InCameraView()) {
    return;
  }
  g_display->SetCameraHint(nullptr);
  g_display->SetCameraCapturing(false);
  g_cam_view_frozen = false;
  g_cam_view_stat_ms = 0;
  g_cam_view_last_frames = 0;
  if (!CamLink::GetInstance().BeginVideo()) {
    CloseCameraView();
  }
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

  // Not here: esp_bt_controller_mem_release(ESP_BT_MODE_BTDM). Nothing uses Bluetooth, so handing
  // its DRAM back looks like free money on a board with no PSRAM. It is not - measured on device,
  // the call returns ESP_OK and moves the heap by exactly zero bytes ("free 212284 -> 212284").
  // This build simply never reserves the region. Do not spend time on it again.

  // Reserve the Opus codec state first, before LVGL, WiFi and mbedTLS have had a chance to carve up
  // the internal heap. It needs a ~24KB *contiguous* block, which simply does not exist any more by
  // the time the first "listen" starts on this no-PSRAM ESP32 - that failed allocation was the
  // cause of the original reboot loop.
  opus_codec_pool::Preallocate();

  // Immediately initialize MG92B servos to 90 degrees
  ServoController::GetInstance().Init();

  // Opens UART2 towards the ESP32-CAM. Done here, not lazily: begin() allocates
  // the driver's 256 byte RX ring, and at this point ~180KB is free. Deferring
  // it would mean asking for that memory mid-session, when the largest
  // contiguous block has been measured at 2,036 bytes.
  CamLink::GetInstance().Init();

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

  // Tool registration moved ahead of ConfigureWifi(): it has no network dependency, and the engine
  // singleton it builds is better allocated while the heap is still clean than after WiFi and TLS
  // have carved it up.
  //
  // The GetInstance() below is not redundant - it forces the singleton up *before* the probe.
  // InitMcpTools() touches Engine::GetInstance() on its first line, and EngineImpl's constructor
  // spawns AiVoxMain (5KB stack) and AiVoxNetwork (6KB stack) and reserves the 5KB tools buffer.
  // With those charged to the bracket the tool table measured 20,520 bytes and looked like a leak.
  // Measured properly it is *zero*: every AddTool() serialises into the buffer the engine already
  // reserved, and the cJSON temporaries are freed. Descriptions cost flash, not heap - so do not
  // trim them hoping to recover RAM.
  (void)ai_vox::Engine::GetInstance();
  LogHeap("before mcp tools");
  InitMcpTools();
  LogHeap("after mcp tools");

  ConfigureWifi();

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

  // Wi-Fi strength into the status bar. Polled every 2s rather than driven off WiFi events on
  // purpose: the failure this board actually suffers is not a disconnect but a link that stays
  // associated while the signal collapses - which is what "network latency high: 3039 ms" and the
  // websocket errors in the log are. An event-driven indicator would sit on four bars throughout.
  static uint32_t s_last_net_report = 0;
  static int s_last_rssi_logged = 0;
  if (g_display != nullptr && (s_last_net_report == 0 || millis() - s_last_net_report >= 2000)) {
    s_last_net_report = millis();
    const bool up = (WiFi.status() == WL_CONNECTED);
    const int rssi = up ? WiFi.RSSI() : 0;
    g_display->ShowNetwork(up, rssi);
    // Logged on a 5dB step, not every poll: the thresholds in ShowNetwork() are only as good as the
    // RSSI this room actually produces, and a 2s spam of unchanged numbers buries everything else.
    if (abs(rssi - s_last_rssi_logged) >= 5) {
      s_last_rssi_logged = rssi;
      printf("[wifi] rssi %d dBm\n", rssi);
    }
  }


  // Commit a changed volume once the dust has settled. Waiting for a quiet moment keeps the
  // flash-cache stall an NVS write causes out of the audio path, and the delay also coalesces a
  // burst of "再大声一点...再大声一点" into a single write.
  if (g_volume_dirty && g_chat_state != ai_vox::ChatState::kSpeaking && millis() - g_last_volume_exec_time >= 3000) {
    FlushVolumeToNvs();
  }

  // Before the event pump, so a countdown that expires on this pass gets its announcement in while
  // the engine state is still whatever the last event left it as.
  TimerTick();
  NotificationTick();

  // Drains at most a few bytes of UART and, at most once per 60ms, nudges a
  // servo. Nothing here allocates.
  auto& cam = CamLink::GetInstance();
  cam.Poll();

  // The cam arms tracking by itself when it sees one raised finger. Say so, or
  // the head starting to move looks like a fault.
  if (cam.TakeGestureArmed() && g_display) {
    g_display->ShowStatus("跟踪开启");
  }

  // A "这是什么?" answer coming back from the camera board, seconds after the
  // tool call that asked for it. tools/call is asynchronous - the engine took
  // the id, and this is where we finally redeem it.
  {
    int64_t look_id = 0;
    bool look_ok = false;
    const char* look_text = nullptr;
    if (cam.TakeLookResult(&look_id, &look_ok, &look_text)) {
      printf("cam look result (%s): %s\n", look_ok ? "ok" : "err", look_text);
      if (look_ok) {
        // The description, not a finished sentence: the server's model reads
        // this and phrases the reply itself, so it comes out in the assistant's
        // own voice rather than as a readout.
        engine.SendMcpCallResponse(look_id, std::string(look_text));
      } else {
        engine.SendMcpCallError(look_id, std::string(look_text));
      }
      if (g_display && g_display->InCameraView()) {
        // Put what the camera decided on the frozen frame. The spoken reply is
        // the server's job and arrives a second or two later; this is so the
        // user can see the recognition landed on the right object.
        g_display->SetCameraHint(look_text);
        g_display->SetCameraCapturing(false);
        // Either close the detour or thaw the picture, 2.5s from now: long
        // enough to read a short line, short enough that the robot is back to
        // normal before the assistant has finished speaking.
        g_cam_view_deadline_ms = millis() + 2500;
      }
    }
  }

  // The deferred half of a look. The preview has been on screen for
  // kLookPreviewMs, so now the picture is frozen and the request goes out.
  //
  // Why it is deferred at all: the cam stops reading its UART for the several
  // seconds it spends uploading, so every command needed for this exchange has
  // to arrive before the `V` does. FreezeCameraView() drops the link back to
  // 115200 first; sending `V` while still at 921600 would have the answer come
  // back at a rate this board was no longer listening at.
  if (g_pending_look.active && static_cast<int32_t>(millis() - g_pending_look.fire_at_ms) >= 0) {
    g_pending_look.active = false;
    FreezeCameraView();
    if (g_display) {
      g_display->SetCameraHint("识别中…");
    }
    if (!cam.RequestLook(g_pending_look.id, g_pending_look.question.c_str())) {
      engine.SendMcpCallError(g_pending_look.id, "Camera is busy");
      CloseCameraView();
    }
    g_pending_look.question.clear();
  }

  // Viewfinder housekeeping.
  if (g_display && g_display->InCameraView()) {
    const uint32_t now_ms = millis();

    // The cam went away, the link got too noisy, or the picture stalled - all
    // of which CamLink handles by turning video off. There is no point leaving
    // an empty viewfinder on screen once that happens. A frozen view is a
    // different thing entirely: video is off because we turned it off.
    if (!cam.video_active() && !g_cam_view_frozen) {
      printf("camera view: video stopped, closing\n");
      CloseCameraView();
    } else {
      if (now_ms - g_cam_view_stat_ms >= 1000) {
        // Once a second, and only once: lv_label_set_text() invalidates the
        // label whether or not the text changed, and loop() runs hundreds of
        // times in that second.
        g_cam_view_stat_ms = now_ms;
        if (g_cam_view_frozen) {
          g_display->SetCameraTelemetry("HOLD");
        } else {
          // Measured frame rate. Claiming a number here rather than counting
          // one would be the easiest thing in this whole feature to get quietly
          // wrong.
          const uint16_t frames = cam.video_frames();
          char line[32];
          snprintf(line, sizeof(line), "%u FPS · 921K", static_cast<unsigned>(frames - g_cam_view_last_frames));
          g_cam_view_last_frames = frames;
          g_display->SetCameraTelemetry(line);
        }
      }
      if (g_cam_view_deadline_ms != 0 && static_cast<int32_t>(now_ms - g_cam_view_deadline_ms) >= 0) {
        g_cam_view_deadline_ms = 0;
        if (g_cam_view_transient) {
          CloseCameraView();
        } else {
          // The user opened the camera themselves, so it goes back to live
          // rather than away.
          ThawCameraView();
        }
      }
    }
  }

  const auto events = g_observer->PopEvents();

  for (auto& event : events) {
    if (auto text_received_event = std::get_if<ai_vox::TextReceivedEvent>(&event)) {
      printf("on text received: %s\n", text_received_event->content.c_str());
      MaybeAdoptVisionEndpoint(text_received_event->content);
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
            // Timer first, then volume, and only on what the user actually said. Each of these is
            // exclusive: a phrase that turned out to be a timer request is not also a head command,
            // so the first one to claim it wins.
            if (!CheckAndExecuteTimerFallback(g_last_user_query) && !CheckAndExecuteVolumeFallback(g_last_user_query)) {
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

      // Any explicit head command wins over the tracker for a few seconds.
      // Otherwise "向左转头" is obeyed and then silently undone as the tracker
      // drags the head back onto the user's face.
      if (name.find("head") != std::string::npos) {
        cam.NoteManualHeadCommand();
      }

      if (matches("self.camera.look", "take_photo")) {
        // Deliberately does NOT answer here. The cam needs a few seconds to
        // capture, upload and recognise; the id is parked and redeemed at the
        // top of a later loop() pass. Answering now would mean answering
        // before we know anything.
        const auto question_ptr = mcp_tool_call_event->param<std::string>("question");
        const char* question = question_ptr != nullptr ? question_ptr->c_str() : "";
        const int64_t look_id = mcp_tool_call_event->id;

        if (!cam.IsPresent()) {
          engine.SendMcpCallError(look_id, "Camera not connected");
        } else if (g_pending_look.active) {
          engine.SendMcpCallError(look_id, "Camera is already busy");
        } else if (OpenCameraView(/*transient=*/true)) {
          // Show the shot before taking it. The character is 40px tall on a
          // 240x320 panel and says nothing about what is being pointed at;
          // putting the actual frame up is the difference between the robot
          // looking at your hand and the robot appearing to ignore you.
          //
          // The request itself waits for kLookPreviewMs - see the pending-look
          // block in loop() for why it cannot simply be sent now.
          g_pending_look.active = true;
          g_pending_look.id = look_id;
          g_pending_look.question = question;
          g_pending_look.fire_at_ms = millis() + kLookPreviewMs;
          if (g_display) {
            g_display->ShowStatus("看一下...");
            g_display->SetCameraHint("取景中…");
          }
        } else if (cam.RequestLook(look_id, question)) {
          // No viewfinder - not enough heap, or the display refused. The
          // feature still works, it just does it without showing its work.
          if (g_display) {
            g_display->ShowStatus("看一下...");
          }
        } else {
          engine.SendMcpCallError(look_id, "Camera is already busy");
        }
      } else if (matches("self.camera.track_on")) {
        cam.SetTrackingEnabled(true);
        engine.SendMcpCallResponse(mcp_tool_call_event->id, cam.IsPresent());
      } else if (matches("self.camera.track_off")) {
        cam.SetTrackingEnabled(false);
        engine.SendMcpCallResponse(mcp_tool_call_event->id, true);
      } else if (matches("self.camera.view_on")) {
        const char* why = "Camera view unavailable";
        if (OpenCameraView(/*transient=*/false, &why)) {
          engine.SendMcpCallResponse(mcp_tool_call_event->id, true);
        } else {
          engine.SendMcpCallError(mcp_tool_call_event->id, why);
        }
      } else if (matches("self.camera.view_off")) {
        CloseCameraView();
        engine.SendMcpCallResponse(mcp_tool_call_event->id, true);
      } else if (matches("self.audio_speaker.set_volume", "set_volume")) {
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
      } else if (matches("self.timer.start", "start_timer") || matches("self.timer.set", "set_timer")) {
        int64_t seconds = 0;
        const auto seconds_ptr = mcp_tool_call_event->param<int64_t>("seconds");
        if (seconds_ptr != nullptr) {
          seconds = *seconds_ptr;
        } else {
          // Models routinely answer in minutes whatever the schema declares, and a dropped tool
          // call here means the user's timer silently never runs.
          const auto minutes_ptr = mcp_tool_call_event->param<int64_t>("minutes");
          if (minutes_ptr != nullptr) {
            seconds = *minutes_ptr * 60;
          } else {
            const auto duration_ptr = mcp_tool_call_event->param<int64_t>("duration");
            if (duration_ptr != nullptr) {
              seconds = *duration_ptr;
            }
          }
        }
        if (seconds > 0) {
          StartTimer(static_cast<uint32_t>(std::min<int64_t>(seconds, kTimerMaxSeconds)));
          engine.SendMcpCallResponse(mcp_tool_call_event->id, static_cast<int64_t>(g_timer_total_seconds));
        } else {
          engine.SendMcpCallError(mcp_tool_call_event->id, "Missing valid argument: seconds");
        }
      } else if (matches("self.timer.cancel", "cancel_timer") || matches("self.timer.stop", "stop_timer")) {
        const bool was_active = CancelTimer();
        printf("on mcp tool call: timer.cancel (was active: %d)\n", static_cast<int>(was_active));
        engine.SendMcpCallResponse(mcp_tool_call_event->id, was_active);
      } else if (matches("self.timer.query", "query_timer") || matches("self.timer.get", "get_timer")) {
        const auto remaining = static_cast<int64_t>(TimerRemainingSeconds());
        printf("on mcp tool call: timer.query -> %" PRId64 " s\n", remaining);
        engine.SendMcpCallResponse(mcp_tool_call_event->id, remaining);
      } else if (matches("self.screen.notify", "notify") || matches("self.screen.alert", "alert")) {
        const auto body_ptr = mcp_tool_call_event->param<std::string>("body");
        // Same defensive argument hunt as timer.start: the model picks its own key names often
        // enough that insisting on "body" would drop real notifications on the floor.
        const auto message_ptr = body_ptr != nullptr ? nullptr : mcp_tool_call_event->param<std::string>("message");
        const auto text_ptr =
            (body_ptr == nullptr && message_ptr == nullptr) ? mcp_tool_call_event->param<std::string>("text") : nullptr;
        const std::string* body = body_ptr != nullptr ? body_ptr : (message_ptr != nullptr ? message_ptr : text_ptr);
        if (body == nullptr || body->empty()) {
          engine.SendMcpCallError(mcp_tool_call_event->id, "Missing valid argument: body");
        } else {
          const auto title_ptr = mcp_tool_call_event->param<std::string>("title");
          // hold_seconds is deliberately not in the schema (see InitMcpTools) but is still honoured
          // if the model sends it anyway. Clamped, not rejected: a nonsensical value should land on
          // a sane hold rather than lose the notification.
          const auto hold_ptr = mcp_tool_call_event->param<int64_t>("hold_seconds");
          uint32_t hold_ms = kAlertDefaultHoldMs;
          if (hold_ptr != nullptr && *hold_ptr > 0) {
            hold_ms = static_cast<uint32_t>(std::min<int64_t>(*hold_ptr, kAlertMaxHoldMs / 1000)) * 1000;
          }
          ShowNotification(title_ptr != nullptr && !title_ptr->empty() ? title_ptr->c_str() : "提醒", body->c_str(), hold_ms);
          engine.SendMcpCallResponse(mcp_tool_call_event->id, true);
        }
      } else if (matches("self.screen.notify_clear", "notify_clear") || matches("self.screen.alert_clear", "alert_clear")) {
        const bool was_visible = g_alert_visible;
        ClearNotification();
        printf("on mcp tool call: screen.notify_clear (was visible: %d)\n", static_cast<int>(was_visible));
        engine.SendMcpCallResponse(mcp_tool_call_event->id, was_visible);
      }
    }
  }

  // Handle Web UI requests for servo testing
  ServoWebServer::GetInstance().HandleClient();
  delay(2);
}