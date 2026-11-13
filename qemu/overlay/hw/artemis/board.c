/*
 * Artemis Watch 2.0 board glue: power logic, battery sense and the bridge that
 * connects the emulated hardware to the front end.
 *
 * Bridge wire format, over a stream chardev, all little-endian:
 *   u8 type, u24 payload length, payload.
 *
 * From the board:
 *   BRIDGE_HELLO  u16 version, u8 revision, u8 lcd width, u8 lcd height
 *   BRIDGE_FRAME  u32 sequence, width*height RGB565 pixels
 *   BRIDGE_PIN    u8 pin, u16 level (of 65535), u32 frequency in Hz
 *   BRIDGE_POWER  u8 on
 *   BRIDGE_SYNCED u32 token, echoing BRIDGE_SYNC
 *
 * To the board:
 *   BRIDGE_SET_PIN      u8 pin, i8 level (-1 releases the pad)
 *   BRIDGE_SET_IMU      f32 accel_mg[3], f32 gyro_mdps[3], f32 temp_c
 *   BRIDGE_SET_BATTERY  u16 mv, u8 usb, u8 charging
 *   BRIDGE_SET_RTC      i64 seconds since the epoch, as wall-clock time; the
 *                       lowest value means the clock was never set
 *   BRIDGE_RESET
 *   BRIDGE_REQ_FRAME
 *   BRIDGE_TAP          u8 axis, u8 double
 *   BRIDGE_STEPS        u16 count
 *   BRIDGE_SYNC         u32 token: everything before this has been applied, and
 *                       everything reported until now precedes the answer
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "sysemu/runstate.h"
#include "hw/artemis/artemis.h"
#ifdef __linux__
#include <sys/prctl.h>
#endif

#define BRIDGE_VERSION 2

enum {
    BRIDGE_HELLO = 0x01,
    BRIDGE_FRAME = 0x02,
    BRIDGE_PIN = 0x03,
    BRIDGE_POWER = 0x04,
    BRIDGE_SYNCED = 0x05,

    BRIDGE_SET_PIN = 0x81,
    BRIDGE_SET_IMU = 0x82,
    BRIDGE_SET_BATTERY = 0x83,
    BRIDGE_SET_RTC = 0x84,
    BRIDGE_RESET = 0x85,
    BRIDGE_REQ_FRAME = 0x86,
    BRIDGE_TAP = 0x87,
    BRIDGE_STEPS = 0x88,
    BRIDGE_SYNC = 0x89,
};

#define FRAME_BYTES (ARTEMIS_LCD_W * ARTEMIS_LCD_H * 2)
#define FRAME_INTERVAL_MS 16
/* Past this the front end isn't keeping up, so stop queueing frames for it. */
#define TX_BACKLOG_LIMIT (FRAME_BYTES * 2)

/*
 * BatteryV3 in the firmware reads the battery through a 1:4 divider with a
 * fixed 75 mV correction; CALIB_EN switches the divider to a 2.5 V reference.
 * With a cable in, the divider sits on the USB rail behind a Schottky diode
 * instead of on the battery: this is what a watch measured there.
 */
#define BATT_DIVIDER 4
#define BATT_OFFSET_MV 75
#define VREF_MV 2500
#define USB_RAIL_MV 4370
#define PAD_HIGH_MV 3300

/*
 * Code each attenuation produces at 850 mV, from a watch's calibration eFuses;
 * artemis_emu/efuse.py stores the same figures.
 */
static const uint32_t adc_digi_at_850mv[4] = { 3659, 2742, 1923, 1017 };

/* ESP-IDF's curve-fitting correction for ADC1, so calibrated reads are exact. */
static const double adc_error_coef[4][5] = {
    { -2.7856531419538344, -0.0050871540569528, 9.798249589e-6, 0, 0 },
    { -2.9831022915028695, -0.0049393185868806, 1.01379430548e-5, 0, 0 },
    { -2.3285545746296417, -0.0147640181047414, 2.08385525314e-5, 0, 0 },
    { -0.644403418269478, -0.0644334888647536, 1.297891447611e-4,
      -7.0769718e-8, 1.3515e-11 },
};

