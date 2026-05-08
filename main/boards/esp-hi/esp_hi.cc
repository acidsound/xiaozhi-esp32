#include "wifi_board.h"
#include "adc_pdm_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "power_save_timer.h"
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_err.h>
#include <esp_wifi.h>
#include <esp_event.h>

#include "display/lcd_display.h"
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include "esp_lcd_ili9341.h"

#include "assets/lang_config.h"
#include "anim_player.h"
#include "brave_search.h"
#include "emoji_display.h"
#include "servo_dog_ctrl.h"
#include "led_strip.h"
#include "driver/rmt_tx.h"
#include "device_state.h"

#include "sdkconfig.h"

#ifdef CONFIG_ESP_HI_WEB_CONTROL_ENABLED
#include "esp_hi_web_control.h"
#endif //CONFIG_ESP_HI_WEB_CONTROL_ENABLED

#define TAG "ESP_HI"

static const ili9341_lcd_init_cmd_t vendor_specific_init[] = {
    {0x11, NULL, 0, 120},     // Sleep out, Delay 120ms
    {0xB1, (uint8_t []){0x05, 0x3A, 0x3A}, 3, 0},
    {0xB2, (uint8_t []){0x05, 0x3A, 0x3A}, 3, 0},
    {0xB3, (uint8_t []){0x05, 0x3A, 0x3A, 0x05, 0x3A, 0x3A}, 6, 0},
    {0xB4, (uint8_t []){0x03}, 1, 0},   // Dot inversion
    {0xC0, (uint8_t []){0x44, 0x04, 0x04}, 3, 0},
    {0xC1, (uint8_t []){0xC0}, 1, 0},
    {0xC2, (uint8_t []){0x0D, 0x00}, 2, 0},
    {0xC3, (uint8_t []){0x8D, 0x6A}, 2, 0},
    {0xC4, (uint8_t []){0x8D, 0xEE}, 2, 0},
    {0xC5, (uint8_t []){0x08}, 1, 0},
    {0xE0, (uint8_t []){0x0F, 0x10, 0x03, 0x03, 0x07, 0x02, 0x00, 0x02, 0x07, 0x0C, 0x13, 0x38, 0x0A, 0x0E, 0x03, 0x10}, 16, 0},
    {0xE1, (uint8_t []){0x10, 0x0B, 0x04, 0x04, 0x10, 0x03, 0x00, 0x03, 0x03, 0x09, 0x17, 0x33, 0x0B, 0x0C, 0x06, 0x10}, 16, 0},
    {0x35, (uint8_t []){0x00}, 1, 0},
    {0x3A, (uint8_t []){0x05}, 1, 0},
    {0x36, (uint8_t []){0xC8}, 1, 0},
    {0x29, NULL, 0, 0},     // Display on
    {0x2C, NULL, 0, 0},     // Memory write
};

static const led_strip_config_t bsp_strip_config = {
    .strip_gpio_num = GPIO_NUM_8,
    .max_leds = 4,
    .led_model = LED_MODEL_WS2812,
    .flags = {
        .invert_out = false
    }
};

static const led_strip_rmt_config_t bsp_rmt_config = {
    .clk_src = RMT_CLK_SRC_DEFAULT,
    .resolution_hz = 10 * 1000 * 1000,
    .flags = {
        .with_dma = false
    }
};

class EspHi : public WifiBoard {
private:
    Button boot_button_;
    Button audio_wake_button_;
    Button move_wake_button_;
    anim::EmojiWidget* display_ = nullptr;
    bool web_server_initialized_ = false;
    bool web_server_start_pending_ = false;
    bool web_control_suspended_ = false;
    bool web_control_stopped_for_sleep_ = false;
    PowerSaveTimer* power_save_timer_ = nullptr;
    led_strip_handle_t led_strip_;
    bool led_on_ = false;

#ifdef CONFIG_ESP_HI_WEB_CONTROL_ENABLED
    void StartWebControlServerAsync()
    {
        if (web_server_initialized_ || web_server_start_pending_ || web_control_suspended_) {
            return;
        }

        web_server_start_pending_ = true;
        BaseType_t result = xTaskCreate(
            [](void* arg) {
                EspHi* instance = static_cast<EspHi*>(arg);

                vTaskDelay(pdMS_TO_TICKS(5000));

                for (int attempt = 1; attempt <= 3 && !instance->web_server_initialized_ &&
                        !instance->web_control_suspended_; ++attempt) {
                    ESP_LOGI(TAG, "WiFi connected, init web control server (attempt %d)", attempt);
                    esp_err_t err = esp_hi_web_control_server_init();
                    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
                        ESP_LOGI(TAG, "Web control server initialized");
                        instance->web_server_initialized_ = true;
                        break;
                    }

                    ESP_LOGW(TAG, "Failed to initialize web control server: %d", err);
                    vTaskDelay(pdMS_TO_TICKS(3000));
                }

                instance->web_server_start_pending_ = false;
                vTaskDelete(NULL);
            },
            "web_server_init",
            1024 * 3, this, 5, nullptr);

