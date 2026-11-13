/*
 * CircuitMess Artemis Watch 2.0 peripheral models for the ESP32-S3 machine.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include "hw/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/ssi/ssi.h"
#include "hw/dma/esp_gdma.h"
#include "chardev/char-fe.h"
#include "qemu/fifo8.h"
#include "qemu/timer.h"

/* Nets from the Artemis Watch 2.0 schematic (hardware revisions 2 and 3). */
enum {
    ARTEMIS_PIN_LEVER_RIGHT = 1,  /* firmware "Up" */
    ARTEMIS_PIN_LEVER_LEFT = 2,   /* firmware "Down" */
    ARTEMIS_PIN_LEVER_PUSH = 3,   /* firmware "Select" */
    ARTEMIS_PIN_CALIB_EN = 4,
    ARTEMIS_PIN_BATTERY_READ = 5,
    ARTEMIS_PIN_CHRG_OUT = 6,
    ARTEMIS_PIN_CHRG_IN = 7,
    ARTEMIS_PIN_RGB_G = 12,
    ARTEMIS_PIN_RGB_B = 13,
    ARTEMIS_PIN_RGB_R = 14,
    ARTEMIS_PIN_LED6 = 17,
    ARTEMIS_PIN_LED5 = 18,
    ARTEMIS_PIN_BUTTON = 21,      /* firmware "Alt" / "Back" */
    ARTEMIS_PIN_TFT_BL = 33,
    ARTEMIS_PIN_TFT_RST = 34,
    ARTEMIS_PIN_TFT_SDA = 35,
    ARTEMIS_PIN_TFT_SCK = 36,
    ARTEMIS_PIN_POWEROFF = 37,
    ARTEMIS_PIN_GYRO_INT1 = 38,
    ARTEMIS_PIN_GYRO_INT2 = 39,
    ARTEMIS_PIN_I2C_SDA = 40,
    ARTEMIS_PIN_I2C_SCL = 41,
    ARTEMIS_PIN_USB_DETECT = 42,
    ARTEMIS_PIN_LED4 = 43,
    ARTEMIS_PIN_LED3 = 44,
    ARTEMIS_PIN_LED2 = 45,
    ARTEMIS_PIN_LED1 = 46,
    ARTEMIS_PIN_BUZZER = 47,
    ARTEMIS_PIN_TFT_DC = 48,
};

#define ARTEMIS_GPIO_COUNT 54
#define ARTEMIS_LEDC_CHANNELS 8

#define ARTEMIS_LCD_W 128
#define ARTEMIS_LCD_H 128

/* ------------------------------------------------------------------------ */
/* LEDC                                                                      */

#define TYPE_ARTEMIS_LEDC "artemis.ledc"
OBJECT_DECLARE_SIMPLE_TYPE(ArtemisLedcState, ARTEMIS_LEDC)

typedef void (*ArtemisLedcNotify)(void *opaque);

struct ArtemisLedcState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *fade_timer;

    uint32_t ch_conf0[ARTEMIS_LEDC_CHANNELS];
    uint32_t ch_hpoint[ARTEMIS_LEDC_CHANNELS];
    uint32_t ch_duty[ARTEMIS_LEDC_CHANNELS];     /* shadow, 1/16 steps */
    uint32_t ch_conf1[ARTEMIS_LEDC_CHANNELS];
    uint32_t ch_duty_out[ARTEMIS_LEDC_CHANNELS]; /* latched, 1/16 steps */
    bool ch_fading[ARTEMIS_LEDC_CHANNELS];
    int64_t ch_fade_next[ARTEMIS_LEDC_CHANNELS];
    uint32_t timer_conf[4];
    uint32_t timer_active[4];
    uint32_t int_raw;
    uint32_t int_ena;
    uint32_t conf;

    ArtemisLedcNotify notify;
    void *notify_opaque;
};

/* Duty as a fraction of 65535 and PWM frequency in Hz for a channel output. */
void artemis_ledc_channel_output(ArtemisLedcState *s, int ch,
                                 uint32_t *duty, uint32_t *freq);