static double adc_error_mv(int atten, double v)
{
    const double *k = adc_error_coef[atten];

    return k[0] + v * (k[1] + v * (k[2] + v * (k[3] + v * k[4])));
}

static uint32_t mv_to_adc_code(int atten, double mv)
{
    /* The driver computes mv = v - error(v); invert it by iterating. */
    double v = mv;

    for (int i = 0; i < 4; i++) {
        v = mv + adc_error_mv(atten, v);
    }

    double code = v * adc_digi_at_850mv[atten] / 850.0;

    return (uint32_t)CLAMP(code + 0.5, 0, 4095);
}

static uint32_t adc_input(void *opaque, int unit, int channel, int atten)
{
    ArtemisBoardState *s = ARTEMIS_BOARD(opaque);
    double mv = 0;

    /* ADC1 channel n is GPIO n + 1, ADC2 channel n is GPIO n + 11. */
    int pin = unit == 1 ? channel + 1 : channel + 11;

    if (pin == ARTEMIS_PIN_BATTERY_READ) {
        if (artemis_gpio_get_level(s->gpio, ARTEMIS_PIN_CALIB_EN)) {
            mv = (double)VREF_MV / BATT_DIVIDER;
        } else {
            double sense = (double)s->battery_mv - BATT_OFFSET_MV;

            if (s->usb_plugged) {
                sense = MAX(sense, USB_RAIL_MV);
            }
            mv = sense / BATT_DIVIDER;
        }
    } else if (pin < ARTEMIS_GPIO_COUNT && artemis_gpio_get_level(s->gpio, pin)) {
        /* No analogue source on the other pads: they're at a logic level. */
        mv = PAD_HIGH_MV;
    }
    return mv_to_adc_code(atten, mv);
}

/* ------------------------------------------------------------------------ */

static void flush_tx(ArtemisBoardState *s);

static gboolean tx_writable(void *do_not_use, GIOCondition cond, void *opaque)
{
    ArtemisBoardState *s = ARTEMIS_BOARD(opaque);

    s->tx_watch = 0;
    flush_tx(s);
    return G_SOURCE_REMOVE;
}

static void flush_tx(ArtemisBoardState *s)
{
    if (!s->client_connected) {
        g_byte_array_set_size(s->tx, 0);
        return;
    }

    while (s->tx->len) {
        int n = qemu_chr_fe_write(&s->chr, s->tx->data, s->tx->len);

        if (n <= 0) {
            if (!s->tx_watch) {
                s->tx_watch = qemu_chr_fe_add_watch(&s->chr,
                                                    G_IO_OUT | G_IO_HUP,
                                                    tx_writable, s);
            }
            return;
        }
        g_byte_array_remove_range(s->tx, 0, n);
    }
}

static void flush_bh(void *opaque)
{
    flush_tx(ARTEMIS_BOARD(opaque));
}

/* Safe from vCPU context: only queues, the main loop does the writing. */
static void send_msg(ArtemisBoardState *s, uint8_t type, const void *a,
                     uint32_t a_len, const void *b, uint32_t b_len)
{
    uint32_t len = a_len + b_len;
    uint8_t header[4] = { type, len, len >> 8, len >> 16 };

    if (!s->client_connected) {
        return;
    }
    g_byte_array_append(s->tx, header, sizeof(header));
    if (a_len) {
        g_byte_array_append(s->tx, a, a_len);
    }
    if (b_len) {
        g_byte_array_append(s->tx, b, b_len);
    }
    qemu_bh_schedule(s->flush_bh);
}

static void send_power(ArtemisBoardState *s)
{
    uint8_t on = s->powered;

    send_msg(s, BRIDGE_POWER, &on, 1, NULL, 0);
}

static void send_frame(ArtemisBoardState *s)
{
    uint8_t seq[4];

    if (s->powered) {
        artemis_st7735s_snapshot(s->panel, s->frame);
    } else {
        memset(s->frame, 0, FRAME_BYTES);
    }
    stl_le_p(seq, ++s->frame_seq);
#if HOST_BIG_ENDIAN
    for (int i = 0; i < ARTEMIS_LCD_W * ARTEMIS_LCD_H; i++) {
        s->frame[i] = bswap16(s->frame[i]);
    }
#endif
    send_msg(s, BRIDGE_FRAME, seq, sizeof(seq), s->frame, FRAME_BYTES);
}

