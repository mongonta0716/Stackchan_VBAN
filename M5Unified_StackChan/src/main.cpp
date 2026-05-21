#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <SD.h>
#if defined(USE_ATOMIC_ECHO_BASE)
  #include <SPIFFS.h>
  #include <esp_idf_version.h>
  #include <M5EchoBase.h>
#endif
#include <cerrno>
#include <cstring>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <unistd.h>
#include "AudioOutputM5Speaker.h"
#include <Avatar.h> // https://github.com/meganetaaan/m5stack-avatar
#include <Stackchan_system_config.h> // https://github.com/stack-chan/stackchan-arduino
#include <Stackchan_servo.h> // https://github.com/stack-chan/stackchan-arduino
#if defined(ARDUINO_M5STACK_CORES3)
  #include <gob_unifiedButton.hpp>
  goblib::UnifiedButton unifiedButton;
#endif

#ifndef VBAN_STREAM_NAME
  #define VBAN_STREAM_NAME "Stream1"
#endif

#ifndef VBAN_AUDIO_ONLY
  #define VBAN_AUDIO_ONLY 0
#endif

#ifndef USE_AVATAR
  #if VBAN_AUDIO_ONLY
    #define USE_AVATAR 0
  #elif defined(ARDUINO_M5Stack_ATOMS3)
    #define USE_AVATAR 0
  #else
    #define USE_AVATAR 1
  #endif
#endif

#if USE_AVATAR && !defined(ARDUINO_M5Stack_ATOMS3)
  #define USE_SERVO
#endif

#ifndef SERVO_DEBUG_LOG
  #if defined(DEBUG) || defined(_DEBUG)
    #define SERVO_DEBUG_LOG 1
  #else
    #define SERVO_DEBUG_LOG 0
  #endif
#endif

static constexpr size_t WAVE_SIZE = 320;
static constexpr int SD_SPI_FREQUENCY = 10000000;
static constexpr TickType_t LIPSYNC_INTERVAL_TICKS = pdMS_TO_TICKS(150);
static constexpr uint16_t VBAN_PORT = 6980;
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
static constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS = 5000;
static constexpr uint32_t WIFI_IP_DISPLAY_MS = 5000;
static constexpr uint32_t STATUS_INTERVAL_MS = 1000;
static constexpr uint8_t VBAN_HEADER_SIZE = 28;
static constexpr size_t VBAN_PROTOCOL_MAX_SIZE = 1464;
static constexpr int VBAN_SOCKET_RCVBUF = 64 * 1024;
static constexpr uint8_t VBAN_SR_MASK = 0x1F;
static constexpr uint8_t VBAN_PROTOCOL_MASK = 0xE0;
static constexpr uint8_t VBAN_PROTOCOL_AUDIO = 0x00;
static constexpr uint8_t VBAN_BIT_RESOLUTION_MASK = 0x07;
static constexpr uint8_t VBAN_BITFMT_16_INT = 1;
static constexpr uint8_t VBAN_RESERVED_MASK = 0x08;
static constexpr uint8_t VBAN_CODEC_MASK = 0xF0;
static constexpr uint8_t VBAN_CODEC_PCM = 0x00;
static constexpr uint8_t VBAN_STREAM_NAME_SIZE = 16;
static constexpr size_t VBAN_OUTPUT_CHANNELS = 2;
static constexpr size_t VBAN_SAMPLES_MAX_NB = 256;
static constexpr size_t VBAN_OUTPUT_SAMPLES_MAX = VBAN_SAMPLES_MAX_NB * VBAN_OUTPUT_CHANNELS;
static constexpr size_t VBAN_AUDIO_QUEUE_LENGTH = 128;
static constexpr size_t VBAN_AUDIO_PREFILL = 32;
static constexpr size_t VBAN_AUDIO_HIGH_WATER = VBAN_AUDIO_PREFILL + 16;
static constexpr size_t VBAN_AUDIO_DRIFT_DROP_TARGET = VBAN_AUDIO_PREFILL + 4;
static constexpr size_t VBAN_AUDIO_DRIFT_DROP_BURST_MAX = 2;
static constexpr size_t VBAN_AUDIO_BATCH_PACKETS = 2;
static constexpr size_t VBAN_AUDIO_BATCH_SAMPLES_MAX = VBAN_OUTPUT_SAMPLES_MAX * VBAN_AUDIO_BATCH_PACKETS;
static constexpr uint8_t VBAN_I2S_DMA_BUF_COUNT = 8;
static constexpr uint16_t VBAN_I2S_DMA_BUF_LEN = 512;
static constexpr esp_log_level_t VBAN_I2S_LOG_LEVEL_VALUE = static_cast<esp_log_level_t>(VBAN_I2S_LOG_LEVEL);
static constexpr uint32_t VBAN_FRAME_RESYNC_THRESHOLD_MS = 250;
static constexpr uint32_t VBAN_PLC_MAX_GAP_PACKETS = 2;
static constexpr uint32_t VBAN_AUDIO_PACKET_WAIT_MS = 20;
static constexpr UBaseType_t VBAN_RECEIVE_TASK_PRIORITY = 7;
static constexpr UBaseType_t VBAN_AUDIO_TASK_PRIORITY = 6;
#if defined(USE_ATOMIC_ECHO_BASE)
static constexpr uint32_t VBAN_PLAYBACK_SAMPLE_RATE = 48000;
#else
static constexpr uint32_t VBAN_PLAYBACK_SAMPLE_RATE = 48000;
#endif

#if defined(USE_ATOMIC_ECHO_BASE)
static constexpr int ECHOBASE_I2C_SDA = 38;
static constexpr int ECHOBASE_I2C_SCL = 39;
static constexpr int ECHOBASE_I2S_DIN = 7;
static constexpr int ECHOBASE_I2S_WS = 6;
static constexpr int ECHOBASE_I2S_DOUT = 5;
static constexpr int ECHOBASE_I2S_BCK = 8;
#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0))
static M5EchoBase echobase;
#else
static M5EchoBase echobase(I2S_NUM_0);
#endif
#endif

