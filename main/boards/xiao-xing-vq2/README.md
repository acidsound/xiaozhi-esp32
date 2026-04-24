# Xiao Xing VQ2

Preliminary board definition for the Xiao Xing VQ2 ESP32-S3-N16R8 robot board.

Known hardware from board photos:

- ESP32-S3-N16R8 module, expected 16 MB flash and 8 MB PSRAM.
- I2C OLED display module with `VCC`, `GND`, `SCK`, `SDA`.
- I2S MEMS microphone module with `SD`, `VDD`, `GND`, `L/R`, `WS`, `SCK`.
- Four visible color LEDs, one near USB and three around the front/back body.
- Four servo outputs.

The GPIO map is not confirmed yet. This board target keeps the dog servo
layout compatible with ESP-HI for hardware testing, enables the common S3
OLED-board I2S audio pin map, and starts RGB LED strip probing from GPIO9
based on factory-firmware string hints.

Suggested bring-up order:

1. Back up the factory firmware.
2. Confirm the chip and flash size with `esptool.py --chip esp32s3 flash_id`.
3. Flash this target and check whether the OLED initializes.
4. Confirm I2S microphone/speaker pins. Audio is already initialized with
   candidate pins in `config.h`.
5. Confirm I2C OLED pins/address, then adjust `DISPLAY_SDA_PIN`,
   `DISPLAY_SCL_PIN`, display type, and mirror flags.
6. Test the four servo outputs with the ESP-HI-compatible pin map. GPIO19/20
   overlap ESP32-S3 native USB, so treat the current servo map as a hypothesis
   until the servos move correctly.
7. Confirm RGB LED data GPIO. Start with GPIO9; if it does not light the LEDs,
   use `self.light.set_gpio` to probe other candidate GPIOs without reflashing.
