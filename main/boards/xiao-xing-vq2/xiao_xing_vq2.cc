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

#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <cstdio>

#if XIAO_XING_VQ2_ENABLE_LED_STRIP
#include "driver/rmt_tx.h"
#include "led_strip.h"
#endif

#define TAG "XIAO_XING_VQ2"

class XiaoXingVq2 : public WifiBoard {
private:
    Button boot_button_;
    Display* display_ = nullptr;
    i2c_master_bus_handle_t display_i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    bool dog_motion_enabled_ = false;

#if XIAO_XING_VQ2_ENABLE_LED_STRIP
    led_strip_handle_t led_strip_ = nullptr;
    gpio_num_t active_led_gpio_ = GPIO_NUM_NC;
    bool led_on_ = false;
#endif

    static bool IsValidGpio(gpio_num_t gpio) {
        return gpio != GPIO_NUM_NC;
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
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

    void InitializeSsd1306Display() {
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

        ESP_LOGI(TAG, "Install SSD1306 OLED driver");
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "OLED init failed; using NoDisplay fallback");
            display_ = new NoDisplay();
            return;
        }

        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));
        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
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

    esp_err_t SetLedColor(uint8_t r, uint8_t g, uint8_t b) {
        if (led_strip_ == nullptr) {
            return ESP_ERR_INVALID_STATE;
        }

        esp_err_t ret = ESP_OK;
        for (int i = 0; i < RGB_LED_COUNT; ++i) {
            ret |= led_strip_set_pixel(led_strip_, i, r, g, b);
        }
        ret |= led_strip_refresh(led_strip_);
        return ret;
    }

    ReturnValue SetLedGpio(int gpio) {
        if (gpio < 0 || gpio >= GPIO_NUM_MAX) {
            return false;
        }

        esp_err_t ret = ConfigureLedStrip(static_cast<gpio_num_t>(gpio), true);
        if (ret != ESP_OK) {
            return false;
        }

        // Use a dim color so a successful probe is visible without being harsh.
        ret = SetLedColor(0x18, 0x18, 0x18);
        led_on_ = (ret == ESP_OK);
        return ret == ESP_OK;
    }

    ReturnValue GetLedStatus() {
        char buffer[96];
        snprintf(buffer, sizeof(buffer), "{\"gpio\":%d,\"count\":%d,\"on\":%s}",
            static_cast<int>(active_led_gpio_), RGB_LED_COUNT, led_on_ ? "true" : "false");
        return std::string(buffer);
    }
#endif

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool("self.dog.basic_control", "Robot dog basic movement control",
            PropertyList({
                Property("action", kPropertyTypeString),
            }), [this](const PropertyList& properties) -> ReturnValue {
                if (!dog_motion_enabled_) {
                    return false;
                }
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

        mcp_server.AddTool("self.dog.advanced_control", "Robot dog advanced movement control",
            PropertyList({
                Property("action", kPropertyTypeString),
            }), [this](const PropertyList& properties) -> ReturnValue {
                if (!dog_motion_enabled_) {
                    return false;
                }
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

#if XIAO_XING_VQ2_ENABLE_LED_STRIP
        mcp_server.AddTool("self.light.get_power", "Get light power state", PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                (void)properties;
                return led_on_;
            });

        mcp_server.AddTool("self.light.turn_on", "Turn on light", PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                (void)properties;
                if (SetLedColor(0xFF, 0xFF, 0xFF) != ESP_OK) {
                    return false;
                }
                led_on_ = true;
                return true;
            });

        mcp_server.AddTool("self.light.turn_off", "Turn off light", PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                (void)properties;
                if (SetLedColor(0x00, 0x00, 0x00) != ESP_OK) {
                    return false;
                }
                led_on_ = false;
                return true;
            });

        mcp_server.AddTool("self.light.set_rgb", "Set RGB light color",
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
                led_on_ = true;
                return true;
            });

        mcp_server.AddTool("self.light.get_status", "Get RGB LED strip status", PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                (void)properties;
                return GetLedStatus();
            });

        mcp_server.AddTool("self.light.set_gpio", "Set RGB LED strip data GPIO for VQ2 bring-up testing",
            PropertyList({
                Property("gpio", kPropertyTypeInteger, 0, GPIO_NUM_MAX - 1),
            }), [this](const PropertyList& properties) -> ReturnValue {
                int gpio = properties["gpio"].value<int>();
                return SetLedGpio(gpio);
            });
#endif
    }

public:
    XiaoXingVq2() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeButtons();
        InitializeDisplayI2c();
        InitializeSsd1306Display();
        InitializeDogMotion();
#if XIAO_XING_VQ2_ENABLE_LED_STRIP
        InitializeLedStrip();
#endif
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