static AudioOutputM5Speaker out(&M5.Speaker, m5spk_virtual_channel);
static int udp_socket_fd = -1;
static uint8_t vban_buffer[VBAN_PROTOCOL_MAX_SIZE];
static int16_t playback_batch[VBAN_AUDIO_BATCH_SAMPLES_MAX];
static int16_t silence_packet[VBAN_OUTPUT_SAMPLES_MAX];
static QueueHandle_t free_packet_queue = nullptr;
static QueueHandle_t audio_packet_queue = nullptr;
static bool udp_started = false;
static bool sd_ready = false;
#if defined(USE_ATOMIC_ECHO_BASE)
static bool spiffs_ready = false;
static bool echobase_ready = false;
#endif
static uint32_t last_wifi_attempt_ms = 0;
static uint32_t last_status_ms = 0;
static uint32_t last_packet_ms = 0;
static uint32_t valid_packets = 0;
static uint32_t dropped_packets = 0;
static uint32_t underrun_count = 0;
static uint32_t packet_gap_count = 0;
static uint32_t packet_conceal_count = 0;
static uint32_t packet_out_of_order_count = 0;
static uint32_t packet_resync_count = 0;
static uint32_t drift_drop_count = 0;
static uint32_t resync_drop_count = 0;
static uint32_t max_queued_packets = 0;
static uint32_t current_vban_frame_delta = 0;
static uint32_t max_vban_frame_delta = 0;
static int32_t current_vban_frame_error = 0;
static int32_t current_vban_frame_error_ms = 0;
static uint32_t max_vban_frame_error_abs = 0;
static uint32_t current_vban_frame_counter = 0;
static uint32_t jitter_prime_count = 0;
static uint32_t audio_write_fail_count = 0;
static int audio_last_error = 0;
static uint32_t current_sample_rate = 48000;
static uint32_t current_playback_sample_rate = VBAN_PLAYBACK_SAMPLE_RATE;
static uint8_t current_channels = 0;
static uint16_t current_frames_per_packet = 0;
static uint32_t last_vban_frame = 0;
static uint16_t last_vban_packet_frames = 0;
static bool have_vban_frame = false;
static volatile bool audio_primed = false;
static bool avatar_started = false;

using namespace m5avatar;
Avatar avatar;
StackchanSystemConfig system_config;

#ifdef USE_SERVO
static constexpr uint32_t SERVO_MOVE_DURATION_MS = 500;
static constexpr float SERVO_GAZE_X_GAIN_DEGREES = 20.0f;
static constexpr float SERVO_GAZE_Y_DOWN_GAIN_DEGREES = 15.0f;
static constexpr float SERVO_GAZE_Y_UP_GAIN_DEGREES = 10.0f;
StackchanSERVO stackchanServo;
#endif
static fft_t fft;
static int16_t raw_data[WAVE_SIZE * 2];
static float lipsync_level_max = 10.0f;
float mouth_ratio = 0.0f;

static void configureI2SLogLevel()
{
  static constexpr const char* i2s_log_tags[] = {
    "i2s",
    "i2s_common",
    "i2s_std",
    "i2s_pdm",
    "i2s_tdm",
    "i2s_platform",
    "i2s_dma",
  };

  for (const char* tag : i2s_log_tags) {
    esp_log_level_set(tag, VBAN_I2S_LOG_LEVEL_VALUE);
  }

  if (VBAN_I2S_LOG_LEVEL >= ESP_LOG_INFO) {
    M5_LOGI("I2S log level:%d", (int)VBAN_I2S_LOG_LEVEL_VALUE);
  }
}

struct VBanHeader
{
  char vban[4];
  uint8_t format_SR;
  uint8_t format_nbs;
  uint8_t format_nbc;
  uint8_t format_bit;
  char streamname[VBAN_STREAM_NAME_SIZE];
  uint32_t nuFrame;
} __attribute__((packed));

struct VbanAudioPacket
{
  int16_t samples[VBAN_OUTPUT_SAMPLES_MAX];
  size_t sample_count;
  uint32_t sample_rate;
  uint8_t channels;
  bool stereo;
};

struct VbanPacketTiming
{
  uint32_t frame_counter;
  uint16_t frames_per_packet;
  uint32_t sample_rate;
};

struct VbanFrameSyncResult
{
  bool accepted;
  uint32_t missing_packets;
};

static VbanAudioPacket* packet_pool = nullptr;
static VbanAudioPacket conceal_template = {};
static bool have_conceal_template = false;

static constexpr uint32_t VBanSRList[] = {
  6000, 12000, 24000, 48000, 96000, 192000, 384000,
  8000, 16000, 32000, 64000, 128000, 256000, 512000,
  11025, 22050, 44100, 88200, 176400, 352800, 705600
};
static const char expected_vban_stream_name[VBAN_STREAM_NAME_SIZE + 1] = VBAN_STREAM_NAME;

static void drawStatus(const char* line1, const char* line2 = nullptr, const char* line3 = nullptr)
{
  if (avatar_started) {
    M5_LOGI("%s", line1);
    if (line2) M5_LOGI("%s", line2);
    if (line3) M5_LOGI("%s", line3);
    return;
  }
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setCursor(0, 0);
  M5.Display.setTextSize(2);
  M5.Display.println(line1);
  if (line2) M5.Display.println(line2);
  if (line3) M5.Display.println(line3);
}

static bool hasWifiCredentials()
{
  const String& ssid = system_config.getWiFiSetting()->ssid;
  return ssid.length() > 0 && ssid != "YOUR_WIFI_SSID";
}