        if (result != pdPASS) {
            web_server_start_pending_ = false;
            ESP_LOGW(TAG, "Failed to create web control init task");
        }
    }

    void StopWebControlServer()
    {
        web_control_suspended_ = true;
        if (web_server_initialized_) {
            esp_err_t err = esp_hi_web_control_server_deinit();
            if (err == ESP_OK) {
                web_server_initialized_ = false;
            } else {
                ESP_LOGW(TAG, "Failed to stop web control server: %d", err);
            }
        }
    }

    static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                 int32_t event_id, void* event_data)
    {
        if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
            EspHi* instance = static_cast<EspHi*>(arg);
            instance->StartWebControlServerAsync();
        }
    }
#endif //CONFIG_ESP_HI_WEB_CONTROL_ENABLED

    void WakePowerSaveTimer()
    {
        if (power_save_timer_ != nullptr) {
            power_save_timer_->WakeUp();
        }
    }

    void HandleMoveWakePressDown(int64_t current_time, int64_t &last_trigger_time, int &gesture_state)
    {
        int64_t interval = last_trigger_time == 0 ? 0 : current_time - last_trigger_time;
        last_trigger_time = current_time;

        if (interval > 1000) {
            gesture_state = 0;
        } else {
            switch (gesture_state) {
            case 0:
                break;
            case 1:
                if (interval > 300) {
                    gesture_state = 2;
                }
                break;
            case 2:
                if (interval > 100) {
                    gesture_state = 0;
                }
                break;
            }
        }
    }

    void HandleMoveWakePressUp(int64_t current_time, int64_t &last_trigger_time, int &gesture_state)
    {
        int64_t interval = current_time - last_trigger_time;

        if (interval > 1000) {
            gesture_state = 0;
        } else {
            switch (gesture_state) {
            case 0:
                if (interval > 300) {
                    gesture_state = 1;
                }
                break;
            case 1:
                break;
            case 2:
                if (interval < 100) {
                    ESP_LOGI(TAG, "gesture detected");
                    gesture_state = 0;
                    WakePowerSaveTimer();
                    auto &app = Application::GetInstance();
                    app.ToggleChatState();
                }
                break;
            }
        }
    }

    void InitializeButtons()
    {
        static int64_t last_trigger_time = 0;
        static int gesture_state = 0;  // 0: init, 1: wait second long interval, 2: wait oscillation

        boot_button_.OnClick([this]() {
            auto &app = Application::GetInstance();
            // During startup (before connected), pressing BOOT button enters Wi-Fi config mode without reboot
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            WakePowerSaveTimer();
            app.ToggleChatState();
        });

        audio_wake_button_.OnPressDown([this]() {
        });

        audio_wake_button_.OnPressUp([this]() {
        });

        move_wake_button_.OnPressDown([this]() {
            int64_t current_time = esp_timer_get_time() / 1000;
            HandleMoveWakePressDown(current_time, last_trigger_time, gesture_state);
        });

        move_wake_button_.OnPressUp([this]() {
            int64_t current_time = esp_timer_get_time() / 1000;
            HandleMoveWakePressUp(current_time, last_trigger_time, gesture_state);
        });
    }
    
    void InitializeLed() {
        ESP_LOGI(TAG, "BLINK_GPIO setting %d", bsp_strip_config.strip_gpio_num);

        ESP_ERROR_CHECK(led_strip_new_rmt_device(&bsp_strip_config, &bsp_rmt_config, &led_strip_));
        led_strip_set_pixel(led_strip_, 0, 0x00, 0x00, 0x00);
        led_strip_set_pixel(led_strip_, 1, 0x00, 0x00, 0x00);
        led_strip_set_pixel(led_strip_, 2, 0x00, 0x00, 0x00);
        led_strip_set_pixel(led_strip_, 3, 0x00, 0x00, 0x00);
        led_strip_refresh(led_strip_);
    }

    esp_err_t SetLedColor(uint8_t r, uint8_t g, uint8_t b) {
        esp_err_t ret = ESP_OK;

        ret |= led_strip_set_pixel(led_strip_, 0, r, g, b);
        ret |= led_strip_set_pixel(led_strip_, 1, r, g, b);
        ret |= led_strip_set_pixel(led_strip_, 2, r, g, b);
        ret |= led_strip_set_pixel(led_strip_, 3, r, g, b);
        ret |= led_strip_refresh(led_strip_);
        return ret;
    }

    void InitializeIot()
    {
        ESP_LOGI(TAG, "Initialize Iot");
        InitializeLed();
        SetLedColor(0x00, 0x00, 0x00);

#ifdef CONFIG_ESP_HI_WEB_CONTROL_ENABLED
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(err);
        }
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED,
                                                 &wifi_event_handler, this));