static void frame_tick(void *opaque)
{
    ArtemisBoardState *s = ARTEMIS_BOARD(opaque);

    if (s->client_connected && s->tx->len < TX_BACKLOG_LIMIT) {
        if (artemis_st7735s_take_dirty(s->panel) || s->frame_force) {
            s->frame_force = false;
            send_frame(s);
        }
    }
    timer_mod(s->frame_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                              FRAME_INTERVAL_MS);
}

static void pin_changed(void *opaque, int pin, uint32_t level, uint32_t freq)
{
    ArtemisBoardState *s = ARTEMIS_BOARD(opaque);
    uint8_t msg[7];

    msg[0] = pin;
    stw_le_p(msg + 1, level);
    stl_le_p(msg + 3, freq);
    send_msg(s, BRIDGE_PIN, msg, sizeof(msg), NULL, 0);
}

/* ------------------------------------------------------------------------ */
/* Power                                                                     */

static void power_on(ArtemisBoardState *s)
{
    if (s->powered) {
        return;
    }
    s->powered = true;
    qemu_set_irq(s->power_line, 1);
    s->frame_force = true;
    send_power(s);
}

static void power_off(ArtemisBoardState *s)
{
    if (!s->powered) {
        return;
    }
    s->powered = false;
    qemu_set_irq(s->power_line, 0);
    artemis_st7735s_power_cycle(s->panel);
    artemis_lsm6_power_cycle(s->imu);
    s->frame_force = true;
    send_power(s);
}

/*
 * The LDO stays on until firmware pulls POWEROFF low, and external 5 V holds
 * its enable high regardless. Either the button or USB turns it back on.
 */
static void evaluate_power(ArtemisBoardState *s)
{
    bool pulled_low = artemis_gpio_is_output(s->gpio, ARTEMIS_PIN_POWEROFF) &&
                      !artemis_gpio_get_level(s->gpio, ARTEMIS_PIN_POWEROFF);

    if (pulled_low && !s->usb_plugged) {
        power_off(s);
    }
}

static void poweroff_pad(void *opaque, int n, int level)
{
    evaluate_power(ARTEMIS_BOARD(opaque));
}

static void imu_interrupt(void *opaque, int n, int level)
{
    ArtemisBoardState *s = ARTEMIS_BOARD(opaque);

    artemis_gpio_set_external(s->gpio, n ? ARTEMIS_PIN_GYRO_INT2
                                         : ARTEMIS_PIN_GYRO_INT1, level);
}

/*
 * The charger's status pin sinks hard while charging, about 20 uA once the
 * battery is full, and floats without a supply. The watch pulls that net up
 * through 300 k and ties it to CHRG_OUT through 2.55 k, so firmware tells
 * "full" from "charging" by driving CHRG_OUT high and reading CHRG_IN.
 */
static void update_charger_nets(void *opaque)
{
    ArtemisBoardState *s = ARTEMIS_BOARD(opaque);
    bool driven = artemis_gpio_is_output(s->gpio, ARTEMIS_PIN_CHRG_OUT);
    bool driven_high = driven &&
                       artemis_gpio_get_level(s->gpio, ARTEMIS_PIN_CHRG_OUT);
    bool level;

    if (s->usb_plugged && s->charging) {
        level = false;
    } else if (s->usb_plugged) {
        level = driven_high;
    } else {
        level = !driven || driven_high;
    }

    artemis_gpio_set_external(s->gpio, ARTEMIS_PIN_CHRG_IN, level);
    artemis_gpio_set_external(s->gpio, ARTEMIS_PIN_CHRG_OUT,
                              driven ? ARTEMIS_GPIO_EXT_FLOATING : level);
}

static void apply_battery(ArtemisBoardState *s)
{
    artemis_gpio_set_external(s->gpio, ARTEMIS_PIN_USB_DETECT, s->usb_plugged);
    update_charger_nets(s);
    if (s->usb_plugged) {
        power_on(s);
    } else {
        evaluate_power(s);
    }
}

/* ------------------------------------------------------------------------ */
/* Commands from the front end                                               */

