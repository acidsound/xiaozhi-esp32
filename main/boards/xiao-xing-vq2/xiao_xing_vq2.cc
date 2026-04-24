#include "wifi_board.h"
#include "application.h"
#include "button.h"
#include "codecs/dummy_audio_codec.h"
#include "codecs/no_audio_codec.h"
#include "config.h"
#include "display/display.h"
#include "display/oled_display.h"
#include "mcp_server.h"
#include "servo_dog_ctrl.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_event.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "sdkconfig.h"

#ifdef CONFIG_ESP_HI_WEB_CONTROL_ENABLED
#include "esp_hi_web_control.h"
#endif

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#if XIAO_XING_VQ2_ENABLE_LED_STRIP
#include "driver/rmt_tx.h"
#include "led_strip.h"
#endif

#define TAG "XIAO_XING_VQ2"

class XiaoXingVq2 : public WifiBoard {
private:
    static constexpr int kBasicMotionRepeatCount = 1;
    static constexpr int kBasicMotionSpeed = 30;
    static constexpr int kBowLeanRepeatCount = 2;
    static constexpr int kSwayMotionRepeatCount = 4;
    static constexpr int kShakeHandRepeatCount = 10;
    static constexpr int kGestureHoldTimeMs = 500;
    static constexpr int kShakeHandHoldTimeMs = 3000;
    static constexpr int kSwayAngleOffset = 20;

    Button boot_button_;
    Button audio_wake_button_;
    Button move_wake_button_;
    Display* display_ = nullptr;
    i2c_master_bus_handle_t display_i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    bool dog_motion_enabled_ = false;
    bool web_server_initialized_ = false;

#if XIAO_XING_VQ2_ENABLE_LED_STRIP
    led_strip_handle_t led_strip_ = nullptr;
    gpio_num_t active_led_gpio_ = GPIO_NUM_NC;
    bool led_on_ = false;
    uint8_t led_brightness_ = 100;
    uint8_t led_red_ = 0xFF;
    uint8_t led_green_ = 0xFF;
    uint8_t led_blue_ = 0xFF;
#endif

    static bool IsValidGpio(gpio_num_t gpio) {
        return gpio != GPIO_NUM_NC;
    }

#ifdef CONFIG_ESP_HI_WEB_CONTROL_ENABLED
    static void EnsureServoControlNvsValue(nvs_handle_t handle, const char* key) {
        int32_t value = 0;
        esp_err_t err = nvs_get_i32(handle, key, &value);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_ERROR_CHECK(nvs_set_i32(handle, key, 0));
        } else if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to read servo_control.%s: %s", key, esp_err_to_name(err));
        }
    }

    static void EnsureServoControlNvsDefaults() {
        nvs_handle_t handle;
        esp_err_t err = nvs_open("servo_control", NVS_READWRITE, &handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to prepare servo_control NVS namespace: %s", esp_err_to_name(err));
            return;
        }

        EnsureServoControlNvsValue(handle, "fl");
        EnsureServoControlNvsValue(handle, "fr");
        EnsureServoControlNvsValue(handle, "bl");
        EnsureServoControlNvsValue(handle, "br");
        ESP_ERROR_CHECK(nvs_commit(handle));
        nvs_close(handle);
    }

    static void wifi_event_handler(void* arg, esp_event_base_t event_base,
            int32_t event_id, void* event_data) {
        (void)event_data;
        if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
            xTaskCreate(
                [](void* arg) {
                    auto* instance = static_cast<XiaoXingVq2*>(arg);
                    vTaskDelay(pdMS_TO_TICKS(5000));

                    if (!instance->web_server_initialized_) {
                        ESP_LOGI(TAG, "WiFi connected, init web control server");
                        EnsureServoControlNvsDefaults();
                        esp_err_t err = esp_hi_web_control_server_init();
                        if (err != ESP_OK) {
                            ESP_LOGE(TAG, "Failed to initialize web control server: %d", err);
                        } else {
                            ESP_LOGI(TAG, "Web control server initialized");
                            instance->web_server_initialized_ = true;
                        }
                    }

                    vTaskDelete(nullptr);
                },
                "web_server_init",
                1024 * 10, arg, 5, nullptr);
        }
    }
