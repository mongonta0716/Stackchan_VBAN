/// set M5Speaker virtual channel (0-7)
#include <driver/i2s.h>
#include <M5Unified.h>

static constexpr uint8_t m5spk_virtual_channel = 0;

class AudioOutputM5Speaker
{
  public:
    AudioOutputM5Speaker(m5::Speaker_Class* m5sound, uint8_t virtual_sound_channel = 0)
    {
      _m5sound = m5sound;
      _virtual_ch = virtual_sound_channel;
    }
    virtual ~AudioOutputM5Speaker(void) {};

    bool begin(const m5::speaker_config_t& speaker_config, uint32_t sample_rate)
    {
      _sample_rate = sample_rate;

      m5::speaker_config_t effective_config = speaker_config;
      effective_config.sample_rate = sample_rate;
      _i2s_port = effective_config.i2s_port;
      if (effective_config.pin_bck < 0 || effective_config.pin_ws < 0 || effective_config.pin_data_out < 0) {
#if defined(ARDUINO_M5STACK_CORES3)
        effective_config.i2s_port = I2S_NUM_1;
        effective_config.pin_bck = GPIO_NUM_34;
        effective_config.pin_ws = GPIO_NUM_33;
        effective_config.pin_data_out = GPIO_NUM_13;
#else
        effective_config.i2s_port = I2S_NUM_1;
        effective_config.pin_bck = GPIO_NUM_12;
        effective_config.pin_ws = GPIO_NUM_0;
        effective_config.pin_data_out = GPIO_NUM_2;
#endif
      }
      _i2s_port = effective_config.i2s_port;
      _pin_bck = effective_config.pin_bck;
      _pin_ws = effective_config.pin_ws;
      _pin_data_out = effective_config.pin_data_out;
      _pin_mck = effective_config.pin_mck;

      i2s_driver_uninstall(_i2s_port);

      i2s_config_t i2s_config = {};
      i2s_config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
      i2s_config.sample_rate = sample_rate;
      i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
      i2s_config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
      i2s_config.communication_format = static_cast<i2s_comm_format_t>(I2S_COMM_FORMAT_STAND_I2S);
      i2s_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
      i2s_config.dma_buf_count = effective_config.dma_buf_count;
      i2s_config.dma_buf_len = effective_config.dma_buf_len;
      i2s_config.use_apll = false;
      i2s_config.tx_desc_auto_clear = true;
      i2s_config.fixed_mclk = 0;

      i2s_pin_config_t pin_config = {};
      pin_config.bck_io_num = effective_config.pin_bck;
      pin_config.ws_io_num = effective_config.pin_ws;
      pin_config.data_out_num = effective_config.pin_data_out;
      pin_config.data_in_num = I2S_PIN_NO_CHANGE;
#if defined(ESP_IDF_VERSION_VAL)
 #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 1)
      pin_config.mck_io_num = effective_config.pin_mck;
 #endif
#endif

      M5_LOGI("Direct I2S effective port:%d bck:%d ws:%d data:%d mck:%d dma:%u/%u rate:%lu",
              _i2s_port, _pin_bck, _pin_ws, _pin_data_out, _pin_mck,
              (unsigned)effective_config.dma_buf_count, (unsigned)effective_config.dma_buf_len,
              sample_rate);
      _last_error = i2s_driver_install(_i2s_port, &i2s_config, 0, nullptr);
      if (_last_error != ESP_OK) {
        return false;
      }
      _last_error = i2s_set_pin(_i2s_port, &pin_config);
      if (_last_error != ESP_OK) {
        i2s_driver_uninstall(_i2s_port);
        return false;
      }
      i2s_zero_dma_buffer(_i2s_port);
      _last_error = i2s_start(_i2s_port);
      enableAmp(effective_config, sample_rate);
      _started = true;
      return true;
    }

    void setVolume(uint8_t volume)
    {
      _volume = volume;
    }

    bool stop(void)
    {
      i2s_zero_dma_buffer(_i2s_port);
      for (size_t i = 0; i < tri_buffer_count; ++i)
      {
        memset(_tri_buffer[i], 0, tri_buf_size * sizeof(int16_t));
      }
      ++_update_count;
      return true;
    }

