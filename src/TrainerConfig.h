#pragma once

/*
 *   Board and build configuration for the DTT-Trainer on the LilyGo T-RGB
 *   2.1 inch round board (ESP32-S3R8, 16 MB flash, 8 MB OPI PSRAM).
 *
 *   Pin notes that matter:
 *     - The T-RGB has no free GPIO. The I2C bus (SDA 8, SCL 48) is shared by the
 *       touch controller, the XL9535 expander and the exposed I2C connector, so
 *       the VL6180 sensor goes on that same bus. There is no second bus.
 *     - The panel init runs as 3-wire SPI through the XL9535 expander, not from
 *       ESP32 pins: the ST7701S needs its SPI before the RGB bus carries pixels.
 *     - GPIO46 is the backlight, GPIO1 is the touch interrupt, GPIO4 is the
 *       battery sense input (100k/100k divider, so the pin sees half the cell).
 *     - The RGB data pins below are the 2.1 inch (full circle) mapping. A 2.8
 *       inch board uses a different init sequence; see TRAINER_PANEL_2_8_INCH.
 */

/* ------------------------------------------------------------------ panel -- */

#define TRAINER_TFT_WIDTH 480
#define TRAINER_TFT_HEIGHT 480

#define TRAINER_TFT_BL 46   /* backlight, PWM capable */
#define TRAINER_TFT_DE 45
#define TRAINER_TFT_VSYNC 41
#define TRAINER_TFT_HSYNC 47
#define TRAINER_TFT_PCLK 42

/* Blue DB0..DB4 (B0..B4) */
#define TRAINER_TFT_B0 7
#define TRAINER_TFT_B1 6
#define TRAINER_TFT_B2 5
#define TRAINER_TFT_B3 3
#define TRAINER_TFT_B4 2
/* Green DB5..DB10 (G0..G5) */
#define TRAINER_TFT_G0 14
#define TRAINER_TFT_G1 13
#define TRAINER_TFT_G2 12
#define TRAINER_TFT_G3 11
#define TRAINER_TFT_G4 10
#define TRAINER_TFT_G5 9
/* Red DB11..DB15 (R0..R4) */
#define TRAINER_TFT_R0 21
#define TRAINER_TFT_R1 18
#define TRAINER_TFT_R2 17
#define TRAINER_TFT_R3 16
#define TRAINER_TFT_R4 15

#define TRAINER_TFT_HSYNC_POLARITY 1
#define TRAINER_TFT_HSYNC_FRONT_PORCH 50
#define TRAINER_TFT_HSYNC_PULSE_WIDTH 1
#define TRAINER_TFT_HSYNC_BACK_PORCH 30
#define TRAINER_TFT_VSYNC_POLARITY 1
#define TRAINER_TFT_VSYNC_FRONT_PORCH 20
#define TRAINER_TFT_VSYNC_PULSE_WIDTH 1
#define TRAINER_TFT_VSYNC_BACK_PORCH 30
#define TRAINER_TFT_PCLK_ACTIVE_NEG 1

/* Pixel clock for the panel. The LilyGo example uses 8 MHz, and a slower scanout
 * also keeps the PSRAM read pressure down. */
#define TRAINER_TFT_PCLK_HZ 8000000

/* The panel has exactly one framebuffer and it lives in PSRAM, where the RGB DMA
 * reads it out continuously. Arduino_GFX configures no bounce buffer by default,
 * which means every drawn region is copied into that same memory while it is on
 * screen: that is the tearing this board shows. The driver's bounce buffer makes
 * it copy during the vertical blanking instead, through a small buffer in internal
 * RAM. The size must be a multiple of the panel width; 20 lines is 19 kB and it
 * removes the visible tearing. Set to 0 to draw straight into PSRAM again. */
#define TRAINER_TFT_BOUNCE_LINES 20

/* Backlight PWM. A low PWM frequency can be visible as flicker through the
 * switching converter that drives the LEDs, so this is well above what the eye
 * follows, with 10 bit resolution for smooth dimming. */
#define TRAINER_TFT_BL_PWM_HZ 20000
#define TRAINER_TFT_BL_PWM_BITS 10

/* Uncomment for the 2.8 inch T-RGB: only the panel init sequence differs. */
/* #define TRAINER_PANEL_2_8_INCH 1 */