#endif

    void HandleMoveWakePressDown(int64_t current_time, int64_t& last_trigger_time, int& gesture_state) {
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

    void HandleMoveWakePressUp(int64_t current_time, int64_t& last_trigger_time, int& gesture_state) {
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
                    ESP_LOGI(TAG, "move wake gesture detected");
                    gesture_state = 0;
                    auto& app = Application::GetInstance();
                    app.ToggleChatState();
                }
                break;
            }
        }
    }

    void InitializeButtons() {
        static int64_t last_trigger_time = 0;
        static int gesture_state = 0;

        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
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

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

#ifdef SH1106
    void ApplySh1106PanelConfig() {
        // The managed SH1106 driver maps mirror_x to display reverse, so apply
        // the VQ2 panel orientation and color mode explicitly after LVGL setup.
        constexpr uint8_t kSetDisplayNormal = 0xA6;
        constexpr uint8_t kSetDisplayReverse = 0xA7;
        constexpr uint8_t kSetComScanNormal = 0xC0;
        constexpr uint8_t kSetComScanReverse = 0xC8;
        constexpr uint8_t kSetSegmentRemapNormal = 0xA0;
        constexpr uint8_t kSetSegmentRemapInverse = 0xA1;

        ESP_LOGI(TAG, "Apply VQ2 SH1106 config: segment_inverse=%d com_reverse=%d reverse_color=%d",
            DISPLAY_SEGMENT_REMAP_INVERSE, DISPLAY_COM_SCAN_REVERSE, DISPLAY_REVERSE_COLOR);
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_io_,
            DISPLAY_SEGMENT_REMAP_INVERSE ? kSetSegmentRemapInverse : kSetSegmentRemapNormal, nullptr, 0));
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_io_,
            DISPLAY_COM_SCAN_REVERSE ? kSetComScanReverse : kSetComScanNormal, nullptr, 0));
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_io_,
            DISPLAY_REVERSE_COLOR ? kSetDisplayReverse : kSetDisplayNormal, nullptr, 0));
    }
#endif

    void InitializeOledDisplay() {
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
            .scl_speed_hz = 400 * 1000,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(display_i2c_bus_, &io_config, &panel_io_));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        ESP_LOGI(TAG, "Install SH1106 OLED driver");
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_LOGI(TAG, "Install SSD1306 OLED driver");
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "OLED init failed; using NoDisplay fallback");
            display_ = new NoDisplay();
            return;
        }

        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));
        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
#ifdef SH1106
        ApplySh1106PanelConfig();
#endif
    }

    void InitializeDogMotion() {
#if XIAO_XING_VQ2_ENABLE_DOG_MOTION
        if (!IsValidGpio(FL_GPIO_NUM) || !IsValidGpio(FR_GPIO_NUM) ||
                !IsValidGpio(BL_GPIO_NUM) || !IsValidGpio(BR_GPIO_NUM)) {
            ESP_LOGW(TAG, "Dog motion is enabled but one or more servo GPIOs are invalid");
            return;
        }

        servo_dog_ctrl_config_t config = {
            .fl_gpio_num = FL_GPIO_NUM,
            .fr_gpio_num = FR_GPIO_NUM,
            .bl_gpio_num = BL_GPIO_NUM,
            .br_gpio_num = BR_GPIO_NUM,
        };
        servo_dog_ctrl_init(&config);
        dog_motion_enabled_ = true;
#else
        ESP_LOGI(TAG, "Dog motion disabled until VQ2 servo GPIOs are confirmed");
#endif
    }

