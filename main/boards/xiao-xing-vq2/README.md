# Xiao Xing VQ2

Board definition for the Xiao Xing VQ2 ESP32-S3-N16R8 robot board.

Confirmed hardware:

- ESP32-S3-N16R8 module, expected 16 MB flash and 8 MB PSRAM.
- SH1106 128x64 I2C OLED, SDA GPIO41, SCL GPIO42.
- I2S speaker: BCLK GPIO15, LRCK GPIO16, DOUT GPIO7.
- I2S microphone: SCK GPIO5, WS GPIO4, DIN GPIO6.
- Four populated servo outputs: FL GPIO17, FR GPIO13, BL GPIO18, BR GPIO14.
- Optional factory tail servo slot on GPIO12 is not populated on the tested unit.
- Two lower WS2812 RGB LEDs on GPIO8.

The runtime implementation mirrors the ESP-HI dog and light controls while
keeping the VQ2 display and I2S audio wiring.