/* -------------------------------------------------------------------- I2C -- */

#define TRAINER_I2C_SDA 8
#define TRAINER_I2C_SCL 48
#define TRAINER_I2C_SPEED 400000

/* XL9535 I2C expander, 16 bit, drives the panel init lines */
#define TRAINER_XL9535_ADDR 0x20
#define TRAINER_XL9535_PIN_POWER_EN 2
#define TRAINER_XL9535_PIN_CS 3
#define TRAINER_XL9535_PIN_MOSI 4
#define TRAINER_XL9535_PIN_SCLK 5
#define TRAINER_XL9535_PIN_LCD_RST 6
#define TRAINER_XL9535_PIN_TOUCH_RST 1

/* ------------------------------------------------------------------ touch -- */

#define TRAINER_TOUCH_ADDR 0x15 /* CST820 / CST816 family; 0x38 is FT3267 */
#define TRAINER_TOUCH_IRQ 1
#define TRAINER_TOUCH_TAP_MS 600      /* shorter press: a tap */
#define TRAINER_TOUCH_LONGPRESS_MS 1500

/* While the controller answers, its registers are sampled at this rate. The
 * CST820 does not hold its interrupt line low for the length of a touch (LilyGo's
 * own driver reads the registers continuously for that reason), so a tap is not
 * visible on the interrupt line alone. */
#define TRAINER_TOUCH_POLL_MS 50

/* Failed reads in a row before the controller counts as asleep. One NACK is a
 * hiccup on a bus that the sensor shares. */
#define TRAINER_TOUCH_FAIL_LIMIT 4
/* A controller that went to sleep wakes on a touch, and a reset through the
 * expander wakes it as well. This is the fallback interval for a board whose
 * interrupt line is not connected; the expander answers, so it costs no NACK. */
#define TRAINER_TOUCH_ASLEEP_PROBE_MS 60000
#define TRAINER_TOUCH_ASLEEP_BACKOFF_MS 60000

/* ---------------------------------------------------------------- battery -- */

/* 100k/100k divider on the board, so the ADC sees half the cell voltage. */
#define TRAINER_BAT_ADC_PIN 4
#define TRAINER_BAT_DIVIDER 2.0f

/* ----------------------------------------------------------------- sensor -- */

#define TRAINER_SENSOR_ADDR 0x29

/* ---------------------------------------------------------------- samples -- */

/* One task owns every I2C access (sensor and touch), because the Arduino Wire
 * library is not thread safe and all three devices share one bus. */
#define TRAINER_SENSOR_TASK_HZ 25
#define TRAINER_DISPLAY_TASK_HZ 10
#define TRAINER_TOUCH_EVERY_N 2 /* touch is polled every second sensor tick */

/* Longest interval the session integration accepts in one step. A blocked task
 * must not be able to add a minute of depth time in one go. */
#define TRAINER_MAX_STEP_SECONDS 0.5f

/* -------------------------------------------------------------------- WiFi -- */

/* The country the device is operated in. It decides which channels and which
 * transmit power are legal, and an ESP32 that never got a real country code runs
 * as "01", the worldwide safe set, which is the lowest common denominator of
 * every regulation. Change it to your own country if you are not in Germany. */
#ifndef TRAINER_WIFI_COUNTRY
#define TRAINER_WIFI_COUNTRY "DE"
#endif

/* 19.5 dBm in quarter-dBm units, the ceiling of the ESP32-S3 radio. The IDF
 * accepts 8 .. 84, which is 2 .. 21 dBm. */
#define TRAINER_WIFI_TX_POWER_QUARTER_DBM 78

/* How long after boot the radio report is written to the serial port. It scans
 * when the device is not connected, which costs the radio about a second, so it
 * happens once and only once. */
#define TRAINER_WIFI_REPORT_MS 12000

/* A depth reading is a measurement, not a state. When nothing is in front of the
 * sensor, the last value is not the truth, so the readout falls back to 0 after
 * this long instead of holding a number from a minute ago. */
#define TRAINER_DEPTH_HOLD_MS 2000

/* One status line every minute, so a serial terminal that was opened late still
 * shows what the board is doing. */
#define TRAINER_STATUS_INTERVAL_MS 60000
