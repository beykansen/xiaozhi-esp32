#include "wifi_board.h"
#include "tcamerapluss3_audio_codec.h"
#include "display/lcd_display.h"
#include "buddy_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "power_save_timer.h"
#include "i2c_device.h"
#include "sy6970.h"
#include "pin_config.h"
#include "esp_video.h"
#include "ir_filter_controller.h"
#include "settings.h"
#include "system_info.h"
#include "assets/lang_config.h"
#include "wifi_manager.h"
#include "mcp_server.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <esp_lcd_touch_cst816s.h>
#include <esp_lvgl_port.h>
#include <thread>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>

#define TAG "LilygoTCameraPlusS3Board"

constexpr const char* AIBUDDY_SETTINGS_NAMESPACE = "aibuddy";
constexpr const char* SLEEP_SECONDS_KEY = "sleep_seconds";
constexpr int DEFAULT_SLEEP_SECONDS = 60;
constexpr const char* OTA_PATH_SUFFIX = "/ota/";
constexpr const char* CONVERSATION_RESET_PATH = "/conversation/reset";
constexpr const char* CHAT_CLEARED_MESSAGE = "Chat cleared";
constexpr const char* CHAT_CLEAR_FAILED_MESSAGE = "Chat clear failed";
constexpr int64_t BATTERY_LOG_INTERVAL_US = 5 * 60 * 1000 * 1000LL;
constexpr uint8_t SY6970_REG_INPUT_CURRENT_LIMIT = 0x00;
constexpr uint8_t SY6970_REG_CHARGE_CURRENT = 0x04;
constexpr int SY6970_INPUT_LIMIT_OFFSET_MA = 100;
constexpr int SY6970_INPUT_LIMIT_STEP_MA = 50;
constexpr int SY6970_CHARGE_CURRENT_STEP_MA = 64;
constexpr int USB_INPUT_CURRENT_LIMIT_MA = 1500;
constexpr int BATTERY_CHARGE_CURRENT_MA = 1100;

class Pmic : public Sy6970 {
public:

    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Sy6970(i2c_bus, addr) {
        uint8_t chip_id = ReadReg(0x14);
        ESP_LOGI(TAG, "Get sy6970 chip ID: 0x%02X", (chip_id & 0B00111000));

        WriteReg(0x00, 0B00001000); // Disable ILIM pin
        WriteReg(0x02, 0B11011101); // Enable ADC measurement function
        WriteReg(0x07, 0B10001101); // Disable watchdog timer feeding function
        WriteReg(SY6970_REG_INPUT_CURRENT_LIMIT, InputCurrentLimitRegister(USB_INPUT_CURRENT_LIMIT_MA));
        WriteReg(SY6970_REG_CHARGE_CURRENT, ChargeCurrentRegister(BATTERY_CHARGE_CURRENT_MA));
    }

    static uint8_t InputCurrentLimitRegister(int milliamps) {
        return static_cast<uint8_t>((milliamps - SY6970_INPUT_LIMIT_OFFSET_MA) / SY6970_INPUT_LIMIT_STEP_MA);
    }

    static uint8_t ChargeCurrentRegister(int milliamps) {
        return static_cast<uint8_t>(milliamps / SY6970_CHARGE_CURRENT_STEP_MA);
    }
};