#if XIAO_XING_VQ2_ENABLE_LED_STRIP
    esp_err_t ConfigureLedStrip(gpio_num_t gpio, bool clear_previous) {
        if (!IsValidGpio(gpio)) {
            ESP_LOGW(TAG, "RGB LED strip disabled because GPIO is not configured");
            return ESP_ERR_INVALID_ARG;
        }

        if (clear_previous && led_strip_ != nullptr) {
            led_strip_clear(led_strip_);
            led_strip_del(led_strip_);
            led_strip_ = nullptr;
            active_led_gpio_ = GPIO_NUM_NC;
        }

        led_strip_config_t strip_config = {
            .strip_gpio_num = gpio,
            .max_leds = RGB_LED_COUNT,
            .led_model = LED_MODEL_WS2812,
            .flags = {
                .invert_out = false,
            },
        };
        led_strip_rmt_config_t rmt_config = {
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = 10 * 1000 * 1000,
            .mem_block_symbols = 0,
            .flags = {
                .with_dma = false,
            },
        };

        esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip_);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to initialize RGB LED strip on GPIO%d: %s",
                static_cast<int>(gpio), esp_err_to_name(ret));
            return ret;
        }

        active_led_gpio_ = gpio;
        ESP_LOGI(TAG, "RGB LED strip initialized on GPIO%d, count=%d",
            static_cast<int>(active_led_gpio_), RGB_LED_COUNT);
        return led_strip_clear(led_strip_);
    }

    void InitializeLedStrip() {
        ConfigureLedStrip(RGB_LED_GPIO, false);
    }

    static uint8_t ScaleLedChannel(uint8_t value, uint8_t brightness) {
        return static_cast<uint8_t>((static_cast<uint16_t>(value) * brightness) / 100);
    }

    esp_err_t ApplyLedState() {
        if (led_strip_ == nullptr) {
            return ESP_ERR_INVALID_STATE;
        }

        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
        if (led_on_ && led_brightness_ > 0) {
            r = ScaleLedChannel(led_red_, led_brightness_);
            g = ScaleLedChannel(led_green_, led_brightness_);
            b = ScaleLedChannel(led_blue_, led_brightness_);
        }

        esp_err_t ret = ESP_OK;
        for (int i = 0; i < RGB_LED_COUNT; ++i) {
            ret |= led_strip_set_pixel(led_strip_, i, r, g, b);
        }
        ret |= led_strip_refresh(led_strip_);
        return ret;
    }

    esp_err_t SetLedColor(uint8_t r, uint8_t g, uint8_t b) {
        led_red_ = r;
        led_green_ = g;
        led_blue_ = b;
        led_on_ = (r != 0 || g != 0 || b != 0);
        if (led_on_ && led_brightness_ == 0) {
            led_brightness_ = 100;
        }
        return ApplyLedState();
    }

    esp_err_t TurnLedOn() {
        if (led_brightness_ == 0) {
            led_brightness_ = 100;
        }
        if (led_red_ == 0 && led_green_ == 0 && led_blue_ == 0) {
            led_red_ = 0xFF;
            led_green_ = 0xFF;
            led_blue_ = 0xFF;
        }
        led_on_ = true;
        return ApplyLedState();
    }

    esp_err_t TurnLedOff() {
        led_on_ = false;
        return ApplyLedState();
    }

    esp_err_t SetLedBrightness(int brightness) {
        if (brightness < 0) {
            brightness = 0;
        } else if (brightness > 100) {
            brightness = 100;
        }
        led_brightness_ = static_cast<uint8_t>(brightness);
        if (led_brightness_ == 0) {
            led_on_ = false;
        }
        return ApplyLedState();
    }
#endif

    bool SendDogAction(servo_dog_state_t state, int repeat_count, int speed) {
        if (!dog_motion_enabled_) {
            return false;
        }

        dog_action_args_t args = {
            .repeat_count = NOT_USE,
            .speed = NOT_USE,
            .hold_time_ms = NOT_USE,
            .angle_offset = NOT_USE,
        };
        dog_action_args_t* args_ptr = &args;
        int resolved_repeat_count = repeat_count;
        int resolved_speed = speed > 0 ? speed : kBasicMotionSpeed;

        switch (state) {
        case DOG_STATE_FORWARD:
        case DOG_STATE_BACKWARD:
        case DOG_STATE_TURN_LEFT:
        case DOG_STATE_TURN_RIGHT: {
            resolved_repeat_count = repeat_count > 0 ? repeat_count : kBasicMotionRepeatCount;
            args.repeat_count = static_cast<int16_t>(resolved_repeat_count);
            args.speed = static_cast<int16_t>(resolved_speed);
            break;
        }
        case DOG_STATE_BOW:
        case DOG_STATE_LEAN_BACK:
            args.speed = static_cast<int16_t>(resolved_speed);
            args.hold_time_ms = kGestureHoldTimeMs;
            break;
        case DOG_STATE_BOW_LEAN:
            resolved_repeat_count = repeat_count > 0 ? repeat_count : kBowLeanRepeatCount;
            args.repeat_count = static_cast<int16_t>(resolved_repeat_count);
            args.speed = static_cast<int16_t>(resolved_speed);
            break;
        case DOG_STATE_SWAY:
            resolved_repeat_count = repeat_count > 0 ? repeat_count : kSwayMotionRepeatCount;
            args.repeat_count = static_cast<int16_t>(resolved_repeat_count);
            args.speed = static_cast<int16_t>(resolved_speed);
            args.angle_offset = kSwayAngleOffset;
            break;
        case DOG_STATE_SHAKE_HAND:
            resolved_repeat_count = repeat_count > 0 ? repeat_count : kShakeHandRepeatCount;
            args.repeat_count = static_cast<int16_t>(resolved_repeat_count);
            args.hold_time_ms = kShakeHandHoldTimeMs;
            break;
        case DOG_STATE_INSTALLATION:
        case DOG_STATE_IDLE:
        case DOG_STATE_LAY_DOWN:
        case DOG_STATE_SWAY_BACK_FORTH:
        case DOG_STATE_POKE:
        case DOG_STATE_SHAKE_BACK_LEGS:
        case DOG_STATE_JUMP_FORWARD:
        case DOG_STATE_JUMP_BACKWARD:
        case DOG_STATE_RETRACT_LEGS:
            args_ptr = nullptr;
            break;
        default:
            ESP_LOGW(TAG, "Unknown dog action: state=%d", static_cast<int>(state));
            return false;
        }

        if (args_ptr == nullptr) {
            ESP_LOGI(TAG, "Dog action: state=%d default args", static_cast<int>(state));
        } else {
            ESP_LOGI(TAG, "Dog action: state=%d repeat_count=%d speed=%d hold_time_ms=%d angle_offset=%d",
                static_cast<int>(state), args.repeat_count, args.speed,
                args.hold_time_ms, args.angle_offset);
        }
        return servo_dog_ctrl_send(state, args_ptr) == ESP_OK;
    }

    void InitializeIot() {
#if XIAO_XING_VQ2_ENABLE_LED_STRIP
        InitializeLedStrip();
        TurnLedOff();
#endif

#ifdef CONFIG_ESP_HI_WEB_CONTROL_ENABLED
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(err);
        }
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED,
            &wifi_event_handler, this));
