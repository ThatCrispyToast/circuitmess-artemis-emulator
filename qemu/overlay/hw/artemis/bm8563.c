/*
 * Belling BM8563 real-time clock (PCF8563 compatible).
 *
 * The battery keeps it running through chip resets and power-off, so time is
 * derived from the host clock and survives everything but an explicit set.
 * Its INT pin isn't connected on the watch; alarm and timer only raise flags.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "hw/artemis/artemis.h"

enum {
    R_CS1 = 0x00,
    R_CS2 = 0x01,
    R_SECONDS = 0x02,
    R_MINUTES = 0x03,
    R_HOURS = 0x04,
    R_DAYS = 0x05,
    R_WEEKDAYS = 0x06,
    R_MONTHS = 0x07,
    R_YEARS = 0x08,
    R_ALARM_MIN = 0x09,
    R_ALARM_HOUR = 0x0A,
    R_ALARM_DAY = 0x0B,
    R_ALARM_WDAY = 0x0C,
    R_CLKOUT = 0x0D,
    R_TIMER_CTRL = 0x0E,
    R_TIMER = 0x0F,
};

#define CS1_STOP      BIT(5)
#define CS2_AF        BIT(3)
#define CS2_TF        BIT(2)
#define ALARM_DISABLE BIT(7)
#define TIMER_ENABLE  BIT(7)
#define MONTH_CENTURY BIT(7)
#define SECONDS_VL    BIT(7)

static inline uint8_t to_bcd(int v)
{
    return (v / 10) << 4 | (v % 10);
}

static inline int from_bcd(uint8_t v)
{
    return (v >> 4) * 10 + (v & 0x0F);
}

static int64_t host_ns(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
}

static int64_t current_epoch(ArtemisBm8563State *s)
{
    if (s->stopped) {
        return s->base_epoch;
    }
    return s->base_epoch + (host_ns() - s->base_host_ns) / NANOSECONDS_PER_SECOND;
}

void artemis_bm8563_set_epoch(ArtemisBm8563State *s, int64_t epoch)
{
    s->base_epoch = epoch;
    s->base_host_ns = host_ns();
}

static void check_alarm(ArtemisBm8563State *s, const struct tm *tm)
{
    uint8_t *r = s->regs;
    bool any = false;
    bool match = true;

    if (!(r[R_ALARM_MIN] & ALARM_DISABLE)) {
        any = true;
        match &= from_bcd(r[R_ALARM_MIN] & 0x7F) == tm->tm_min;
    }
    if (!(r[R_ALARM_HOUR] & ALARM_DISABLE)) {
        any = true;
        match &= from_bcd(r[R_ALARM_HOUR] & 0x3F) == tm->tm_hour;
    }
    if (!(r[R_ALARM_DAY] & ALARM_DISABLE)) {
        any = true;
        match &= from_bcd(r[R_ALARM_DAY] & 0x3F) == tm->tm_mday;
    }
    if (!(r[R_ALARM_WDAY] & ALARM_DISABLE)) {
        any = true;
        match &= (r[R_ALARM_WDAY] & 7) == tm->tm_wday;
    }
    if (any && match) {
        r[R_CS2] |= CS2_AF;
    }
}

static void check_timer(ArtemisBm8563State *s)
{
    if (!(s->regs[R_TIMER_CTRL] & TIMER_ENABLE) || !s->regs[R_TIMER]) {
        return;
    }
    if (s->timer_deadline_ns && host_ns() >= s->timer_deadline_ns) {
        s->regs[R_CS2] |= CS2_TF;
        s->timer_deadline_ns = 0;
    }
}

/* Refreshes the time registers: the chip freezes them for a whole access. */
static void capture(ArtemisBm8563State *s)
{
    time_t t = current_epoch(s);
    struct tm tm;
    uint8_t *r = s->regs;

    gmtime_r(&t, &tm);
    r[R_SECONDS] = to_bcd(tm.tm_sec) | (s->low_voltage ? SECONDS_VL : 0);
    r[R_MINUTES] = to_bcd(tm.tm_min);
    r[R_HOURS] = to_bcd(tm.tm_hour);
    r[R_DAYS] = to_bcd(tm.tm_mday);
    r[R_WEEKDAYS] = tm.tm_wday;
    r[R_MONTHS] = to_bcd(tm.tm_mon + 1) |
                  (tm.tm_year >= 100 ? MONTH_CENTURY : 0);
    r[R_YEARS] = to_bcd(tm.tm_year % 100);

    check_alarm(s, &tm);
    check_timer(s);
}