#endif //CONFIG_ESP_HI_WEB_CONTROL_ENABLED
    }

    void InitializePowerSaveTimer()
    {
        power_save_timer_ = new PowerSaveTimer(-1, 60, -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            if (display_ != nullptr) {
                display_->SetPowerSaveMode(true);
            }
#if CONFIG_ESP_HI_WEB_CONTROL_ENABLED && CONFIG_IDF_TARGET_ESP32C3
            web_control_stopped_for_sleep_ = true;
            StopWebControlServer();
#endif
        });
        power_save_timer_->OnExitSleepMode([this]() {
            if (display_ != nullptr) {
                display_->SetPowerSaveMode(false);
            }
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeSpi()
    {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * 10 * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay()
    {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        const ili9341_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(ili9341_lcd_init_cmd_t),
        };

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = (void *) &vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_set_gap(panel, 0, 24);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        ESP_LOGI(TAG, "LCD panel create success, %p", panel);

        esp_lcd_panel_disp_on_off(panel, true);

        ESP_LOGI(TAG, "Create emoji widget, panel: %p, panel_io: %p", panel, panel_io);
        display_ = new anim::EmojiWidget(panel, panel_io);

#if CONFIG_ESP_CONSOLE_NONE
        servo_dog_ctrl_config_t config = {
            .fl_gpio_num = FL_GPIO_NUM,
            .fr_gpio_num = FR_GPIO_NUM,
            .bl_gpio_num = BL_GPIO_NUM,
            .br_gpio_num = BR_GPIO_NUM,
        };

        ESP_LOGI(TAG, "Servo GPIO map: FL=%d FR=%d BL=%d BR=%d",
            config.fl_gpio_num, config.fr_gpio_num, config.bl_gpio_num, config.br_gpio_num);
        esp_err_t err = servo_dog_ctrl_init(&config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize servo dog controller: %s", esp_err_to_name(err));
        }
#endif
    }

    void InitializeTools()
    {
        auto& mcp_server = McpServer::GetInstance();

#if CONFIG_IDF_TARGET_ESP32C3
        const char* wifi_info_description = "Wi-Fi SSID, IP, 웹 조작 주소를 확인합니다.";
        const char* dog_basic_description = "로봇 기본 이동: forward, backward, turn_left, turn_right, stop.";
        const char* dog_advanced_description =
            "로봇 동작: sway_back_forth, lay_down, sway, retract_legs, shake_hand, shake_back_legs, jump_forward.";
        const char* light_get_description = "조명 켜짐 상태를 확인합니다.";
        const char* light_on_description = "조명을 켭니다.";
        const char* light_off_description = "조명을 끕니다.";
        const char* light_rgb_description = "조명 RGB 색을 0-255 값으로 설정합니다.";
#else
        const char* wifi_info_description =
            "현재 Wi-Fi 네트워크 정보를 확인합니다. 사용자가 IP 주소, 웹 조작 주소, 같은 네트워크에서 접속할 주소, SSID, 신호 세기를 물어보면 이 도구를 사용하세요.";
        const char* dog_basic_description = "机器人的基础动作。机器人可以做以下基础动作：\n"
            "forward: 向前移动\nbackward: 向后移动\nturn_left: 向左转\nturn_right: 向右转\nstop: 立即停止当前动作";
        const char* dog_advanced_description = "机器人的扩展动作。机器人可以做以下扩展动作：\n"
            "sway_back_forth: 前后摇摆\nlay_down: 趴下\nsway: 左右摇摆\nretract_legs: 收回腿部\n"
            "shake_hand: 握手\nshake_back_legs: 伸懒腰\njump_forward: 向前跳跃";
        const char* light_get_description = "获取灯是否打开";
        const char* light_on_description = "打开灯";
        const char* light_off_description = "关闭灯";
        const char* light_rgb_description = "设置RGB颜色";
#endif

        mcp_server.AddTool("self.network.get_wifi_info",
            wifi_info_description,
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                (void)properties;
                return cJSON_Parse(GetBoardJson().c_str());
            });

        mcp_server.AddTool("self.web.search",
#if CONFIG_IDF_TARGET_ESP32C3
            "Brave Search API로 최신 웹 정보를 확인합니다. 최근/오늘/검색 요청에 사용하세요. ESP32-C3에서는 결과 1개만 반환합니다. ok=true이면 핵심을 한국어로 요약하고 출처 제목과 URL을 말하세요. ok=false이면 실패 이유와 조치를 짧게 안내하세요.",
#else
            "Brave Search API로 최신 웹 정보를 검색하거나 조사합니다. 사용자가 '검색해줘', '알아봐줘', '찾아봐줘', '최근', '오늘', '요즘'처럼 빠른 최신 정보나 뉴스 확인을 요청하면 mode='web'을 사용하세요. 사용자가 '조사해줘', '자세히 알아봐줘', '근거까지', '출처 내용을 보고', '본문 기준으로', '비교해서', '분석해줘', '왜 그런지'처럼 깊이 있는 확인을 요청하면 mode='context'를 사용하세요. mode='context' 결과가 ok=true이면 단순히 검색했다고 하지 말고 '조사해봤다', '출처 내용을 살펴봤다'처럼 말하고, 근거를 자연스럽게 요약하세요. 결과를 그대로 읽지 말고 핵심만 한국어로 요약하고, 중요한 출처 1-3개를 함께 말하세요. quota_warning=true이면 답변 끝에 quota_warning_message를 짧게 덧붙이세요. ok=false 응답이면 기다리거나 재시도하지 말고 실패 이유와 필요한 조치를 짧게 안내한 뒤, 일반 지식으로 답할 수 있는 범위만 답하세요.",
#endif
#if CONFIG_IDF_TARGET_ESP32C3
            PropertyList({
                Property("query", kPropertyTypeString),
            }), [](const PropertyList& properties) -> ReturnValue {
                std::string query = properties["query"].value<std::string>();
                return brave_search::SearchText(query, 1, "web");
            });
#else
            PropertyList({
                Property("query", kPropertyTypeString),
                Property("max_results", kPropertyTypeInteger, 1, 1, 3),
                Property("mode", kPropertyTypeString, "auto"),
            }), [](const PropertyList& properties) -> ReturnValue {
                std::string query = properties["query"].value<std::string>();
                int max_results = properties["max_results"].value<int>();
                std::string mode = properties["mode"].value<std::string>();
                return brave_search::Search(query, max_results, mode);
            });
#endif

#if !CONFIG_IDF_TARGET_ESP32C3
        mcp_server.AddTool("self.web.search.get_config_status",
            "Brave Search 설정 상태를 확인합니다. 실제 API 키 문자열은 이 도구의 반환값에 존재하지 않고 configured 여부만 반환됩니다.",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                (void)properties;
                return brave_search::GetConfigStatus();
            });