#endif
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool("self.dog.basic_control", "로봇 강아지의 기본 동작 제어. 사용할 수 있는 동작:\n"
            "forward: 앞으로 이동\nbackward: 뒤로 이동\nturn_left: 왼쪽으로 회전\n"
            "turn_right: 오른쪽으로 회전\nstop: 현재 동작 즉시 정지\n"
            "speed는 동작 속도이며 20=느리게, 30=보통, 40=빠르게입니다. 기본값은 30입니다.\n"
            "repeat_count는 이동/회전 반복 횟수이며 1이 기본값입니다.",
            PropertyList({
                Property("action", kPropertyTypeString),
                Property("speed", kPropertyTypeInteger, kBasicMotionSpeed, 20, 40),
                Property("repeat_count", kPropertyTypeInteger, kBasicMotionRepeatCount, 1, 4),
            }), [this](const PropertyList& properties) -> ReturnValue {
                if (!dog_motion_enabled_) {
                    return false;
                }
                const std::string& action = properties["action"].value<std::string>();
                int speed = properties["speed"].value<int>();
                int repeat_count = properties["repeat_count"].value<int>();
                if (action == "forward") {
                    return SendDogAction(DOG_STATE_FORWARD, repeat_count, speed);
                } else if (action == "backward") {
                    return SendDogAction(DOG_STATE_BACKWARD, repeat_count, speed);
                } else if (action == "turn_left") {
                    return SendDogAction(DOG_STATE_TURN_LEFT, repeat_count, speed);
                } else if (action == "turn_right") {
                    return SendDogAction(DOG_STATE_TURN_RIGHT, repeat_count, speed);
                } else if (action == "stop") {
                    return SendDogAction(DOG_STATE_IDLE, 0, 0);
                } else {
                    return false;
                }
                return true;
            });

        mcp_server.AddTool("self.dog.advanced_control", "로봇 강아지의 확장 동작 제어. 사용할 수 있는 동작:\n"
            "sway_back_forth: 앞뒤로 흔들기\nlay_down: 엎드리기\nsway: 좌우로 흔들기\n"
            "bow: 인사하기\nlean_back: 뒤로 젖히기\nbow_lean: 앞뒤로 인사하기\n"
            "retract_legs: 다리 접기\nshake_hand: 손 흔들기\npoke: 찌르기\n"
            "shake_back_legs: 뒷다리 흔들기\njump_forward: 앞으로 점프\n"
            "jump_backward: 뒤로 점프\n"
            "speed는 지원되는 동작의 속도이며 20=느리게, 30=보통, 40=빠르게입니다. 기본값은 30입니다.\n"
            "repeat_count는 지원되는 동작의 반복 횟수이며 0이면 동작별 기본값을 사용합니다.",
            PropertyList({
                Property("action", kPropertyTypeString),
                Property("speed", kPropertyTypeInteger, kBasicMotionSpeed, 20, 40),
                Property("repeat_count", kPropertyTypeInteger, 0, 0, 10),
            }), [this](const PropertyList& properties) -> ReturnValue {
                if (!dog_motion_enabled_) {
                    return false;
                }
                const std::string& action = properties["action"].value<std::string>();
                int speed = properties["speed"].value<int>();
                int repeat_count = properties["repeat_count"].value<int>();
                if (action == "sway_back_forth") {
                    return SendDogAction(DOG_STATE_SWAY_BACK_FORTH, repeat_count, speed);
                } else if (action == "lay_down") {
                    return SendDogAction(DOG_STATE_LAY_DOWN, repeat_count, speed);
                } else if (action == "sway") {
                    return SendDogAction(DOG_STATE_SWAY, repeat_count, speed);
                } else if (action == "bow") {
                    return SendDogAction(DOG_STATE_BOW, repeat_count, speed);
                } else if (action == "lean_back") {
                    return SendDogAction(DOG_STATE_LEAN_BACK, repeat_count, speed);
                } else if (action == "bow_lean") {
                    return SendDogAction(DOG_STATE_BOW_LEAN, repeat_count, speed);
                } else if (action == "retract_legs") {
                    return SendDogAction(DOG_STATE_RETRACT_LEGS, repeat_count, speed);
                } else if (action == "shake_hand") {
                    return SendDogAction(DOG_STATE_SHAKE_HAND, repeat_count, speed);
                } else if (action == "poke") {
                    return SendDogAction(DOG_STATE_POKE, repeat_count, speed);
                } else if (action == "shake_back_legs") {
                    return SendDogAction(DOG_STATE_SHAKE_BACK_LEGS, repeat_count, speed);
                } else if (action == "jump_forward") {
                    return SendDogAction(DOG_STATE_JUMP_FORWARD, repeat_count, speed);
                } else if (action == "jump_backward") {
                    return SendDogAction(DOG_STATE_JUMP_BACKWARD, repeat_count, speed);
                } else {
                    return false;
                }
            });