/* Applies registers written during the access back to the running clock. */
static void commit_time(ArtemisBm8563State *s)
{
    uint8_t *r = s->regs;
    struct tm tm = {
        .tm_sec = from_bcd(r[R_SECONDS] & 0x7F),
        .tm_min = from_bcd(r[R_MINUTES] & 0x7F),
        .tm_hour = from_bcd(r[R_HOURS] & 0x3F),
        .tm_mday = from_bcd(r[R_DAYS] & 0x3F),
        .tm_mon = from_bcd(r[R_MONTHS] & 0x1F) - 1,
        .tm_year = from_bcd(r[R_YEARS]) + ((r[R_MONTHS] & MONTH_CENTURY) ? 100 : 0),
    };

    s->low_voltage = r[R_SECONDS] & SECONDS_VL;
    artemis_bm8563_set_epoch(s, timegm(&tm));
}

static void arm_timer(ArtemisBm8563State *s)
{
    static const int64_t tick_ns[4] = {
        NANOSECONDS_PER_SECOND / 4096,
        NANOSECONDS_PER_SECOND / 64,
        NANOSECONDS_PER_SECOND,
        60 * NANOSECONDS_PER_SECOND,
    };
    uint8_t ctrl = s->regs[R_TIMER_CTRL];

    if (ctrl & TIMER_ENABLE && s->regs[R_TIMER]) {
        s->timer_deadline_ns = host_ns() + s->regs[R_TIMER] * tick_ns[ctrl & 3];
    } else {
        s->timer_deadline_ns = 0;
    }
}

static int bm8563_event(I2CSlave *i2c, enum i2c_event event)
{
    ArtemisBm8563State *s = ARTEMIS_BM8563(i2c);

    switch (event) {
    case I2C_START_RECV:
        capture(s);
        break;
    case I2C_START_SEND:
        s->have_pointer = false;
        capture(s);
        break;
    default:
        break;
    }
    return 0;
}

static uint8_t bm8563_recv(I2CSlave *i2c)
{
    ArtemisBm8563State *s = ARTEMIS_BM8563(i2c);
    uint8_t v = s->regs[s->pointer];

    s->pointer = (s->pointer + 1) & 0x0F;
    return v;
}

static int bm8563_send(I2CSlave *i2c, uint8_t data)
{
    ArtemisBm8563State *s = ARTEMIS_BM8563(i2c);

    if (!s->have_pointer) {
        s->pointer = data & 0x0F;
        s->have_pointer = true;
        return 0;
    }

    uint8_t reg = s->pointer;

    s->regs[reg] = data;
    s->pointer = (reg + 1) & 0x0F;

    if (reg >= R_SECONDS && reg <= R_YEARS) {
        /* Each write lands immediately so partial updates behave too. */
        commit_time(s);
    } else if (reg == R_CS1) {
        s->stopped = data & CS1_STOP;
        if (s->stopped) {
            s->base_epoch = current_epoch(s);
        }
        s->base_host_ns = host_ns();
    } else if (reg == R_TIMER || reg == R_TIMER_CTRL) {
        arm_timer(s);
    }
    return 0;
}

static void bm8563_realize(DeviceState *dev, Error **errp)
{
    ArtemisBm8563State *s = ARTEMIS_BM8563(dev);
    time_t now = time(NULL);
    struct tm local;

    /* The firmware treats the RTC as local time, so seed it with that. */
    localtime_r(&now, &local);
    artemis_bm8563_set_epoch(s, timegm(&local));

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[R_ALARM_MIN] = ALARM_DISABLE;
    s->regs[R_ALARM_HOUR] = ALARM_DISABLE;
    s->regs[R_ALARM_DAY] = ALARM_DISABLE;
    s->regs[R_ALARM_WDAY] = ALARM_DISABLE;
    s->regs[R_CLKOUT] = 0x80;
    s->regs[R_TIMER_CTRL] = 0x03;
}

static void bm8563_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->realize = bm8563_realize;
    k->event = bm8563_event;
    k->recv = bm8563_recv;
    k->send = bm8563_send;
}

static const TypeInfo bm8563_info = {
    .name = TYPE_ARTEMIS_BM8563,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(ArtemisBm8563State),
    .class_init = bm8563_class_init,
};

static void bm8563_register_types(void)
{
    type_register_static(&bm8563_info);
}

type_init(bm8563_register_types)
