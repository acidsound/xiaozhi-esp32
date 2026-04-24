#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

/*
 * Xiao Xing VQ2 pin map.
 *
 * Display, audio, and servo pins are based on the factory zzpet-s3 firmware
 * image and confirmed on hardware. GPIO8 drives the two lower RGB LEDs;
 * the other visible front LEDs may be separate circuits and still need confirmation.
 */
#define XIAO_XING_VQ2_ENABLE_AUDIO      1
#define XIAO_XING_VQ2_ENABLE_LED_STRIP  1
#define XIAO_XING_VQ2_ENABLE_DOG_MOTION 1

#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_15
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_16

#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define AUDIO_WAKE_BUTTON_GPIO  GPIO_NUM_NC
#define MOVE_WAKE_BUTTON_GPIO   GPIO_NUM_NC

#define DISPLAY_SDA_PIN GPIO_NUM_41
#define DISPLAY_SCL_PIN GPIO_NUM_42
#define DISPLAY_WIDTH   128
#define DISPLAY_HEIGHT  64
#define SH1106
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SEGMENT_REMAP_INVERSE false
#define DISPLAY_COM_SCAN_REVERSE false
#define DISPLAY_REVERSE_COLOR true

/* GPIO8 drives the confirmed lower two WS2812 LEDs. */
#define RGB_LED_GPIO   GPIO_NUM_8
#define RGB_LED_COUNT  2

/*
 * Factory firmware exposes five oscillator slots:
 * left_hide_leg=18, right_hide_leg=14, left_front_leg=17,
 * right_front_leg=13, tail_leg=12. The retail VQ2 appears to populate four
 * servos, so tail is kept as an optional probe pin.
 */
#define FL_GPIO_NUM   GPIO_NUM_17
#define FR_GPIO_NUM   GPIO_NUM_13
#define BL_GPIO_NUM   GPIO_NUM_18
#define BR_GPIO_NUM   GPIO_NUM_14
#define TAIL_GPIO_NUM GPIO_NUM_12

#endif // _BOARD_CONFIG_H_