static void startUdp()
{
  if (udp_started) return;

  udp_socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (udp_socket_fd < 0) {
    M5_LOGE("Failed to create VBAN UDP socket errno:%d", errno);
    return;
  }

  int enable = 1;
  setsockopt(udp_socket_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

  int recv_buffer_size = VBAN_SOCKET_RCVBUF;
  if (setsockopt(udp_socket_fd, SOL_SOCKET, SO_RCVBUF, &recv_buffer_size, sizeof(recv_buffer_size)) < 0) {
    M5_LOGE("Failed to set VBAN UDP receive buffer errno:%d", errno);
  }
  socklen_t opt_len = sizeof(recv_buffer_size);
  getsockopt(udp_socket_fd, SOL_SOCKET, SO_RCVBUF, &recv_buffer_size, &opt_len);

  sockaddr_in listen_addr = {};
  listen_addr.sin_family = AF_INET;
  listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  listen_addr.sin_port = htons(VBAN_PORT);
  if (bind(udp_socket_fd, reinterpret_cast<sockaddr*>(&listen_addr), sizeof(listen_addr)) < 0) {
    M5_LOGE("Failed to bind VBAN UDP socket errno:%d", errno);
    close(udp_socket_fd);
    udp_socket_fd = -1;
    return;
  }

  udp_started = true;
  M5_LOGI("VBAN UDP listening on %u rcvbuf:%d", VBAN_PORT, recv_buffer_size);
}

static void stopUdp()
{
  if (!udp_started) return;
  close(udp_socket_fd);
  udp_socket_fd = -1;
  udp_started = false;
}

static void connectWiFi(bool blocking)
{
  if (!hasWifiCredentials()) {
    drawStatus("WiFi not configured", "Set wifi.ssid/password", "in /yaml/SC_SecConfig.yaml");
    return;
  }

  const wifi_s* wifi = system_config.getWiFiSetting();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  WiFi.setTxPower(WIFI_POWER_5dBm);
  WiFi.begin(wifi->ssid.c_str(), wifi->password.c_str());
  last_wifi_attempt_ms = millis();

  M5_LOGI("Connecting WiFi SSID: %s", wifi->ssid.c_str());
  drawStatus("Connecting WiFi...", wifi->ssid.c_str());

  if (!blocking) return;

  uint32_t started_at = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started_at < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    M5.update();
  }
}

static void showWiFiAddressOnStartup()
{
  if (WiFi.status() != WL_CONNECTED) {
    M5_LOGE("WiFi connection failed or timed out");
    drawStatus("WiFi connect failed", "Will retry");
    delay(WIFI_IP_DISPLAY_MS);
    return;
  }

  IPAddress ip = WiFi.localIP();
  M5_LOGI("WiFi connected SSID:%s IP:%s",
          system_config.getWiFiSetting()->ssid.c_str(), ip.toString().c_str());
  M5_LOGI("VBAN listening address: %s:%u stream:%s",
          ip.toString().c_str(), VBAN_PORT, VBAN_STREAM_NAME);

  drawStatus("WiFi connected", ip.toString().c_str(), "VBAN UDP 6980");
  delay(WIFI_IP_DISPLAY_MS);
}

static void maintainWiFi()
{
  if (!hasWifiCredentials()) return;

  if (WiFi.status() == WL_CONNECTED) {
    startUdp();
    return;
  }

  stopUdp();
  if (millis() - last_wifi_attempt_ms >= WIFI_RECONNECT_INTERVAL_MS) {
    M5_LOGI("WiFi disconnected. Reconnecting...");
    connectWiFi(false);
  }
}

static bool streamNameMatches(const char* packet_stream)
{
  return memcmp(packet_stream, expected_vban_stream_name, VBAN_STREAM_NAME_SIZE) == 0;
}

static bool validateVbanPacket(const uint8_t* buffer, size_t size, const VBanHeader** header)
{
  if (size <= VBAN_HEADER_SIZE) return false;

  const VBanHeader* hdr = reinterpret_cast<const VBanHeader*>(buffer);
  if (memcmp(hdr->vban, "VBAN", 4) != 0) return false;
  if (!streamNameMatches(hdr->streamname)) return false;
  if ((hdr->format_SR & VBAN_PROTOCOL_MASK) != VBAN_PROTOCOL_AUDIO) return false;
  if ((hdr->format_SR & VBAN_SR_MASK) >= (sizeof(VBanSRList) / sizeof(VBanSRList[0]))) return false;
  if (hdr->format_bit & VBAN_RESERVED_MASK) return false;
  if ((hdr->format_bit & VBAN_CODEC_MASK) != VBAN_CODEC_PCM) return false;
  if ((hdr->format_bit & VBAN_BIT_RESOLUTION_MASK) != VBAN_BITFMT_16_INT) return false;

  const size_t channels = hdr->format_nbc + 1;
  const size_t samples = hdr->format_nbs + 1;
  const size_t payload_size = samples * channels * sizeof(int16_t);
  if (payload_size != size - VBAN_HEADER_SIZE) return false;
  if (samples > VBAN_SAMPLES_MAX_NB) return false;

  *header = hdr;
  return true;
}

static void releaseAudioPacket(VbanAudioPacket* packet)
{
  if (packet != nullptr && free_packet_queue != nullptr) {
    xQueueSend(free_packet_queue, &packet, 0);
  }
}

static bool buildAudioPacket(const uint8_t* buffer, const VBanHeader* hdr, VbanAudioPacket* packet)
{
  const int16_t* payload = reinterpret_cast<const int16_t*>(buffer + VBAN_HEADER_SIZE);
  const uint8_t channels = hdr->format_nbc + 1;
  const uint16_t frames = hdr->format_nbs + 1;
  int16_t* out_samples = packet->samples;

  packet->sample_count = frames * VBAN_OUTPUT_CHANNELS;
  packet->sample_rate = VBanSRList[hdr->format_SR & VBAN_SR_MASK];
  packet->channels = channels;
  packet->stereo = true;
  if (channels == 1) {
    for (uint16_t i = 0; i < frames; ++i) {
      const int16_t sample = payload[i];
      *out_samples++ = sample;
      *out_samples++ = sample;
    }
  } else {
    const int16_t* in = payload;
    for (uint16_t i = 0; i < frames; ++i) {
      *out_samples++ = in[0];
      *out_samples++ = in[1];
      in += channels;
    }
  }
  return packet->sample_count > 0;
}