#endif
        
        // 基础动作控制
        mcp_server.AddTool("self.dog.basic_control", dog_basic_description,
            PropertyList({
                Property("action", kPropertyTypeString),
            }), [this](const PropertyList& properties) -> ReturnValue {
                const std::string& action = properties["action"].value<std::string>();
                if (action == "forward") {
                    servo_dog_ctrl_send(DOG_STATE_FORWARD, NULL);
                } else if (action == "backward") {
                    servo_dog_ctrl_send(DOG_STATE_BACKWARD, NULL);
                } else if (action == "turn_left") {
                    servo_dog_ctrl_send(DOG_STATE_TURN_LEFT, NULL);
                } else if (action == "turn_right") {
                    servo_dog_ctrl_send(DOG_STATE_TURN_RIGHT, NULL);
                } else if (action == "stop") {
                    servo_dog_ctrl_send(DOG_STATE_IDLE, NULL);
                } else {
                    return false;
                }
                return true;
            });
        
        // 扩展动作控制
        mcp_server.AddTool("self.dog.advanced_control", dog_advanced_description,
            PropertyList({
                Property("action", kPropertyTypeString),
            }), [this](const PropertyList& properties) -> ReturnValue {
                const std::string& action = properties["action"].value<std::string>();
                if (action == "sway_back_forth") {
                    servo_dog_ctrl_send(DOG_STATE_SWAY_BACK_FORTH, NULL);
                } else if (action == "lay_down") {
                    servo_dog_ctrl_send(DOG_STATE_LAY_DOWN, NULL);
                } else if (action == "sway") {
                    dog_action_args_t args = {
                        .repeat_count = 4,
                    };
                    servo_dog_ctrl_send(DOG_STATE_SWAY, &args);
                } else if (action == "retract_legs") {
                    servo_dog_ctrl_send(DOG_STATE_RETRACT_LEGS, NULL);
                } else if (action == "shake_hand") {
                    servo_dog_ctrl_send(DOG_STATE_SHAKE_HAND, NULL);
                } else if (action == "shake_back_legs") {
                    servo_dog_ctrl_send(DOG_STATE_SHAKE_BACK_LEGS, NULL);
                } else if (action == "jump_forward") {
                    servo_dog_ctrl_send(DOG_STATE_JUMP_FORWARD, NULL);
                } else {
                    return false;
                }
                return true;
            });

        // 灯光控制
        mcp_server.AddTool("self.light.get_power", light_get_description, PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            return led_on_;
        });

        mcp_server.AddTool("self.light.turn_on", light_on_description, PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            SetLedColor(0xFF, 0xFF, 0xFF);
            led_on_ = true;
            return true;
        });

        mcp_server.AddTool("self.light.turn_off", light_off_description, PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            SetLedColor(0x00, 0x00, 0x00);
            led_on_ = false;
            return true;
        });

        mcp_server.AddTool("self.light.set_rgb", light_rgb_description, PropertyList({
            Property("r", kPropertyTypeInteger, 0, 255),
            Property("g", kPropertyTypeInteger, 0, 255),
            Property("b", kPropertyTypeInteger, 0, 255)
        }), [this](const PropertyList& properties) -> ReturnValue {
            int r = properties["r"].value<int>();
            int g = properties["g"].value<int>();
            int b = properties["b"].value<int>();

            led_on_ = true;
            SetLedColor(r, g, b);
            return true;
        });
    }

public:
    EspHi() : boot_button_(BOOT_BUTTON_GPIO),
        audio_wake_button_(AUDIO_WAKE_BUTTON_GPIO),
        move_wake_button_(MOVE_WAKE_BUTTON_GPIO)
    {
        InitializeButtons();
        InitializeIot();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializePowerSaveTimer();
        InitializeTools();
    }

    virtual void OnAudioInteractionStarting() override
    {
        SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        WakePowerSaveTimer();
    }

    virtual void OnAudioInteractionFinished() override
    {
#if CONFIG_ESP_HI_WEB_CONTROL_ENABLED && CONFIG_IDF_TARGET_ESP32C3
        if (web_control_stopped_for_sleep_) {
            web_control_stopped_for_sleep_ = false;
            web_control_suspended_ = false;
            StartWebControlServerAsync();
        }
#endif
    }

    virtual AudioCodec* GetAudioCodec() override
    {
        static AdcPdmAudioCodec audio_codec(
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_ADC_MIC_CHANNEL,
            AUDIO_PDM_SPEAK_P_GPIO,
            AUDIO_PDM_SPEAK_N_GPIO,
            AUDIO_PA_CTL_GPIO);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override
    {
        return display_;
    }
};

DECLARE_BOARD(EspHi);