static void handle_message(ArtemisBoardState *s, uint8_t type,
                           const uint8_t *p, uint32_t len)
{
    switch (type) {
    case BRIDGE_SET_PIN:
        if (len >= 2 && p[0] < ARTEMIS_GPIO_COUNT) {
            int level = (int8_t)p[1];

            artemis_gpio_set_external(s->gpio, p[0], level);
            if (p[0] == ARTEMIS_PIN_BUTTON && level == 1) {
                power_on(s);
            }
        }
        break;

    case BRIDGE_SET_IMU:
        if (len >= 28) {
            float accel[3], gyro[3], temp;

            memcpy(accel, p, sizeof(accel));
            memcpy(gyro, p + 12, sizeof(gyro));
            memcpy(&temp, p + 24, sizeof(temp));
            artemis_lsm6_set_motion(s->imu, accel, gyro, temp);
        }
        break;

    case BRIDGE_SET_BATTERY:
        if (len >= 4) {
            s->battery_mv = lduw_le_p(p);
            s->usb_plugged = p[2];
            s->charging = p[3];
            apply_battery(s);
        }
        break;

    case BRIDGE_SET_RTC:
        if (len >= 8) {
            int64_t epoch = ldq_le_p(p);

            if (epoch == INT64_MIN) {
                artemis_bm8563_power_loss(s->rtc);
            } else {
                artemis_bm8563_set_epoch(s->rtc, epoch);
            }
        }
        break;

    case BRIDGE_RESET:
        /* The same as cutting power, since the EN line isn't exposed. */
        s->powered = false;
        artemis_st7735s_power_cycle(s->panel);
        artemis_lsm6_power_cycle(s->imu);
        power_on(s);
        break;

    case BRIDGE_REQ_FRAME:
        s->frame_force = true;
        break;

    case BRIDGE_TAP:
        if (len >= 2) {
            artemis_lsm6_inject_tap(s->imu, p[0], p[1]);
        }
        break;

    case BRIDGE_STEPS:
        if (len >= 2) {
            artemis_lsm6_inject_step(s->imu, lduw_le_p(p));
        }
        break;

    case BRIDGE_SYNC:
        send_msg(s, BRIDGE_SYNCED, p, len, NULL, 0);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "artemis.board: unknown message %02x\n",
                      type);
        break;
    }
}

static int chr_can_receive(void *opaque)
{
    ArtemisBoardState *s = ARTEMIS_BOARD(opaque);

    return sizeof(s->rx_buf) - s->rx_len;
}

static void chr_receive(void *opaque, const uint8_t *buf, int size)
{
    ArtemisBoardState *s = ARTEMIS_BOARD(opaque);

    memcpy(s->rx_buf + s->rx_len, buf, size);
    s->rx_len += size;

    uint32_t pos = 0;

    while (s->rx_len - pos >= 4) {
        uint32_t len = s->rx_buf[pos + 1] | s->rx_buf[pos + 2] << 8 |
                       s->rx_buf[pos + 3] << 16;

        if (4 + len > sizeof(s->rx_buf)) {
            /* Nothing we accept is this long: the stream can't be trusted. */
            pos = s->rx_len;
            break;
        }
        if (s->rx_len - pos < 4 + len) {
            break;
        }
        handle_message(s, s->rx_buf[pos], s->rx_buf + pos + 4, len);
        pos += 4 + len;
    }

    memmove(s->rx_buf, s->rx_buf + pos, s->rx_len - pos);
    s->rx_len -= pos;
}