static VbanAudioPacket* acquireAudioPacket()
{
  VbanAudioPacket* packet = nullptr;
  if (xQueueReceive(free_packet_queue, &packet, 0) == pdTRUE) {
    return packet;
  }

  VbanAudioPacket* stale_packet = nullptr;
  if (xQueueReceive(audio_packet_queue, &stale_packet, 0) == pdTRUE) {
    releaseAudioPacket(stale_packet);
    xQueueReceive(free_packet_queue, &packet, 0);
  }
  return packet;
}

static VbanAudioPacket* acquireFreeAudioPacket()
{
  VbanAudioPacket* packet = nullptr;
  if (free_packet_queue != nullptr && xQueueReceive(free_packet_queue, &packet, 0) == pdTRUE) {
    return packet;
  }
  return nullptr;
}

static unsigned getQueuedAudioPackets()
{
  return audio_packet_queue ? static_cast<unsigned>(uxQueueMessagesWaiting(audio_packet_queue)) : 0;
}

static void noteQueuedAudioPackets(unsigned queued_packets)
{
  if (queued_packets > max_queued_packets) {
    max_queued_packets = queued_packets;
  }
}

static void flushAudioQueueForResync()
{
  if (audio_packet_queue == nullptr) return;

  VbanAudioPacket* stale_packet = nullptr;
  while (xQueueReceive(audio_packet_queue, &stale_packet, 0) == pdTRUE) {
    releaseAudioPacket(stale_packet);
    ++dropped_packets;
    ++resync_drop_count;
  }
}

static void trimAudioQueueForDrift()
{
  if (audio_packet_queue == nullptr) return;
  unsigned queued_packets = getQueuedAudioPackets();
  noteQueuedAudioPackets(queued_packets);
  if (queued_packets <= VBAN_AUDIO_HIGH_WATER) return;

  size_t dropped = 0;
  while (queued_packets > VBAN_AUDIO_DRIFT_DROP_TARGET && dropped < VBAN_AUDIO_DRIFT_DROP_BURST_MAX) {
    VbanAudioPacket* stale_packet = nullptr;
    if (xQueueReceive(audio_packet_queue, &stale_packet, 0) != pdTRUE) {
      break;
    }
    releaseAudioPacket(stale_packet);
    --queued_packets;
    ++dropped_packets;
    ++drift_drop_count;
    ++dropped;
  }
}

static bool queueAudioPacket(VbanAudioPacket* packet)
{
  if (packet == nullptr) return false;
  if (xQueueSend(audio_packet_queue, &packet, 0) == pdTRUE) {
    trimAudioQueueForDrift();
    return true;
  }
  releaseAudioPacket(packet);
  ++dropped_packets;
  return false;
}

static uint32_t vbanResyncThresholdFrames(uint32_t sample_rate)
{
  return (static_cast<uint64_t>(sample_rate) * VBAN_FRAME_RESYNC_THRESHOLD_MS + 999ULL) / 1000ULL;
}

static uint32_t vbanResyncThresholdPackets(uint32_t sample_rate, uint16_t frames_per_packet)
{
  if (sample_rate == 0 || frames_per_packet == 0) return 1;
  const uint32_t frames = vbanResyncThresholdFrames(sample_rate);
  return (frames + frames_per_packet - 1) / frames_per_packet;
}

static int32_t vbanPacketsToMs(int64_t packets, uint16_t frames_per_packet, uint32_t sample_rate)
{
  if (sample_rate == 0) return 0;
  const int64_t frames = packets * static_cast<int64_t>(frames_per_packet);
  return static_cast<int32_t>((frames * 1000LL) / static_cast<int64_t>(sample_rate));
}

static VbanPacketTiming getVbanPacketTiming(const VBanHeader* hdr)
{
  return {
    hdr->nuFrame,
    static_cast<uint16_t>(hdr->format_nbs + 1),
    VBanSRList[hdr->format_SR & VBAN_SR_MASK],
  };
}

static void noteVbanFrameDelta(uint32_t delta)
{
  current_vban_frame_delta = delta;
  if (delta > max_vban_frame_delta && delta < 0x80000000UL) {
    max_vban_frame_delta = delta;
  }
}

static void resetCurrentVbanFrameError()
{
  current_vban_frame_error = 0;
  current_vban_frame_error_ms = 0;
}

static void noteVbanFrameError(int64_t packet_error, uint16_t frames_per_packet, uint32_t sample_rate)
{
  current_vban_frame_error = static_cast<int32_t>(packet_error);
  current_vban_frame_error_ms = vbanPacketsToMs(packet_error, frames_per_packet, sample_rate);
  const uint32_t packet_error_abs = packet_error < 0
    ? static_cast<uint32_t>(-packet_error)
    : static_cast<uint32_t>(packet_error);
  if (packet_error_abs > max_vban_frame_error_abs) {
    max_vban_frame_error_abs = packet_error_abs;
  }
}

static void markVbanFrameOutOfOrder()
{
  ++packet_out_of_order_count;
  ++dropped_packets;
}

static void resyncVbanAudioQueue()
{
  ++packet_resync_count;
  flushAudioQueueForResync();
  have_conceal_template = false;
}

static void commitVbanPacketTiming(const VbanPacketTiming& timing)
{
  have_vban_frame = true;
  last_vban_frame = timing.frame_counter;
  last_vban_packet_frames = timing.frames_per_packet;
  current_vban_frame_counter = timing.frame_counter;
}