/* ------------------------------------------------------------------------ */
/* GPIO matrix, IO_MUX and RTC_IO                                            */

#define TYPE_ARTEMIS_GPIO "artemis.gpio"
OBJECT_DECLARE_SIMPLE_TYPE(ArtemisGpioState, ARTEMIS_GPIO)

#define ARTEMIS_GPIO_EXT_FLOATING (-1)

/* Fired for pins whose visible output changed; level is a fraction of 65535. */
typedef void (*ArtemisPinNotify)(void *opaque, int pin, uint32_t level,
                                 uint32_t freq);
/* Fired when a pad's digital input could have woken the chip from sleep. */
typedef void (*ArtemisWakeNotify)(void *opaque);

struct ArtemisGpioState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion iomux_mem;
    MemoryRegion rtcio_mem;
    qemu_irq irq;

    uint32_t strap_mode;
    uint64_t out;
    uint64_t enable;
    uint64_t status;
    uint32_t bt_select;
    uint32_t sdio_select;
    uint32_t clock_gate;
    uint32_t pin[ARTEMIS_GPIO_COUNT];
    uint32_t func_in_sel[256];
    uint32_t func_out_sel[ARTEMIS_GPIO_COUNT];
    uint32_t iomux[ARTEMIS_GPIO_COUNT];
    uint32_t pin_ctrl;
    uint32_t rtcio[0x100];

    int8_t ext[ARTEMIS_GPIO_COUNT];
    uint64_t level;
    uint32_t published_level[ARTEMIS_GPIO_COUNT];
    uint32_t published_freq[ARTEMIS_GPIO_COUNT];
    bool published_valid[ARTEMIS_GPIO_COUNT];

    qemu_irq output_line[ARTEMIS_GPIO_COUNT];

    ArtemisLedcState *ledc;
    ArtemisPinNotify pin_notify;
    void *pin_notify_opaque;
    ArtemisWakeNotify wake_notify;
    void *wake_notify_opaque;
};

/* Drive a pad from outside the chip; ARTEMIS_GPIO_EXT_FLOATING releases it. */
void artemis_gpio_set_external(ArtemisGpioState *s, int pin, int level);
/* Current logic level seen on a pad. */
bool artemis_gpio_get_level(ArtemisGpioState *s, int pin);
/* Whether the pad's output driver is enabled. */
bool artemis_gpio_is_output(ArtemisGpioState *s, int pin);
/* Recomputes and republishes PWM-driven pads; called when LEDC changes. */
void artemis_gpio_refresh_outputs(ArtemisGpioState *s);
/* Forces every visible pad to be published again, e.g. for a new UI client. */
void artemis_gpio_republish(ArtemisGpioState *s);
/* Whether a GPIO wakeup-enabled pad currently matches its level trigger. */
bool artemis_gpio_wakeup_pending(ArtemisGpioState *s);
/* Whether an RTC pad in `mask` is at the level selected for EXT1 wakeup. */
bool artemis_gpio_ext1_pending(ArtemisGpioState *s, uint32_t mask, bool any_high);

/* ------------------------------------------------------------------------ */
/* General-purpose SPI controller                                            */

#define TYPE_ARTEMIS_GPSPI "artemis.gpspi"
OBJECT_DECLARE_SIMPLE_TYPE(ArtemisGpspiState, ARTEMIS_GPSPI)

struct ArtemisGpspiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    SSIBus *bus;
    ESPGdmaState *gdma;
    QEMUTimer *done_timer;

    uint32_t gdma_periph;
    uint32_t cmd;
    uint32_t addr;
    uint32_t ctrl;
    uint32_t clock;
    uint32_t user;
    uint32_t user1;
    uint32_t user2;
    uint32_t ms_dlen;
    uint32_t misc;
    uint32_t din_mode;
    uint32_t din_num;
    uint32_t dout_mode;
    uint32_t dma_conf;
    uint32_t dma_int_ena;
    uint32_t dma_int_raw;
    uint32_t data[16];
    uint32_t slave;
    uint32_t slave1;
    uint32_t clk_gate;
};