class LilygoTCameraPlusS3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    esp_lcd_touch_handle_t touch_ = nullptr;
    BuddyDisplay* buddy_display_ = nullptr;
    Pmic* pmic_;
    LcdDisplay *display_;
    Button boot_button_;
    Button key1_button_;
    PowerSaveTimer* power_save_timer_;
    EspVideo* camera_;

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, GetSleepSeconds(), -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(0);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            pmic_->PowerOff();
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitI2c(){
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_config = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = TOUCH_I2C_SDA_PIN,
            .scl_io_num = TOUCH_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            }
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &i2c_bus_));
    }

    void I2cDetect() {
        uint8_t address;
        printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\r\n");
        for (int i = 0; i < 128; i += 16) {
            printf("%02x: ", i);
            for (int j = 0; j < 16; j++) {
                fflush(stdout);
                address = i + j;
                esp_err_t ret = i2c_master_probe(i2c_bus_, address, pdMS_TO_TICKS(200));
                if (ret == ESP_OK) {
                    printf("%02x ", address);
                } else if (ret == ESP_ERR_TIMEOUT) {
                    printf("UU ");
                } else {
                    printf("-- ");
                }
            }
            printf("\r\n");
        }
    }

    void InitializeTouch() {
        esp_lcd_panel_io_i2c_config_t tp_io_config = {};
        tp_io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS;
        tp_io_config.scl_speed_hz = 400 * 1000;
        tp_io_config.control_phase_bytes = 1;
        tp_io_config.dc_bit_offset = 0;
        tp_io_config.lcd_cmd_bits = 8;
        tp_io_config.lcd_param_bits = 0;
        tp_io_config.flags.disable_control_phase = 1;
        esp_lcd_panel_io_handle_t tp_io_handle = nullptr;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle));

        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH,
            .y_max = DISPLAY_HEIGHT,
            .rst_gpio_num = TP_RST,
            .int_gpio_num = GPIO_NUM_NC,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = DISPLAY_SWAP_XY,
                .mirror_x = DISPLAY_MIRROR_X,
                .mirror_y = DISPLAY_MIRROR_Y,
            },
        };
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst816s(tp_io_handle, &tp_cfg, &touch_));

        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = touch_,
        };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "Touch registered as LVGL input device");
    }


    void InitSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SCLK;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitSy6970() {
        ESP_LOGI(TAG, "Init Sy6970");
        pmic_ = new Pmic(i2c_bus_, SY6970_ADDRESS);
    }

    void InitializeSt7789Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = LCD_CS;
        io_config.dc_gpio_num = LCD_DC;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 60 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片ST7789
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = LCD_RST;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));

        auto buddy_display = new BuddyDisplay(panel_io, panel,
                                     DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        buddy_display->SetControls({
            .get_sleep_seconds = [this]() { return GetSleepSeconds(); },
            .set_sleep_seconds = [this](int seconds) { SetSleepSeconds(seconds); },
            .wake_up = [this]() { power_save_timer_->WakeUp(); },
            .get_status_lines = [this]() { return GetStatusLines(); },
            .reset_conversation = [this]() { ResetConversation(); },
        });
        buddy_display_ = buddy_display;
        display_ = buddy_display;
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            if (power_save_timer_->IsInSleepMode()) {
                power_save_timer_->WakeUp();
            } else {
                power_save_timer_->EnterSleepMode();
            }
        });
        key1_button_.OnClick([this]() {
            if (power_save_timer_->IsInSleepMode()) {
                power_save_timer_->WakeUp();
                return;
            }
            power_save_timer_->WakeUp();
            buddy_display_->OnMainButtonClick();
        });
        key1_button_.OnLongPress([this]() {
            if (power_save_timer_->IsInSleepMode()) {
                power_save_timer_->WakeUp();
                return;
            }
            power_save_timer_->WakeUp();
            buddy_display_->OnMainButtonLongPress();
        });
        key1_button_.OnPressUp([this]() {
            buddy_display_->OnMainButtonReleased();
        });
    }

    void InitializeCamera() {
        static esp_cam_ctlr_dvp_pin_config_t dvp_pin_config = {
            .data_width = CAM_CTLR_DATA_WIDTH_8,
            .data_io = {
                [0] = Y2_GPIO_NUM,
                [1] = Y3_GPIO_NUM,
                [2] = Y4_GPIO_NUM,
                [3] = Y5_GPIO_NUM,
                [4] = Y6_GPIO_NUM,
                [5] = Y7_GPIO_NUM,
                [6] = Y8_GPIO_NUM,
                [7] = Y9_GPIO_NUM,
            },
            .vsync_io = VSYNC_GPIO_NUM,
            .de_io = HREF_GPIO_NUM,
            .pclk_io = PCLK_GPIO_NUM,
            .xclk_io = XCLK_GPIO_NUM,
        };

        esp_video_init_sccb_config_t sccb_config = {
#ifdef CONFIG_BOARD_TYPE_LILYGO_T_CAMERAPLUS_S3_V1_0_V1_1
            .init_sccb = false,
            .i2c_handle = i2c_bus_,
#elif defined CONFIG_BOARD_TYPE_LILYGO_T_CAMERAPLUS_S3_V1_2
            .init_sccb = true,
            .i2c_config = {
                .port = 1,
                .scl_pin = SIOC_GPIO_NUM,
                .sda_pin = SIOD_GPIO_NUM,
            },
#endif
            .freq = 100000,
        };

        esp_video_init_dvp_config_t dvp_config = {
            .sccb_config = sccb_config,
            .reset_pin = RESET_GPIO_NUM,
            .pwdn_pin = PWDN_GPIO_NUM,
            .dvp_pin = dvp_pin_config,
            .xclk_freq = XCLK_FREQ_HZ,
        };

        esp_video_init_config_t video_config = {
            .dvp = &dvp_config,
        };

        camera_ = new EspVideo(video_config);
    }

    void InitializeTools() {
        static IrFilterController irFilter(AP1511B_GPIO);
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.battery.get_status",
            "Get the battery voltage in millivolts, charge level percent, charge current in milliamps and whether the device runs on external (USB) power.",
            PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                auto json = cJSON_CreateObject();
                cJSON_AddNumberToObject(json, "voltage_mv", pmic_->GetBatteryVoltage());
                cJSON_AddNumberToObject(json, "level_percent", pmic_->GetBatteryLevel());
                cJSON_AddNumberToObject(json, "charge_current_ma", pmic_->GetChargeCurrent());
                cJSON_AddBoolToObject(json, "external_power", pmic_->IsPowerGood());
                cJSON_AddBoolToObject(json, "charging", pmic_->IsCharging());
                cJSON_AddBoolToObject(json, "charge_done", pmic_->IsChargingDone());
                return json;
            });
        mcp_server.AddTool("self.screen.sleep",
            "Turn the screen off and put the device into sleep mode. The user wakes it by touching the screen or pressing a button.",
            PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                power_save_timer_->EnterSleepMode();
                return true;
            });
        mcp_server.AddTool("self.screen.wake",
            "Wake the device from sleep mode and turn the screen back on.",
            PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                power_save_timer_->WakeUp();
                return true;
            });
    }

    StatusLines GetStatusLines() {
        auto& wifi = WifiManager::GetInstance();
        auto app_desc = esp_app_get_description();
        StatusLines lines;
        lines.emplace_back("WiFi", wifi.GetSsid());
        lines.emplace_back("IP", wifi.GetIpAddress());
        lines.emplace_back("Signal", std::to_string(wifi.GetRssi()) + " dBm");
        lines.emplace_back("Battery", std::to_string(pmic_->GetBatteryLevel()) + "% " + std::to_string(pmic_->GetBatteryVoltage()) + " mV");
        lines.emplace_back("Charging", pmic_->IsChargingDone() ? "done" : pmic_->IsCharging() ? std::to_string(pmic_->GetChargeCurrent()) + " mA" : "no");
        lines.emplace_back("Power", pmic_->IsPowerGood() ? "USB" : "battery");
        lines.emplace_back("Board", BOARD_NAME);
        lines.emplace_back("Firmware", app_desc->version);
        lines.emplace_back("MAC", SystemInfo::GetMacAddress());
        lines.emplace_back("Uptime", std::to_string(esp_timer_get_time() / 1000000 / 60) + " min");
        lines.emplace_back("Free heap", std::to_string(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024) + " KB");
        return lines;
    }

    int GetSleepSeconds() {
        Settings settings(AIBUDDY_SETTINGS_NAMESPACE, false);
        return settings.GetInt(SLEEP_SECONDS_KEY, DEFAULT_SLEEP_SECONDS);
    }

    void SetSleepSeconds(int seconds) {
        Settings settings(AIBUDDY_SETTINGS_NAMESPACE, true);
        settings.SetInt(SLEEP_SECONDS_KEY, seconds);
        power_save_timer_->SetSecondsToSleep(seconds);
        ESP_LOGI(TAG, "Sleep timeout set to %d seconds", seconds);
    }

    std::string BridgeUrl(const char* path) {
        std::string url = CONFIG_OTA_URL;
        auto suffix = url.rfind(OTA_PATH_SUFFIX);
        if (suffix != std::string::npos) {
            url.erase(suffix);
        }
        return url + path;
    }

    void ResetConversation() {
        std::thread([this]() {
            auto http = GetNetwork()->CreateHttp(0);
            http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
            http->SetHeader("Content-Type", "application/json");
            http->SetContent("{}");
            auto url = BridgeUrl(CONVERSATION_RESET_PATH);
            bool ok = false;
            if (auto opened = http->Open("POST", url); opened) {
                auto status = http->GetStatusCode();
                ok = status && *status == 200;
                http->Close();
            }
            ESP_LOGI(TAG, "Conversation reset via %s: %s", url.c_str(), ok ? "ok" : "failed");
            GetDisplay()->ShowNotification(ok ? CHAT_CLEARED_MESSAGE : CHAT_CLEAR_FAILED_MESSAGE);
        }).detach();
    }