static VbanFrameSyncResult syncVbanFrameCounter(const VbanPacketTiming& timing)
{
  if (!have_vban_frame) {
    commitVbanPacketTiming(timing);
    return { true, 0 };
  }

  const uint32_t delta = timing.frame_counter - last_vban_frame;
  const uint16_t expected_packet_frames = last_vban_packet_frames ? last_vban_packet_frames : timing.frames_per_packet;
  const uint32_t resync_threshold_packets = vbanResyncThresholdPackets(timing.sample_rate, expected_packet_frames);
  noteVbanFrameDelta(delta);
  resetCurrentVbanFrameError();

  if (delta == 0) {
    markVbanFrameOutOfOrder();
    return { false, 0 };
  }

  if (delta >= 0x80000000UL) {
    const uint32_t backward_delta = last_vban_frame - timing.frame_counter;
    if (!audio_primed &&
        getQueuedAudioPackets() < VBAN_AUDIO_PREFILL &&
        backward_delta > resync_threshold_packets) {
      resyncVbanAudioQueue();
      current_vban_frame_delta = 1;
      commitVbanPacketTiming(timing);
      return { true, 0 };
    }

    markVbanFrameOutOfOrder();
    return { false, 0 };
  }

  const int64_t packet_error = static_cast<int64_t>(delta) - 1LL;
  noteVbanFrameError(packet_error, expected_packet_frames, timing.sample_rate);
  if (packet_error < 0) {
    markVbanFrameOutOfOrder();
    return { false, 0 };
  }

  uint32_t missing_packets = 0;
  if (packet_error > static_cast<int64_t>(resync_threshold_packets)) {
    resyncVbanAudioQueue();
  } else if (packet_error > 0) {
    missing_packets = static_cast<uint32_t>(packet_error);
    packet_gap_count += missing_packets;
  }

  commitVbanPacketTiming(timing);
  return { true, missing_packets };
}

static void enqueueConcealPackets(uint32_t missing_packets)
{
  if (!have_conceal_template) return;
  unsigned queued_packets = getQueuedAudioPackets();
  if (queued_packets >= VBAN_AUDIO_PREFILL) return;

  uint32_t conceal_packets = missing_packets;
  if (conceal_packets > VBAN_PLC_MAX_GAP_PACKETS) {
    conceal_packets = VBAN_PLC_MAX_GAP_PACKETS;
  }
  const uint32_t room_to_prefill = VBAN_AUDIO_PREFILL - queued_packets;
  if (conceal_packets > room_to_prefill) {
    conceal_packets = room_to_prefill;
  }

  for (uint32_t i = 0; i < conceal_packets; ++i) {
    VbanAudioPacket* packet = acquireFreeAudioPacket();
    if (packet == nullptr) {
      return;
    }
    *packet = conceal_template;
    if (queueAudioPacket(packet)) {
      ++packet_conceal_count;
      ++queued_packets;
    } else {
      return;
    }
  }
}

static void enqueueVbanPacket(const uint8_t* buffer, const VBanHeader* hdr)
{
  if (free_packet_queue == nullptr || audio_packet_queue == nullptr) return;

  const VbanFrameSyncResult sync = syncVbanFrameCounter(getVbanPacketTiming(hdr));
  if (!sync.accepted) return;
  enqueueConcealPackets(sync.missing_packets);

  VbanAudioPacket* packet = acquireAudioPacket();
  if (packet == nullptr) {
    ++dropped_packets;
    return;
  }

  if (!buildAudioPacket(buffer, hdr, packet)) {
    releaseAudioPacket(packet);
    ++dropped_packets;
    return;
  }

  current_sample_rate = packet->sample_rate;
  current_channels = packet->channels;
  current_frames_per_packet = packet->sample_count / VBAN_OUTPUT_CHANNELS;
  last_packet_ms = millis();
  if (queueAudioPacket(packet)) {
    conceal_template = *packet;
    have_conceal_template = true;
    ++valid_packets;
  }
}

static bool receiveVban()
{
  if (!udp_started || udp_socket_fd < 0) return false;

  int read_size = recv(udp_socket_fd, vban_buffer, sizeof(vban_buffer), 0);
  if (read_size < 0) {
    ++dropped_packets;
    return true;
  }

  const VBanHeader* hdr = nullptr;
  if (read_size > 0 && validateVbanPacket(vban_buffer, read_size, &hdr)) {
    enqueueVbanPacket(vban_buffer, hdr);
  } else {
    ++dropped_packets;
  }
  return true;
}

static bool initAudioQueues()
{
  packet_pool = static_cast<VbanAudioPacket*>(ps_malloc(sizeof(VbanAudioPacket) * VBAN_AUDIO_QUEUE_LENGTH));
  if (packet_pool == nullptr) {
    packet_pool = static_cast<VbanAudioPacket*>(malloc(sizeof(VbanAudioPacket) * VBAN_AUDIO_QUEUE_LENGTH));
  }
  free_packet_queue = xQueueCreate(VBAN_AUDIO_QUEUE_LENGTH, sizeof(VbanAudioPacket*));
  audio_packet_queue = xQueueCreate(VBAN_AUDIO_QUEUE_LENGTH, sizeof(VbanAudioPacket*));
  if (packet_pool == nullptr || free_packet_queue == nullptr || audio_packet_queue == nullptr) {
    M5_LOGE("Failed to allocate VBAN audio queues");
    return false;
  }

  for (size_t i = 0; i < VBAN_AUDIO_QUEUE_LENGTH; ++i) {
    VbanAudioPacket* packet = &packet_pool[i];
    xQueueSend(free_packet_queue, &packet, 0);
  }
  return true;
}