/* ------------------------------------------------------------------------ */
/* I2C controller                                                            */

#define TYPE_ARTEMIS_I2C "artemis.i2c"
OBJECT_DECLARE_SIMPLE_TYPE(ArtemisI2cState, ARTEMIS_I2C)

#define ARTEMIS_I2C_FIFO_LEN 32
#define ARTEMIS_I2C_CMD_COUNT 8

struct ArtemisI2cState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    I2CBus *bus;
    Fifo8 tx_fifo;
    Fifo8 rx_fifo;

    bool started;
    bool address_phase;
    uint32_t regs[0x40];
    uint32_t cmd[ARTEMIS_I2C_CMD_COUNT];
    uint32_t int_raw;
    uint32_t int_ena;
};

/* ------------------------------------------------------------------------ */
/* USB Serial/JTAG                                                           */

#define TYPE_ARTEMIS_USJ "artemis.usb-serial-jtag"
OBJECT_DECLARE_SIMPLE_TYPE(ArtemisUsjState, ARTEMIS_USJ)

#define ARTEMIS_USJ_FIFO_LEN 64

struct ArtemisUsjState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    CharBackend chr;
    Fifo8 rx_fifo;

    uint32_t tx_count;
    uint8_t tx[ARTEMIS_USJ_FIFO_LEN];
    uint32_t int_raw;
    uint32_t int_ena;
    uint32_t conf0;
    uint32_t misc_conf;
    uint32_t mem_conf;
};

/* ------------------------------------------------------------------------ */
/* SAR ADC via the RTC controller                                            */

#define TYPE_ARTEMIS_SARADC "artemis.saradc"
OBJECT_DECLARE_SIMPLE_TYPE(ArtemisSaradcState, ARTEMIS_SARADC)

typedef uint32_t (*ArtemisAdcInput)(void *opaque, int unit, int channel,
                                    int atten);

struct ArtemisSaradcState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[0x100];

    ArtemisAdcInput input;
    void *input_opaque;
};

/* ------------------------------------------------------------------------ */
/* ST7735S panel                                                             */

#define TYPE_ARTEMIS_ST7735S "artemis.st7735s"
OBJECT_DECLARE_SIMPLE_TYPE(ArtemisSt7735sState, ARTEMIS_ST7735S)

#define ARTEMIS_ST7735S_GRAM_W 132
#define ARTEMIS_ST7735S_GRAM_H 162

struct ArtemisSt7735sState {
    SSIPeripheral parent_obj;

    /* Controller wiring differs per panel revision. */
    uint32_t gram_w;
    uint32_t gram_h;
    uint32_t glass_col;
    uint32_t glass_row;

    bool dc;
    bool in_reset;
    bool sleeping;
    bool display_on;
    bool inverted;
    bool idle;
    uint8_t madctl;
    uint8_t colmod;

    uint8_t cmd;
    bool have_cmd;
    uint32_t param_count;
    uint8_t params[16];

    uint16_t x_start, x_end;
    uint16_t y_start, y_end;
    uint16_t x, y;
    bool ram_writing;
    uint8_t pixel_bytes[3];
    uint32_t pixel_fill;

    bool dirty;
    uint16_t *gram;
};

/* Copies the visible 128x128 glass (RGB565, native endian, upright). */
void artemis_st7735s_snapshot(ArtemisSt7735sState *s, uint16_t *out);
bool artemis_st7735s_take_dirty(ArtemisSt7735sState *s);
void artemis_st7735s_power_cycle(ArtemisSt7735sState *s);

/* ------------------------------------------------------------------------ */
/* LSM6DS3TR-C IMU                                                           */

#define TYPE_ARTEMIS_LSM6DS3TRC "artemis.lsm6ds3trc"
OBJECT_DECLARE_SIMPLE_TYPE(ArtemisLsm6State, ARTEMIS_LSM6DS3TRC)

#define ARTEMIS_LSM6_FIFO_WORDS 2048

struct ArtemisLsm6State {
    I2CSlave parent_obj;