public:
    LilygoTCameraPlusS3Board() : boot_button_(BOOT_BUTTON_GPIO), key1_button_(KEY1_BUTTON_GPIO) {
        InitializePowerSaveTimer();
        InitI2c();
        InitSy6970();
        I2cDetect();
        InitSpi();
        InitializeSt7789Display();
        InitializeTouch();
        InitializeButtons();
        InitializeCamera();
        InitializeTools();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec *GetAudioCodec() override {
        static Tcamerapluss3AudioCodec audio_codec(
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_MIC_I2S_GPIO_BCLK,
            AUDIO_MIC_I2S_GPIO_WS,
            AUDIO_MIC_I2S_GPIO_DATA,
            AUDIO_SPKR_I2S_GPIO_BCLK,
            AUDIO_SPKR_I2S_GPIO_LRCLK,
            AUDIO_SPKR_I2S_GPIO_DATA,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display *GetDisplay() override{
        return display_;
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        bool on_external_power = pmic_->IsPowerGood();
        charging = on_external_power;
        discharging = !on_external_power;
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }

        level = pmic_->GetBatteryLevel();
        LogBatteryStatus(level, on_external_power);
        return true;
    }

    void LogBatteryStatus(int level, bool on_external_power) {
        static int64_t last_log_us = -BATTERY_LOG_INTERVAL_US;
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_log_us < BATTERY_LOG_INTERVAL_US) {
            return;
        }
        last_log_us = now_us;
        ESP_LOGI(TAG, "Battery: %d mV, level %d%%, charge current %d mA, external power %s, charging %s, charge done %s",
                 pmic_->GetBatteryVoltage(), level, pmic_->GetChargeCurrent(), on_external_power ? "yes" : "no",
                 pmic_->IsCharging() ? "yes" : "no", pmic_->IsChargingDone() ? "yes" : "no");
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }
};

DECLARE_BOARD(LilygoTCameraPlusS3Board);