static void vbanReceiveTask(void*)
{
  for (;;) {
    if (!receiveVban()) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}

static void vbanAudioTask(void*)
{
  bool primed = false;
  uint32_t consecutive_underruns = 0;
  for (;;) {
    if (audio_packet_queue == nullptr) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    if (!primed) {
      while (uxQueueMessagesWaiting(audio_packet_queue) < VBAN_AUDIO_PREFILL) {
        vTaskDelay(1);
      }
      primed = true;
      audio_primed = true;
      consecutive_underruns = 0;
      ++jitter_prime_count;
    }

    VbanAudioPacket* packet = nullptr;
    const bool got_packet = xQueueReceive(audio_packet_queue, &packet, pdMS_TO_TICKS(VBAN_AUDIO_PACKET_WAIT_MS)) == pdTRUE;
    if (got_packet) {
      consecutive_underruns = 0;
      const uint32_t sample_rate = packet->sample_rate;
      const bool stereo = packet->stereo;
      size_t batch_sample_count = 0;

      memcpy(&playback_batch[batch_sample_count], packet->samples, packet->sample_count * sizeof(int16_t));
      batch_sample_count += packet->sample_count;
      releaseAudioPacket(packet);

      for (size_t i = 1; i < VBAN_AUDIO_BATCH_PACKETS; ++i) {
        VbanAudioPacket* next_packet = nullptr;
        if (xQueueReceive(audio_packet_queue, &next_packet, 0) != pdTRUE) {
          break;
        }

        if (next_packet->sample_rate != sample_rate || next_packet->stereo != stereo || batch_sample_count + next_packet->sample_count > VBAN_AUDIO_BATCH_SAMPLES_MAX) {
          xQueueSendToFront(audio_packet_queue, &next_packet, 0);
          break;
        }

        memcpy(&playback_batch[batch_sample_count], next_packet->samples, next_packet->sample_count * sizeof(int16_t));
        batch_sample_count += next_packet->sample_count;
        releaseAudioPacket(next_packet);
      }

      current_playback_sample_rate = sample_rate;
      out.playPcm16(playback_batch, batch_sample_count, sample_rate, stereo);
      audio_write_fail_count = out.getWriteFailCount();
      audio_last_error = out.getLastError();
    } else {
      ++underrun_count;
      ++consecutive_underruns;
      if (consecutive_underruns <= 2 && have_conceal_template) {
        out.playPcm16(conceal_template.samples, conceal_template.sample_count,
                      conceal_template.sample_rate, conceal_template.stereo);
      } else {
        const size_t silence_sample_count = current_frames_per_packet
          ? current_frames_per_packet * VBAN_OUTPUT_CHANNELS
          : 103 * VBAN_OUTPUT_CHANNELS;
        out.playPcm16(silence_packet, silence_sample_count, current_playback_sample_rate, true);
      }
      audio_write_fail_count = out.getWriteFailCount();
      audio_last_error = out.getLastError();
      if (consecutive_underruns >= 5 && uxQueueMessagesWaiting(audio_packet_queue) == 0) {
        primed = false;
        audio_primed = false;
      }
    }
  }
}

void lipSync(void *args)
{
  uint32_t last_update_count = 0;
  DriveContext *ctx = (DriveContext *)args;
  Avatar *avatar = ctx->getAvatar();
  for (;;)
  {
    uint64_t level = 0;
    uint32_t update_count = out.getUpdateCount();
    if (update_count != last_update_count) {
      last_update_count = update_count;
      auto buf = out.getBuffer();
      if (buf) {
        memcpy(raw_data, buf, WAVE_SIZE * 2 * sizeof(int16_t));
        fft.exec(raw_data);
        for (size_t bx = 6; bx <= 48; bx += 2) {
          int32_t f = fft.get(bx);
          level += abs(f);
        }
      }
    }

    mouth_ratio = (float)(level >> 16) / lipsync_level_max;
    if (mouth_ratio > 1.2f) {
      if (mouth_ratio > 1.5f) {
        lipsync_level_max += 10.0f;
      }
      mouth_ratio = 1.2f;
    }
    avatar->setMouthOpenRatio(mouth_ratio);

    vTaskDelay(LIPSYNC_INTERVAL_TICKS);
  }
}

#ifdef USE_SERVO
static int gazeToServoOffset(float gaze, float gain_degrees)
{
  const float clamped_gaze = constrain(gaze, -1.0f, 1.0f);
  const float offset = gain_degrees * clamped_gaze;
  return (int)(offset + (offset >= 0.0f ? 0.5f : -0.5f));
}

static void centerAvatarGaze()
{
  avatar.setRightGaze(0.0f, 0.0f);
  avatar.setLeftGaze(0.0f, 0.0f);
}
#endif

void servo(void *args)
{
  float gazeX, gazeY;
  DriveContext *ctx = (DriveContext *)args;
  Avatar *avatar = ctx->getAvatar();
  for (;;)
  {
#ifdef USE_SERVO
    avatar->getGaze(&gazeY, &gazeX);
    int servoX = system_config.getServoInfo(AXIS_X)->start_degree + gazeToServoOffset(gazeX, SERVO_GAZE_X_GAIN_DEGREES);
    int servoY = system_config.getServoInfo(AXIS_Y)->start_degree;
    if (gazeY < 0.0f) {
      servoY += gazeToServoOffset(gazeY, SERVO_GAZE_Y_DOWN_GAIN_DEGREES);
    } else {
      servoY += gazeToServoOffset(gazeY, SERVO_GAZE_Y_UP_GAIN_DEGREES);
    }
    servoX = constrain(servoX, system_config.getServoInfo(AXIS_X)->lower_limit, system_config.getServoInfo(AXIS_X)->upper_limit);
    servoY = constrain(servoY, system_config.getServoInfo(AXIS_Y)->lower_limit, system_config.getServoInfo(AXIS_Y)->upper_limit);
#if SERVO_DEBUG_LOG
    M5_LOGI("Servo gaze x:%d y:%d -> X:%d Y:%d", (int)(gazeX * 100.0f), (int)(gazeY * 100.0f), servoX, servoY);
#endif
    stackchanServo.moveXY(servoX, servoY, SERVO_MOVE_DURATION_MS);
#endif
    delay(5000);
  }
}

void Servo_setup()
{
#ifdef USE_SERVO
  stackchanServo.begin(system_config.getServoInfo(AXIS_X)->pin,
                       system_config.getServoInfo(AXIS_X)->start_degree,
                       system_config.getServoInfo(AXIS_X)->offset,
                       system_config.getServoInfo(AXIS_Y)->pin,
                       system_config.getServoInfo(AXIS_Y)->start_degree,
                       system_config.getServoInfo(AXIS_Y)->offset,
                       (ServoType)system_config.getServoType(),
                       &M5.In_I2C);
#endif
}

void config_read()
{
  if (sd_ready) {
    system_config.loadConfig(SD, "");
    return;
  }
#if defined(USE_ATOMIC_ECHO_BASE)
  if (spiffs_ready) {
    system_config.loadConfig(SPIFFS, "");
  }
#endif
}

static void initConfigStorage()
{
#if defined(USE_ATOMIC_ECHO_BASE)
  spiffs_ready = SPIFFS.begin(true);
  if (!spiffs_ready) {
    M5_LOGE("SPIFFS mount failed. Default config will be used.");
  }
#else
  for (int time_out = 0; time_out <= 6; ++time_out) {
    if (SD.begin(GPIO_NUM_4, SPI, SD_SPI_FREQUENCY)) {
      sd_ready = true;
      break;
    }
    M5_LOGI("SD Wait...");
    M5.Display.println("SD Wait...");
    delay(500);
  }
#endif
}

#if defined(USE_ATOMIC_ECHO_BASE)
static bool initEchoBase(uint8_t volume)
{
  if (!echobase.init(VBAN_PLAYBACK_SAMPLE_RATE, ECHOBASE_I2C_SDA, ECHOBASE_I2C_SCL,
                     ECHOBASE_I2S_DIN, ECHOBASE_I2S_WS, ECHOBASE_I2S_DOUT,
                     ECHOBASE_I2S_BCK, Wire)) {
    M5_LOGE("Failed to initialize Atomic Echo Base");
    return false;
  }
  echobase.setSpeakerVolume(map(volume, 0, 255, 0, 100));
  echobase.setMicGain(ES8311_MIC_GAIN_6DB);
  return true;
}

static void applyEchoBaseOutputSettings(uint8_t volume)
{
  if (!echobase_ready) return;
  echobase.setMute(false);
  echobase.setSpeakerVolume(map(volume, 0, 255, 0, 100));
}
#endif

static void configureAvatarLayout()
{
  switch (M5.getBoard()) {
  case m5::board_t::board_M5AtomS3:
  case m5::board_t::board_M5AtomS3R:
    avatar.setScale(0.55f);
    avatar.setPosition(-60, -95);
    break;
  default:
    avatar.setScale(1.0f);
    avatar.setPosition(0, 0);
    break;
  }
}

struct VbanStatusMetrics
{
  float packet_rate;
  float expected_packet_rate;
  float frame_counter_audio_rate;
  float effective_audio_rate;
  float frame_clock_drift_ms;
  unsigned queued_packets;
  float buffer_ms;
};

static VbanStatusMetrics collectVbanStatusMetrics()
{
  static uint32_t last_ok = 0;
  static uint32_t last_plc = 0;
  static uint32_t last_ms = 0;
  static uint32_t last_frame_counter = 0;
  static bool have_last_frame_counter = false;

  const uint32_t now = millis();
  const uint32_t delta_ms = last_ms ? now - last_ms : 0;
  const uint32_t delta_ok = valid_packets - last_ok;
  const uint32_t delta_plc = packet_conceal_count - last_plc;
  uint32_t frame_counter_delta = 0;
  if (have_last_frame_counter) {
    frame_counter_delta = current_vban_frame_counter - last_frame_counter;
    if (frame_counter_delta >= 0x80000000UL) {
      frame_counter_delta = 0;
    }
  }

  const uint64_t audio_frame_delta = static_cast<uint64_t>(frame_counter_delta) * current_frames_per_packet;
  const float packet_rate = delta_ms ? (delta_ok * 1000.0f / delta_ms) : 0.0f;
  const float expected_packet_rate = current_frames_per_packet
    ? (current_sample_rate / static_cast<float>(current_frames_per_packet))
    : 0.0f;
  const float frame_counter_audio_rate = (delta_ms && audio_frame_delta)
    ? (audio_frame_delta * 1000.0f / delta_ms)
    : 0.0f;
  const float effective_audio_rate = delta_ms
    ? ((delta_ok + delta_plc) * 1000.0f / delta_ms * current_frames_per_packet)
    : 0.0f;
  const float frame_clock_drift_ms = (delta_ms && current_sample_rate && audio_frame_delta)
    ? ((audio_frame_delta * 1000.0f / current_sample_rate) - delta_ms)
    : 0.0f;
  const unsigned queued_packets = getQueuedAudioPackets();
  noteQueuedAudioPackets(queued_packets);
  const float buffer_ms = (current_frames_per_packet && current_playback_sample_rate)
    ? (queued_packets * current_frames_per_packet * 1000.0f / current_playback_sample_rate)
    : 0.0f;

  last_ms = now;
  last_ok = valid_packets;
  last_plc = packet_conceal_count;
  last_frame_counter = current_vban_frame_counter;
  have_last_frame_counter = have_vban_frame;

  return {
    packet_rate,
    expected_packet_rate,
    frame_counter_audio_rate,
    effective_audio_rate,
    frame_clock_drift_ms,
    queued_packets,
    buffer_ms,
  };
}

static void logVbanStatus(const VbanStatusMetrics& metrics)
{
  M5_LOGI("VBAN %s:%u %s %luHz play:%luHz %uch frames:%u ok:%lu drop:%lu driftDrop:%lu resyncDrop:%lu gap:%lu plc:%lu ooo:%lu resync:%lu nu:%lu maxNu:%lu frameErr:%ld frameErrMs:%ld maxFrameErr:%lu underrun:%lu prime:%lu queued:%u maxQ:%lu bufMs:%.1f i2s:%d/%d/%d amp:%u i2sFail:%lu i2sErr:%d pps:%.1f needPps:%.1f fcHz:%.1f effHz:%.1f fcDriftMs:%.1f",
          WiFi.localIP().toString().c_str(), VBAN_PORT, VBAN_STREAM_NAME,
          current_sample_rate, current_playback_sample_rate, current_channels, current_frames_per_packet, valid_packets, dropped_packets, drift_drop_count, resync_drop_count,
          packet_gap_count, packet_conceal_count, packet_out_of_order_count, packet_resync_count,
          current_vban_frame_delta, max_vban_frame_delta,
          (long)current_vban_frame_error, (long)current_vban_frame_error_ms, max_vban_frame_error_abs,
          underrun_count, jitter_prime_count, metrics.queued_packets, max_queued_packets, metrics.buffer_ms,
          out.getBckPin(), out.getWsPin(), out.getDataOutPin(), out.isAmpEnabled(),
          audio_write_fail_count, audio_last_error, metrics.packet_rate, metrics.expected_packet_rate,
          metrics.frame_counter_audio_rate, metrics.effective_audio_rate, metrics.frame_clock_drift_ms);
}

static void updateStatus()
{
  if (millis() - last_status_ms < STATUS_INTERVAL_MS) return;
  last_status_ms = millis();

  if (!hasWifiCredentials()) return;

  if (WiFi.status() == WL_CONNECTED) {
    static uint32_t last_log_ms = 0;
    if (millis() - last_log_ms >= 5000) {
      const VbanStatusMetrics metrics = collectVbanStatusMetrics();
      last_log_ms = millis();
      logVbanStatus(metrics);
    }
  }
}

void setup()
{
  auto cfg = M5.config();
  cfg.internal_spk = true;
  cfg.external_spk = true;
  cfg.serial_baudrate = 115200;

  M5.begin(cfg);
  M5.Log.setLogLevel(m5::log_target_serial, ESP_LOG_INFO);
  configureI2SLogLevel();
  M5_LOGI("M5Unified Stack-chan VBAN receiver starting");
#if defined(ARDUINO_M5STACK_CORES3)
  unifiedButton.begin(&M5.Display, goblib::UnifiedButton::appearance_t::transparent_all);
#endif

  {
    auto spk_cfg = M5.Speaker.config();
    spk_cfg.sample_rate = VBAN_PLAYBACK_SAMPLE_RATE;
    spk_cfg.task_priority = 5;
    spk_cfg.dma_buf_count = VBAN_I2S_DMA_BUF_COUNT;
    spk_cfg.dma_buf_len = VBAN_I2S_DMA_BUF_LEN;
    spk_cfg.task_pinned_core = APP_CPU_NUM;
#if defined(USE_ATOMIC_ECHO_BASE)
    spk_cfg.i2s_port = I2S_NUM_0;
    spk_cfg.pin_bck = ECHOBASE_I2S_BCK;
    spk_cfg.pin_ws = ECHOBASE_I2S_WS;
    spk_cfg.pin_data_out = ECHOBASE_I2S_DOUT;
#endif
    M5.Speaker.config(spk_cfg);
  }

  M5.Display.clear();
  M5.Display.setCursor(0, 0);
  M5.Display.setTextSize(2);

  initConfigStorage();
  config_read();
#if defined(USE_ATOMIC_ECHO_BASE)
  echobase_ready = initEchoBase(system_config.getBluetoothSetting()->start_volume);
#endif

  auto active_spk_cfg = M5.Speaker.config();
  if (VBAN_I2S_LOG_LEVEL >= ESP_LOG_INFO) {
    M5_LOGI("Direct I2S config port:%d bck:%d ws:%d data:%d mck:%d dma:%u/%u",
            active_spk_cfg.i2s_port, active_spk_cfg.pin_bck, active_spk_cfg.pin_ws,
            active_spk_cfg.pin_data_out, active_spk_cfg.pin_mck,
            (unsigned)active_spk_cfg.dma_buf_count, (unsigned)active_spk_cfg.dma_buf_len);
  }
  if (!out.begin(active_spk_cfg, VBAN_PLAYBACK_SAMPLE_RATE)) {
    if (VBAN_I2S_LOG_LEVEL >= ESP_LOG_ERROR) {
      M5_LOGE("Failed to initialize direct I2S audio output");
    }
  } else {
    if (VBAN_I2S_LOG_LEVEL >= ESP_LOG_INFO) {
      M5_LOGI("Direct I2S audio output initialized port:%d bck:%d ws:%d data:%d amp:%u",
              out.getPort(), out.getBckPin(), out.getWsPin(), out.getDataOutPin(), out.isAmpEnabled());
    }
  }
  initAudioQueues();
  const uint8_t start_volume = system_config.getBluetoothSetting()->start_volume;
#if defined(USE_ATOMIC_ECHO_BASE)
  applyEchoBaseOutputSettings(start_volume);
#endif
  M5.Speaker.setChannelVolume(m5spk_virtual_channel, start_volume);
  M5.Speaker.setVolume(start_volume);
  out.setVolume(start_volume);
  M5_LOGI("Audio volume:%u", start_volume);

  if (USE_AVATAR) {
    Servo_setup();
  } else {
    M5_LOGI("Avatar disabled: avatar, servo, and lipsync tasks are disabled");
  }
  connectWiFi(true);
  maintainWiFi();
  showWiFiAddressOnStartup();

  if (USE_AVATAR) {
    configureAvatarLayout();
    avatar.init();
#ifdef USE_SERVO
    centerAvatarGaze();
#endif
    avatar_started = true;
    avatar.addTask(lipSync, "lipSync");
#ifdef USE_SERVO
    avatar.addTask(servo, "servo", 4096);
#endif
  }

  xTaskCreatePinnedToCore(vbanReceiveTask, "vbanRecv", 8192, nullptr, VBAN_RECEIVE_TASK_PRIORITY, nullptr, PRO_CPU_NUM);
  xTaskCreatePinnedToCore(vbanAudioTask, "vbanAudio", 8192, nullptr, VBAN_AUDIO_TASK_PRIORITY, nullptr, APP_CPU_NUM);
}

void loop()
{
  M5.update();
#if defined(ARDUINO_M5STACK_CORES3)
  unifiedButton.update();
#endif
  maintainWiFi();
  updateStatus();
  vTaskDelay(1);
}