    bool playPcm16(const int16_t* samples, size_t sample_count, uint32_t sample_rate, bool stereo)
    {
      if (samples == nullptr || sample_count == 0) return false;

      if (_sample_rate != sample_rate) {
        _last_error = i2s_set_clk(_i2s_port, sample_rate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
        i2s_start(_i2s_port);
        _sample_rate = sample_rate;
        enableAmpForCurrentPins(sample_rate);
      }

      size_t copy_count = sample_count < tri_buf_size ? sample_count : tri_buf_size;
      if (_volume >= 255) {
        memcpy(_tri_buffer[_tri_index], samples, copy_count * sizeof(int16_t));
      } else {
        for (size_t i = 0; i < copy_count; ++i) {
          _tri_buffer[_tri_index][i] = (static_cast<int32_t>(samples[i]) * _volume) / 255;
        }
      }
      if (copy_count < tri_buf_size)
      {
        memset(&_tri_buffer[_tri_index][copy_count], 0, (tri_buf_size - copy_count) * sizeof(int16_t));
      }
      size_t write_bytes = 0;
      esp_err_t err = i2s_write(_i2s_port, _tri_buffer[_tri_index], copy_count * sizeof(int16_t), &write_bytes, pdMS_TO_TICKS(50));
      _last_error = err;
      _tri_index = (_tri_index + 1) % tri_buffer_count;
      bool written = (err == ESP_OK && write_bytes == copy_count * sizeof(int16_t));
      if (written) {
        ++_update_count;
      } else {
        ++_write_fail_count;
      }
      return written;
    }

    const int16_t* getBuffer(void) const { return _tri_buffer[(_tri_index + tri_buffer_count - 1) % tri_buffer_count]; }
    const uint32_t getUpdateCount(void) const { return _update_count; }
    uint32_t getWriteFailCount(void) const { return _write_fail_count; }
    esp_err_t getLastError(void) const { return _last_error; }
    i2s_port_t getPort(void) const { return _i2s_port; }
    int getBckPin(void) const { return _pin_bck; }
    int getWsPin(void) const { return _pin_ws; }
    int getDataOutPin(void) const { return _pin_data_out; }
    bool isAmpEnabled(void) const { return _amp_enabled; }
    bool isStarted(void) const { return _started; }

  protected:
    m5::Speaker_Class* _m5sound;
    uint8_t _virtual_ch;
#if defined(ARDUINO_M5STACK_CORES3)
    i2s_port_t _i2s_port = I2S_NUM_1;
#else
    i2s_port_t _i2s_port = I2S_NUM_1;
#endif
    uint32_t _sample_rate = 0;
    uint8_t _volume = 255;
    bool _started = false;
    bool _amp_enabled = false;
    int _pin_bck = I2S_PIN_NO_CHANGE;
    int _pin_ws = I2S_PIN_NO_CHANGE;
    int _pin_data_out = I2S_PIN_NO_CHANGE;
    int _pin_mck = I2S_PIN_NO_CHANGE;
    uint32_t _write_fail_count = 0;
    esp_err_t _last_error = ESP_OK;
    static constexpr size_t tri_buffer_count = 3;
    static constexpr size_t tri_buf_size = 4096;
    int16_t _tri_buffer[tri_buffer_count][tri_buf_size];
    size_t _tri_index = 0;
    size_t _update_count = 0;

    void enableAmpForCurrentPins(uint32_t sample_rate)
    {
      m5::speaker_config_t speaker_config = {};
      speaker_config.i2s_port = _i2s_port;
      speaker_config.pin_bck = _pin_bck;
      speaker_config.pin_ws = _pin_ws;
      speaker_config.pin_data_out = _pin_data_out;
      speaker_config.pin_mck = _pin_mck;
      speaker_config.sample_rate = sample_rate;
      enableAmp(speaker_config, sample_rate);
    }

    void enableAmp(const m5::speaker_config_t& speaker_config, uint32_t sample_rate)
    {
#if defined(ARDUINO_M5STACK_CORES3)
      if (speaker_config.pin_bck != GPIO_NUM_34) return;
      _amp_enabled = true;
      static constexpr uint8_t aw88298_i2c_addr = 0x36;
      static constexpr uint8_t aw9523_i2c_addr = 0x58;
      M5.In_I2C.bitOn(aw9523_i2c_addr, 0x02, 0b00000100, 400000);

      static constexpr uint8_t rate_tbl[] = {4, 5, 6, 8, 10, 11, 15, 20, 22, 44};
      size_t reg0x06_value = 0;
      size_t rate = (sample_rate + 1102) / 2205;
      while (rate > rate_tbl[reg0x06_value] && ++reg0x06_value < sizeof(rate_tbl)) {}
      reg0x06_value |= 0x14C0;

      writeAw88298(aw88298_i2c_addr, 0x61, 0x0673);
      writeAw88298(aw88298_i2c_addr, 0x04, 0x4040);
      writeAw88298(aw88298_i2c_addr, 0x05, 0x0008);
      writeAw88298(aw88298_i2c_addr, 0x06, reg0x06_value);
      writeAw88298(aw88298_i2c_addr, 0x0C, 0x0064);
#else
      if (speaker_config.pin_bck != GPIO_NUM_12 ||
          speaker_config.pin_ws != GPIO_NUM_0 ||
          speaker_config.pin_data_out != GPIO_NUM_2) {
        return;
      }
      switch (M5.Power.getType()) {
      case m5::Power_Class::pmic_axp192:
        M5.Power.Axp192.setGPIO2(true);
        _amp_enabled = true;
        break;
      case m5::Power_Class::pmic_axp2101:
        M5.Power.Axp2101.setALDO3(3300);
        _amp_enabled = true;
        break;
      default:
        break;
      }
#endif
    }

    void writeAw88298(uint8_t address, uint8_t reg, uint16_t value)
    {
      value = __builtin_bswap16(value);
      M5.In_I2C.writeRegister(address, reg, reinterpret_cast<const uint8_t*>(&value), 2, 400000);
    }
};

#define FFT_SIZE 256
class fft_t
{
  float _wr[FFT_SIZE + 1];
  float _wi[FFT_SIZE + 1];
  float _fr[FFT_SIZE + 1];
  float _fi[FFT_SIZE + 1];
  uint16_t _br[FFT_SIZE + 1];
  size_t _ie;

public:
  fft_t(void)
  {
#ifndef M_PI
#define M_PI 3.141592653
#endif
    _ie = logf( (float)FFT_SIZE ) / log(2.0) + 0.5;
    static constexpr float omega = 2.0f * M_PI / FFT_SIZE;
    static constexpr int s4 = FFT_SIZE / 4;
    static constexpr int s2 = FFT_SIZE / 2;
    for ( int i = 1 ; i < s4 ; ++i)
    {
    float f = cosf(omega * i);
      _wi[s4 + i] = f;
      _wi[s4 - i] = f;
      _wr[     i] = f;
      _wr[s2 - i] = -f;
    }
    _wi[s4] = _wr[0] = 1;

    size_t je = 1;
    _br[0] = 0;
    _br[1] = FFT_SIZE / 2;
    for ( size_t i = 0 ; i < _ie - 1 ; ++i )
    {
      _br[ je << 1 ] = _br[ je ] >> 1;
      je = je << 1;
      for ( size_t j = 1 ; j < je ; ++j )
      {
        _br[je + j] = _br[je] + _br[j];
      }
    }
  }

