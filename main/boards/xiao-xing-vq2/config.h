#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

/*
 * Preliminary VQ2 pin map.
 *
 * The photos confirm the device uses an ESP32-S3-N16R8 module, an I2C OLED
 * display, an I2S MEMS microphone, dog servos, and multiple RGB LEDs. Some
 * GPIO numbers still need hardware confirmation from the original firmware,
 * traces, or probe tests.
 */
#define XIAO_XING_VQ2_ENABLE_AUDIO      1
#define XIAO_XING_VQ2_ENABLE_LED_STRIP  1
#define XIAO_XING_VQ2_ENABLE_DOG_MOTION 1

/* Candidate pins from similar ESP32-S3 0.96" OLED + I2S microphone boards. */
#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_15
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_16

#define BOOT_BUTTON_GPIO        GPIO_NUM_0

/* Candidate OLED I2C pins. Confirm before treating as final. */
#define DISPLAY_SDA_PIN GPIO_NUM_41
#define DISPLAY_SCL_PIN GPIO_NUM_42
#define DISPLAY_WIDTH   128
#define DISPLAY_HEIGHT  64
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true

/*
 * The board appears to have four RGB LEDs. Factory firmware strings expose a
 * GPIO9 hint, so start there, while the VQ2 board exposes a runtime LED GPIO
 * probe tool for confirming or changing the pin without reflashing.
 */
#define RGB_LED_GPIO   GPIO_NUM_9
#define RGB_LED_COUNT  4

/*
 * Servo pins follow the ESP-HI dog layout for initial VQ2 testing. GPIO19/20
 * are also native USB pins on ESP32-S3, so this remains a bring-up hypothesis.
 */
#define FL_GPIO_NUM GPIO_NUM_21
#define FR_GPIO_NUM GPIO_NUM_19
#define BL_GPIO_NUM GPIO_NUM_20
#define BR_GPIO_NUM GPIO_NUM_18

#endif // _BOARD_CONFIG_H_