    qemu_irq int1;
    qemu_irq int2;
    QEMUTimer *timer;

    uint8_t bank_user[256];
    uint8_t bank_a[256];
    uint8_t bank_b[256];
    uint8_t pointer;
    bool have_pointer;
    uint8_t latch[16];

    /* Physical state set from outside, in SI-free watch units. */
    float accel_mg[3];
    float gyro_mdps[3];
    float temp_c;

    int64_t last_tick_ns;
    int64_t boot_ns;
    int64_t xl_ready_ns;
    int64_t g_ready_ns;
    int64_t t_ready_ns;
    uint32_t tilt_hold_ms[6];
    bool tilt_armed[6];
    uint32_t step_count;

    int16_t fifo[ARTEMIS_LSM6_FIFO_WORDS];
    uint32_t fifo_head;
    uint32_t fifo_len;
    int64_t fifo_last_ns;
};

void artemis_lsm6_set_motion(ArtemisLsm6State *s, const float accel_mg[3],
                             const float gyro_mdps[3], float temp_c);
void artemis_lsm6_inject_tap(ArtemisLsm6State *s, int axis, bool double_tap);
void artemis_lsm6_inject_step(ArtemisLsm6State *s, uint32_t steps);
void artemis_lsm6_power_cycle(ArtemisLsm6State *s);

/* ------------------------------------------------------------------------ */
/* BM8563 real-time clock                                                    */

#define TYPE_ARTEMIS_BM8563 "artemis.bm8563"
OBJECT_DECLARE_SIMPLE_TYPE(ArtemisBm8563State, ARTEMIS_BM8563)

struct ArtemisBm8563State {
    I2CSlave parent_obj;

    uint8_t regs[16];
    uint8_t pointer;
    bool have_pointer;

    /* Seconds since the epoch at host-clock instant base_host_ns. */
    int64_t base_epoch;
    int64_t base_host_ns;
    bool stopped;
    bool low_voltage;
    int64_t timer_deadline_ns;
};

void artemis_bm8563_set_epoch(ArtemisBm8563State *s, int64_t epoch);

/* ------------------------------------------------------------------------ */
/* Radio register blocks                                                     */

#define TYPE_ARTEMIS_RADIO "artemis.radio"
OBJECT_DECLARE_SIMPLE_TYPE(ArtemisRadioState, ARTEMIS_RADIO)

void artemis_radio_map(ArtemisRadioState *s, MemoryRegion *sys_mem);

#define TYPE_ARTEMIS_BLE "artemis.ble"
OBJECT_DECLARE_SIMPLE_TYPE(ArtemisBleState, ARTEMIS_BLE)

/* ------------------------------------------------------------------------ */
/* Board: power, battery, and the bridge to the emulator's front end          */

#define TYPE_ARTEMIS_BOARD "artemis.board"
OBJECT_DECLARE_SIMPLE_TYPE(ArtemisBoardState, ARTEMIS_BOARD)

struct ArtemisBoardState {
    SysBusDevice parent_obj;

    CharBackend chr;
    QEMUTimer *frame_timer;
    QEMUBH *flush_bh;
    qemu_irq power_line;

    ArtemisGpioState *gpio;
    ArtemisSt7735sState *panel;
    ArtemisLsm6State *imu;
    ArtemisBm8563State *rtc;

    uint32_t revision;

    /* Bridge wire state. */
    uint8_t rx_buf[256];
    uint32_t rx_len;
    GByteArray *tx;
    guint tx_watch;
    bool client_connected;
    uint16_t *frame;
    uint32_t frame_seq;
    bool frame_force;

    /* Electrical state. */
    uint32_t battery_mv;
    bool usb_plugged;
    bool charging;
    bool powered;
};

/* Wire the board's nets once the devices it glues together are realized. */
void artemis_board_wire(ArtemisBoardState *s, ArtemisGpioState *gpio,
                        ArtemisSaradcState *adc, ArtemisSt7735sState *panel,
                        ArtemisLsm6State *imu, ArtemisBm8563State *rtc);