  void exec(const int16_t* in)
  {
    memset(_fi, 0, sizeof(_fi));
    for ( size_t j = 0 ; j < FFT_SIZE / 2 ; ++j )
    {
      float basej = 0.25 * (1.0-_wr[j]);
      size_t r = FFT_SIZE - j - 1;

      /// perform han window and stereo to mono convert.
      _fr[_br[j]] = basej * (in[j * 2] + in[j * 2 + 1]);
      _fr[_br[r]] = basej * (in[r * 2] + in[r * 2 + 1]);
    }

    size_t s = 1;
    size_t i = 0;
    do
    {
      size_t ke = s;
      s <<= 1;
      size_t je = FFT_SIZE / s;
      size_t j = 0;
      do
      {
        size_t k = 0;
        do
        {
          size_t l = s * j + k;
          size_t m = ke * (2 * j + 1) + k;
          size_t p = je * k;
          float Wxmr = _fr[m] * _wr[p] + _fi[m] * _wi[p];
          float Wxmi = _fi[m] * _wr[p] - _fr[m] * _wi[p];
          _fr[m] = _fr[l] - Wxmr;
          _fi[m] = _fi[l] - Wxmi;
          _fr[l] += Wxmr;
          _fi[l] += Wxmi;
        } while ( ++k < ke) ;
      } while ( ++j < je );
    } while ( ++i < _ie );
  }

  uint32_t get(size_t index)
  {
    return (index < FFT_SIZE / 2) ? (uint32_t)sqrtf(_fr[ index ] * _fr[ index ] + _fi[ index ] * _fi[ index ]) : 0u;
  }
};