#if XIAO_XING_VQ2_ENABLE_LED_STRIP
        mcp_server.AddTool("self.light.get_power", "조명이 켜져 있는지 확인합니다", PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                (void)properties;
                return led_on_;
            });

        mcp_server.AddTool("self.light.turn_on", "조명을 켭니다", PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                (void)properties;
                if (TurnLedOn() != ESP_OK) {
                    return false;
                }
                return true;
            });

        mcp_server.AddTool("self.light.turn_off", "조명을 끕니다", PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                (void)properties;
                if (TurnLedOff() != ESP_OK) {
                    return false;
                }
                return true;
            });

        mcp_server.AddTool("self.light.get_brightness", "조명 밝기를 0-100 값으로 확인합니다", PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                (void)properties;
                return static_cast<int>(led_brightness_);
            });

        mcp_server.AddTool("self.light.set_brightness", "조명 밝기를 0-100 값으로 설정합니다",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 0, 100),
            }), [this](const PropertyList& properties) -> ReturnValue {
                int brightness = properties["brightness"].value<int>();
                if (SetLedBrightness(brightness) != ESP_OK) {
                    return false;
                }
                return true;
            });

        mcp_server.AddTool("self.light.set_rgb", "RGB 조명 색상을 설정합니다",
            PropertyList({
                Property("r", kPropertyTypeInteger, 0, 255),
                Property("g", kPropertyTypeInteger, 0, 255),
                Property("b", kPropertyTypeInteger, 0, 255),
            }), [this](const PropertyList& properties) -> ReturnValue {
                int r = properties["r"].value<int>();
                int g = properties["g"].value<int>();
                int b = properties["b"].value<int>();
                if (SetLedColor(r, g, b) != ESP_OK) {
                    return false;
                }
                return true;
            });
#endif
    }

public:
    XiaoXingVq2() :
        boot_button_(BOOT_BUTTON_GPIO),
        audio_wake_button_(AUDIO_WAKE_BUTTON_GPIO),
        move_wake_button_(MOVE_WAKE_BUTTON_GPIO) {
        InitializeButtons();
        InitializeDisplayI2c();
        InitializeOledDisplay();
        InitializeDogMotion();
        InitializeIot();
        InitializeTools();
    }

    virtual AudioCodec* GetAudioCodec() override {
#if XIAO_XING_VQ2_ENABLE_AUDIO
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
#else
        static DummyAudioCodec audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE);
        return &audio_codec;
#endif
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(XiaoXingVq2);
