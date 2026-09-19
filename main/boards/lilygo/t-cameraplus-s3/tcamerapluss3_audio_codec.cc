#include "tcamerapluss3_audio_codec.h"

#include <vector>

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/i2s_tdm.h>
#include <driver/i2s_pdm.h>

#include "config.h"

static const char TAG[] = "Tcamerapluss3AudioCodec";

constexpr uint32_t I2S_TRANSFER_TIMEOUT_MS = 1000;

Tcamerapluss3AudioCodec::Tcamerapluss3AudioCodec(int input_sample_rate, int output_sample_rate,
    gpio_num_t mic_bclk, gpio_num_t mic_ws, gpio_num_t mic_data,
    gpio_num_t spkr_bclk, gpio_num_t spkr_lrclk, gpio_num_t spkr_data,
    bool input_reference) {
    duplex_ = true;                             // 是否双工
    input_reference_ = input_reference;         // 是否使用参考输入，实现回声消除
    input_channels_ = input_reference_ ? 2 : 1; // 输入通道数
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    CreateVoiceHardware(mic_bclk, mic_ws, mic_data, spkr_bclk, spkr_lrclk, spkr_data);

    ESP_LOGI(TAG, "Tcamerapluss3AudioCodec initialized");

#ifdef CONFIG_BOARD_TYPE_LILYGO_T_CAMERAPLUS_S3_V1_2
    gpio_config_t config_mic_spkr_en;
    config_mic_spkr_en.pin_bit_mask = BIT64(AUDIO_MIC_SPKR_EN);
    config_mic_spkr_en.mode = GPIO_MODE_OUTPUT;
    config_mic_spkr_en.pull_up_en = GPIO_PULLUP_ENABLE;
    config_mic_spkr_en.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config_mic_spkr_en.intr_type = GPIO_INTR_DISABLE;
#if SOC_GPIO_SUPPORT_PIN_HYS_FILTER
config_mic_spkr_en.hys_ctrl_mode = GPIO_HYS_SOFT_ENABLE;
#endif
    gpio_config(&config_mic_spkr_en);
    gpio_set_level(AUDIO_MIC_SPKR_EN, 0);
#endif
}

Tcamerapluss3AudioCodec::~Tcamerapluss3AudioCodec() {
    audio_codec_delete_codec_if(in_codec_if_);
    audio_codec_delete_ctrl_if(in_ctrl_if_);
    audio_codec_delete_codec_if(out_codec_if_);
    audio_codec_delete_ctrl_if(out_ctrl_if_);
    audio_codec_delete_gpio_if(gpio_if_);
    audio_codec_delete_data_if(data_if_);
}

void Tcamerapluss3AudioCodec::CreateVoiceHardware(gpio_num_t mic_bclk, gpio_num_t mic_ws, gpio_num_t mic_data,
    gpio_num_t spkr_bclk, gpio_num_t spkr_lrclk, gpio_num_t spkr_data) {
    
    i2s_chan_config_t mic_chan_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    mic_chan_config.auto_clear = true; // Auto clear the legacy data in the DMA buffer
    i2s_chan_config_t spkr_chan_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    spkr_chan_config.auto_clear = true; // Auto clear the legacy data in the DMA buffer

    ESP_ERROR_CHECK(i2s_new_channel(&mic_chan_config, NULL, &rx_handle_));
    ESP_ERROR_CHECK(i2s_new_channel(&spkr_chan_config, &tx_handle_, NULL));

    #ifdef CONFIG_BOARD_TYPE_LILYGO_T_CAMERAPLUS_S3_V1_0_V1_1
    i2s_std_config_t mic_config = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)input_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            #ifdef   I2S_HW_VERSION_2    
                .ext_clk_freq_hz = 0,
            #endif
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = mic_bclk,
            .ws = mic_ws,
            .dout = I2S_GPIO_UNUSED,
            .din = mic_data,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = true // 默认右通道
            }
        }
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &mic_config));
#elif defined CONFIG_BOARD_TYPE_LILYGO_T_CAMERAPLUS_S3_V1_2
    i2s_pdm_rx_config_t mic_config = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(static_cast<uint32_t>(input_sample_rate_)),
        /* The data bit-width of PDM mode is fixed to 16 */
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = mic_ws,
            .din = mic_data,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(rx_handle_, &mic_config));
#endif

    i2s_std_config_t spkr_config = {
        .clk_cfg ={
            .sample_rate_hz = static_cast<uint32_t>(11025),
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg ={
            .mclk = I2S_GPIO_UNUSED,
            .bclk = spkr_bclk,
            .ws = spkr_lrclk,
            .dout = spkr_data,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &spkr_config));
    ESP_LOGI(TAG, "Voice hardware created");
}

void Tcamerapluss3AudioCodec::SetOutputVolume(int volume) {
    volume_ = volume;
    AudioCodec::SetOutputVolume(volume);
}

void Tcamerapluss3AudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == input_enabled_) {
        return;
    }
    if (enable) {
        ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    } else {
        ESP_ERROR_CHECK(i2s_channel_disable(rx_handle_));
    }
    AudioCodec::EnableInput(enable);
}

void Tcamerapluss3AudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == output_enabled_) {
        return;
    }
    if (enable) {
        ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    } else {
        ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
    }
    AudioCodec::EnableOutput(enable);
}

int Tcamerapluss3AudioCodec::Read(int16_t *dest, int samples) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (input_enabled_) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(rx_handle_, dest, samples * sizeof(int16_t), &bytes_read, I2S_TRANSFER_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S read failed: %s", esp_err_to_name(err));
            return 0;
        }
        
        // 麦克风接收音量放大20倍（限制在 int16_t 范围内防止溢出）
        int16_t *ptr = dest;
        for (int i = 0; i < samples; i++) {
            int32_t amplified = *ptr * 20;
            *ptr++ = (amplified > 32767) ? 32767 : (amplified < -32768) ? -32768 : amplified;
        }
    }
    return samples;
}

int Tcamerapluss3AudioCodec::Write(const int16_t *data, int samples){
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (output_enabled_){
        size_t bytes_written = 0;
        std::vector<int16_t> output_data(samples);
        for (int i = 0; i < samples; i++){
            output_data[i] = (float)data[i] * (float)(volume_ / 100.0);
        }
        esp_err_t err = i2s_channel_write(tx_handle_, output_data.data(), samples * sizeof(int16_t), &bytes_written, I2S_TRANSFER_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(err));
            return 0;
        }
    }
    return samples;
}