static void chr_event(void *opaque, QEMUChrEvent event)
{
    ArtemisBoardState *s = ARTEMIS_BOARD(opaque);

    switch (event) {
    case CHR_EVENT_OPENED: {
        uint8_t hello[5];

        s->client_connected = true;
        s->rx_len = 0;
        g_byte_array_set_size(s->tx, 0);

        stw_le_p(hello, BRIDGE_VERSION);
        hello[2] = s->revision;
        hello[3] = ARTEMIS_LCD_W;
        hello[4] = ARTEMIS_LCD_H;
        send_msg(s, BRIDGE_HELLO, hello, sizeof(hello), NULL, 0);
        send_power(s);
        artemis_gpio_republish(s->gpio);
        s->frame_force = true;
        break;
    }
    case CHR_EVENT_CLOSED:
        /* The front end owns this process: nothing can use it once that's gone. */
        s->client_connected = false;
        g_byte_array_set_size(s->tx, 0);
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_SIGNAL);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------ */

void artemis_board_wire(ArtemisBoardState *s, ArtemisGpioState *gpio,
                        ArtemisSaradcState *adc, ArtemisSt7735sState *panel,
                        ArtemisLsm6State *imu, ArtemisBm8563State *rtc)
{
    DeviceState *dev = DEVICE(s);

    s->gpio = gpio;
    s->panel = panel;
    s->imu = imu;
    s->rtc = rtc;

    adc->input = adc_input;
    adc->input_opaque = s;
    gpio->pin_notify = pin_changed;
    gpio->pin_notify_opaque = s;
    gpio->drive_notify = update_charger_nets;
    gpio->drive_notify_opaque = s;

    qdev_connect_gpio_out_named(DEVICE(gpio), "pad", ARTEMIS_PIN_TFT_DC,
                                qdev_get_gpio_in_named(DEVICE(panel), "dc", 0));
    qdev_connect_gpio_out_named(DEVICE(gpio), "pad", ARTEMIS_PIN_TFT_RST,
                                qdev_get_gpio_in_named(DEVICE(panel), "reset", 0));
    qdev_connect_gpio_out_named(DEVICE(gpio), "pad", ARTEMIS_PIN_POWEROFF,
                                qdev_get_gpio_in_named(dev, "poweroff", 0));
    qdev_connect_gpio_out_named(DEVICE(imu), "int1", 0,
                                qdev_get_gpio_in_named(dev, "imu-int", 0));
    qdev_connect_gpio_out_named(DEVICE(imu), "int2", 0,
                                qdev_get_gpio_in_named(dev, "imu-int", 1));

    apply_battery(s);
}

static void board_realize(DeviceState *dev, Error **errp)
{
    ArtemisBoardState *s = ARTEMIS_BOARD(dev);

#ifdef __linux__
    /* Timers are serviced from this thread: don't let the kernel round them up. */
    prctl(PR_SET_TIMERSLACK, 1);
#endif

    s->tx = g_byte_array_new();
    s->frame = g_malloc0(FRAME_BYTES);
    s->flush_bh = qemu_bh_new(flush_bh, s);
    s->powered = true;

    qemu_chr_fe_set_handlers(&s->chr, chr_can_receive, chr_receive, chr_event,
                             NULL, s, NULL, true);

    s->frame_timer = timer_new_ms(QEMU_CLOCK_REALTIME, frame_tick, s);
    timer_mod(s->frame_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                              FRAME_INTERVAL_MS);
}

static void board_init(Object *obj)
{
    ArtemisBoardState *s = ARTEMIS_BOARD(obj);
    DeviceState *dev = DEVICE(obj);

    qdev_init_gpio_in_named(dev, poweroff_pad, "poweroff", 1);
    qdev_init_gpio_in_named(dev, imu_interrupt, "imu-int", 2);
    qdev_init_gpio_out_named(dev, &s->power_line, "power", 1);
}

static Property board_properties[] = {
    DEFINE_PROP_CHR("chardev", ArtemisBoardState, chr),
    DEFINE_PROP_UINT32("revision", ArtemisBoardState, revision, 3),
    DEFINE_PROP_UINT32("battery-mv", ArtemisBoardState, battery_mv, 3900),
    DEFINE_PROP_BOOL("usb", ArtemisBoardState, usb_plugged, false),
    DEFINE_PROP_BOOL("charging", ArtemisBoardState, charging, false),
    /* Average CPU cycles per instruction, in thousandths; 0 runs unpaced. */
    DEFINE_PROP_UINT32("cycles-per-insn-milli", ArtemisBoardState,
                       cycles_per_insn_milli, ARTEMIS_CYCLES_PER_INSN_MILLI),
    DEFINE_PROP_END_OF_LIST(),
};

static void board_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = board_realize;
    device_class_set_props(dc, board_properties);
}

static const TypeInfo board_info = {
    .name = TYPE_ARTEMIS_BOARD,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ArtemisBoardState),
    .instance_init = board_init,
    .class_init = board_class_init,
};

static void board_register_types(void)
{
    type_register_static(&board_info);
}

type_init(board_register_types)
