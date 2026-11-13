/*
 * ST LSM6DS3TR-C accelerometer and gyroscope, I2C interface.
 *
 * Output registers come from a physical state the front end sets. Modelled
 * beyond plain sampling: register banks, data-ready flags and interrupts, the
 * FIFO in continuous/FIFO modes, wrist tilt (which wakes the stock firmware),
 * injected tap events, the step counter and the timestamp counter. Free-fall,
 * wake-up and 6D detection flags always read as inactive.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/artemis/artemis.h"

enum {
    R_FUNC_CFG_ACCESS = 0x01,
    R_FIFO_CTRL1 = 0x06,
    R_FIFO_CTRL2 = 0x07,
    R_FIFO_CTRL3 = 0x08,
    R_FIFO_CTRL5 = 0x0A,
    R_DRDY_PULSE_CFG_G = 0x0B,
    R_INT1_CTRL = 0x0D,
    R_INT2_CTRL = 0x0E,
    R_WHO_AM_I = 0x0F,
    R_CTRL1_XL = 0x10,
    R_CTRL2_G = 0x11,
    R_CTRL3_C = 0x12,
    R_CTRL10_C = 0x19,
    R_WAKE_UP_SRC = 0x1B,
    R_TAP_SRC = 0x1C,
    R_D6D_SRC = 0x1D,
    R_STATUS_REG = 0x1E,
    R_OUT_TEMP_L = 0x20,
    R_OUTX_L_G = 0x22,
    R_OUTX_L_XL = 0x28,
    R_OUTZ_H_XL = 0x2D,
    R_FIFO_STATUS1 = 0x3A,
    R_FIFO_STATUS2 = 0x3B,
    R_FIFO_STATUS3 = 0x3C,
    R_FIFO_STATUS4 = 0x3D,
    R_FIFO_DATA_OUT_L = 0x3E,
    R_FIFO_DATA_OUT_H = 0x3F,
    R_TIMESTAMP0 = 0x40,
    R_TIMESTAMP1 = 0x41,
    R_TIMESTAMP2 = 0x42,
    R_STEP_TIMESTAMP_L = 0x49,
    R_STEP_TIMESTAMP_H = 0x4A,
    R_STEP_COUNTER_L = 0x4B,
    R_STEP_COUNTER_H = 0x4C,
    R_FUNC_SRC1 = 0x53,
    R_FUNC_SRC2 = 0x54,
    R_WRIST_TILT_IA = 0x55,
    R_TAP_CFG = 0x58,
    R_MD1_CFG = 0x5E,
    R_MD2_CFG = 0x5F,
};

/* Embedded-function bank B */
enum {
    B_WRIST_TILT_LAT = 0x50,
    B_WRIST_TILT_THS = 0x54,
    B_WRIST_TILT_MASK = 0x59,
};

#define ACCESS_BANK_A       BIT(7)
#define ACCESS_BANK_B       BIT(5)
#define CTRL3_SW_RESET      BIT(0)
#define CTRL3_IF_INC        BIT(2)
#define CTRL10_FUNC_EN      BIT(2)
#define CTRL10_TIMER_EN     BIT(5)
#define CTRL10_WRIST_TILT   BIT(7)
#define TAP_CFG_LIR         BIT(0)
#define TAP_CFG_INT_EN      BIT(7)
#define DRDY_INT2_TILT      BIT(0)
#define MD_DOUBLE_TAP       BIT(3)
#define MD_SINGLE_TAP       BIT(6)
#define INT_DRDY_XL         BIT(0)
#define INT_DRDY_G          BIT(1)
#define STATUS_XLDA         BIT(0)
#define STATUS_GDA          BIT(1)
#define STATUS_TDA          BIT(2)
#define FUNC2_WRIST_TILT    BIT(0)
#define FUNC1_STEP_DETECTED BIT(4)
#define FIFO2_EMPTY         BIT(4)
#define FIFO2_FULL          BIT(5)
#define FIFO2_OVER_RUN      BIT(6)
#define FIFO2_WATERM        BIT(7)
#define TAP_SRC_Z           BIT(0)
#define TAP_SRC_DOUBLE      BIT(4)
#define TAP_SRC_SINGLE      BIT(5)
#define TAP_SRC_IA          BIT(6)

#define TICK_NS (5 * SCALE_MS)

/* Hz*10 for the four-bit ODR fields shared by both sensors and the FIFO. */
static const uint32_t odr_tenths[16] = {
    0, 125, 260, 520, 1040, 2080, 4160, 8330, 16660, 33330, 66660, 0, 0, 0, 0, 0,
};

static uint32_t xl_odr(ArtemisLsm6State *s)
{
    return odr_tenths[s->bank_user[R_CTRL1_XL] >> 4];
}

static uint32_t g_odr(ArtemisLsm6State *s)
{
    return odr_tenths[s->bank_user[R_CTRL2_G] >> 4];
}

static int16_t accel_counts(ArtemisLsm6State *s, int axis)
{
    /* 0.061, 0.488, 0.122, 0.244 mg/LSB for FS_XL 00, 01, 10, 11. */
    static const float mg_per_lsb[4] = { 0.061f, 0.488f, 0.122f, 0.244f };
    float v = s->accel_mg[axis] / mg_per_lsb[s->bank_user[R_CTRL1_XL] >> 2 & 3];

    return (int16_t)CLAMP(v, -32768.0f, 32767.0f);
}

static int16_t gyro_counts(ArtemisLsm6State *s, int axis)
{
    uint8_t ctrl = s->bank_user[R_CTRL2_G];
    /* FS_125 overrides FS_G; otherwise 8.75, 17.5, 35, 70 mdps/LSB. */
    static const float mdps_per_lsb[4] = { 8.75f, 17.5f, 35.0f, 70.0f };
    float sens = (ctrl & BIT(1)) ? 4.375f : mdps_per_lsb[ctrl >> 2 & 3];
    float v = s->gyro_mdps[axis] / sens;

    return (int16_t)CLAMP(v, -32768.0f, 32767.0f);
}

static int16_t temp_counts(ArtemisLsm6State *s)
{
    float v = (s->temp_c - 25.0f) * 256.0f;

    return (int16_t)CLAMP(v, -32768.0f, 32767.0f);
}

static void update_irqs(ArtemisLsm6State *s)
{
    uint8_t *u = s->bank_user;
    uint8_t status = u[R_STATUS_REG];
    bool int1 = false;
    bool int2 = false;

    int1 |= (u[R_INT1_CTRL] & INT_DRDY_XL) && (status & STATUS_XLDA);
    int1 |= (u[R_INT1_CTRL] & INT_DRDY_G) && (status & STATUS_GDA);
    int2 |= (u[R_INT2_CTRL] & INT_DRDY_XL) && (status & STATUS_XLDA);
    int2 |= (u[R_INT2_CTRL] & INT_DRDY_G) && (status & STATUS_GDA);

    if (u[R_DRDY_PULSE_CFG_G] & DRDY_INT2_TILT &&
        u[R_FUNC_SRC2] & FUNC2_WRIST_TILT) {
        int2 = true;
    }

    if (u[R_TAP_CFG] & TAP_CFG_INT_EN) {
        uint8_t tap = u[R_TAP_SRC];
        int1 |= (u[R_MD1_CFG] & MD_SINGLE_TAP) && (tap & TAP_SRC_SINGLE);
        int1 |= (u[R_MD1_CFG] & MD_DOUBLE_TAP) && (tap & TAP_SRC_DOUBLE);
        int2 |= (u[R_MD2_CFG] & MD_SINGLE_TAP) && (tap & TAP_SRC_SINGLE);
        int2 |= (u[R_MD2_CFG] & MD_DOUBLE_TAP) && (tap & TAP_SRC_DOUBLE);
    }

    qemu_set_irq(s->int1, int1);
    qemu_set_irq(s->int2, int2);
}

static void fifo_push(ArtemisLsm6State *s, int16_t word)
{
    uint32_t tail = (s->fifo_head + s->fifo_len) % ARTEMIS_LSM6_FIFO_WORDS;

    s->fifo[tail] = word;
    if (s->fifo_len < ARTEMIS_LSM6_FIFO_WORDS) {
        s->fifo_len++;
    } else {
        s->fifo_head = (s->fifo_head + 1) % ARTEMIS_LSM6_FIFO_WORDS;
    }
}

/* Catches the FIFO up with elapsed time; samples are generated on demand. */
static void fifo_catch_up(ArtemisLsm6State *s)
{
    uint8_t *u = s->bank_user;
    uint32_t mode = u[R_FIFO_CTRL5] & 7;
    uint32_t odr = odr_tenths[u[R_FIFO_CTRL5] >> 3 & 15];
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (mode == 0 || odr == 0) {
        s->fifo_last_ns = now;
        return;
    }

    bool want_g = u[R_FIFO_CTRL3] >> 3 & 7;
    bool want_xl = u[R_FIFO_CTRL3] & 7;
    int64_t period = NANOSECONDS_PER_SECOND * 10 / odr;

    while (s->fifo_last_ns + period <= now) {
        s->fifo_last_ns += period;

        /* FIFO mode (1) stops when full; the stream modes overwrite. */
        if (mode == 1 && s->fifo_len + 6 > ARTEMIS_LSM6_FIFO_WORDS) {
            continue;
        }
        if (want_g) {
            for (int i = 0; i < 3; i++) {
                fifo_push(s, gyro_counts(s, i));
            }
        }
        if (want_xl) {
            for (int i = 0; i < 3; i++) {
                fifo_push(s, accel_counts(s, i));
            }
        }
    }
}

static uint32_t tilt_threshold_mg(ArtemisLsm6State *s)
{
    return (uint32_t)s->bank_b[B_WRIST_TILT_THS] * 15625 / 1000;
}

/* A masked direction fires once it has held past the threshold long enough. */
static void evaluate_wrist_tilt(ArtemisLsm6State *s, uint32_t elapsed_ms)
{
    uint8_t *u = s->bank_user;
    uint8_t mask = s->bank_b[B_WRIST_TILT_MASK];
    uint32_t latency_ms = (uint32_t)s->bank_b[B_WRIST_TILT_LAT] * 40;
    uint32_t threshold = tilt_threshold_mg(s);

    if (!(u[R_CTRL10_C] & CTRL10_WRIST_TILT) || !(u[R_CTRL10_C] & CTRL10_FUNC_EN)) {
        memset(s->tilt_armed, 1, sizeof(s->tilt_armed));
        memset(s->tilt_hold_ms, 0, sizeof(s->tilt_hold_ms));
        return;
    }

    /* Bits 2..7: Z-, Z+, Y-, Y+, X-, X+. */
    for (int dir = 0; dir < 6; dir++) {
        int axis = 2 - dir / 2;
        bool negative = !(dir & 1);
        float v = negative ? -s->accel_mg[axis] : s->accel_mg[axis];

        if (!(mask & BIT(2 + dir))) {
            continue;
        }
        if (v < threshold) {
            s->tilt_armed[dir] = true;
            s->tilt_hold_ms[dir] = 0;
            continue;
        }
        if (s->tilt_armed[dir]) {
            s->tilt_hold_ms[dir] += elapsed_ms;
            if (s->tilt_hold_ms[dir] >= latency_ms) {
                s->tilt_hold_ms[dir] = 0;
                s->tilt_armed[dir] = false;
                u[R_WRIST_TILT_IA] |= BIT(2 + dir);
                u[R_FUNC_SRC2] |= FUNC2_WRIST_TILT;
            }
        }
    }
}

/* New data appears once per ODR period after the previous read. */
static void refresh_status(ArtemisLsm6State *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint8_t status = s->bank_user[R_STATUS_REG];
    bool xl = xl_odr(s);
    bool g = g_odr(s);

    status = (xl && now >= s->xl_ready_ns) ? status | STATUS_XLDA
                                           : status & ~STATUS_XLDA;
    status = (g && now >= s->g_ready_ns) ? status | STATUS_GDA
                                         : status & ~STATUS_GDA;
    status = ((xl || g) && now >= s->t_ready_ns) ? status | STATUS_TDA
                                                 : status & ~STATUS_TDA;
    s->bank_user[R_STATUS_REG] = status;
}

static void tick(void *opaque)
{
    ArtemisLsm6State *s = ARTEMIS_LSM6DS3TRC(opaque);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t elapsed_ms = (now - s->last_tick_ns) / SCALE_MS;

    if (elapsed_ms) {
        s->last_tick_ns = now;
        evaluate_wrist_tilt(s, elapsed_ms);
    }

    refresh_status(s);
    update_irqs(s);
    timer_mod(s->timer, now + TICK_NS);
}

static void chip_reset(ArtemisLsm6State *s)
{
    memset(s->bank_user, 0, sizeof(s->bank_user));
    memset(s->bank_a, 0, sizeof(s->bank_a));
    memset(s->bank_b, 0, sizeof(s->bank_b));
    memset(s->latch, 0, sizeof(s->latch));
    memset(s->tilt_armed, 1, sizeof(s->tilt_armed));
    memset(s->tilt_hold_ms, 0, sizeof(s->tilt_hold_ms));
    s->bank_user[R_WHO_AM_I] = 0x6A;
    s->bank_user[R_CTRL3_C] = CTRL3_IF_INC;
    s->bank_b[B_WRIST_TILT_LAT] = 0x0F;
    s->bank_b[B_WRIST_TILT_THS] = 0x20;
    s->bank_b[B_WRIST_TILT_MASK] = 0xC0;
    s->fifo_head = 0;
    s->fifo_len = 0;
    s->step_count = 0;
    s->pointer = 0;
    s->have_pointer = false;
    s->boot_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->fifo_last_ns = s->boot_ns;
    s->last_tick_ns = s->boot_ns;
}

void artemis_lsm6_power_cycle(ArtemisLsm6State *s)
{
    chip_reset(s);
    update_irqs(s);
}

void artemis_lsm6_set_motion(ArtemisLsm6State *s, const float accel_mg[3],
                             const float gyro_mdps[3], float temp_c)
{
    memcpy(s->accel_mg, accel_mg, sizeof(s->accel_mg));
    memcpy(s->gyro_mdps, gyro_mdps, sizeof(s->gyro_mdps));
    s->temp_c = temp_c;
}

void artemis_lsm6_inject_tap(ArtemisLsm6State *s, int axis, bool double_tap)
{
    uint8_t axis_bit = BIT(CLAMP(axis, 0, 2) == 2 ? 0 : (axis == 1 ? 1 : 2));

    s->bank_user[R_TAP_SRC] = TAP_SRC_IA | axis_bit |
                              (double_tap ? TAP_SRC_DOUBLE : TAP_SRC_SINGLE);
    update_irqs(s);
}

void artemis_lsm6_inject_step(ArtemisLsm6State *s, uint32_t steps)
{
    s->step_count += steps;
    s->bank_user[R_FUNC_SRC1] |= FUNC1_STEP_DETECTED;
    update_irqs(s);
}

static int64_t ten_ns_per_sample(uint32_t odr_tenths_hz)
{
    return odr_tenths_hz ? NANOSECONDS_PER_SECOND * 10 / odr_tenths_hz : 0;
}

static uint8_t read_data_reg(ArtemisLsm6State *s, uint8_t reg)
{
    uint32_t idx = reg - R_OUT_TEMP_L;

    /* Reading the low byte latches the word so a pair can't tear. */
    if (!(idx & 1)) {
        int16_t v;

        if (reg == R_OUT_TEMP_L) {
            v = temp_counts(s);
        } else if (reg < R_OUTX_L_XL) {
            v = g_odr(s) ? gyro_counts(s, (reg - R_OUTX_L_G) / 2)
                         : (int16_t)lduw_le_p(&s->latch[idx]);
        } else {
            v = xl_odr(s) ? accel_counts(s, (reg - R_OUTX_L_XL) / 2)
                          : (int16_t)lduw_le_p(&s->latch[idx]);
        }
        stw_le_p(&s->latch[idx], v);

        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        if (reg == R_OUT_TEMP_L) {
            s->bank_user[R_STATUS_REG] &= ~STATUS_TDA;
            s->t_ready_ns = now + ten_ns_per_sample(MAX(xl_odr(s), g_odr(s)));
        } else if (reg < R_OUTX_L_XL) {
            s->bank_user[R_STATUS_REG] &= ~STATUS_GDA;
            s->g_ready_ns = now + ten_ns_per_sample(g_odr(s));
        } else {
            s->bank_user[R_STATUS_REG] &= ~STATUS_XLDA;
            s->xl_ready_ns = now + ten_ns_per_sample(xl_odr(s));
        }
    }
    return s->latch[idx];
}

static uint32_t timestamp_counts(ArtemisLsm6State *s)
{
    if (!(s->bank_user[R_CTRL10_C] & CTRL10_TIMER_EN)) {
        return 0;
    }
    /* 25 us per LSB at the default timestamp resolution. */
    return (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->boot_ns) / (25 * SCALE_US);
}

static uint8_t read_user_reg(ArtemisLsm6State *s, uint8_t reg)
{
    uint8_t *u = s->bank_user;
    uint8_t v;

    switch (reg) {
    case R_STATUS_REG:
        refresh_status(s);
        return u[reg];

    case R_OUT_TEMP_L ... R_OUTZ_H_XL:
        v = read_data_reg(s, reg);
        update_irqs(s);
        return v;

    case R_FIFO_STATUS1:
        fifo_catch_up(s);
        return s->fifo_len & 0xFF;
    case R_FIFO_STATUS2: {
        uint32_t threshold = (u[R_FIFO_CTRL2] & 7) << 8 | u[R_FIFO_CTRL1];

        fifo_catch_up(s);
        v = s->fifo_len >> 8 & 7;
        if (s->fifo_len == 0) {
            v |= FIFO2_EMPTY;
        }
        if (s->fifo_len >= ARTEMIS_LSM6_FIFO_WORDS) {
            v |= FIFO2_FULL | FIFO2_OVER_RUN;
        }
        if (threshold && s->fifo_len >= threshold) {
            v |= FIFO2_WATERM;
        }
        return v;
    }
    case R_FIFO_STATUS3:
    case R_FIFO_STATUS4: {
        /* Pattern position within a {Gx,Gy,Gz,XLx,XLy,XLz} set. */
        bool both = (u[R_FIFO_CTRL3] >> 3 & 7) && (u[R_FIFO_CTRL3] & 7);
        uint32_t words = both ? 6 : 3;
        return reg == R_FIFO_STATUS3 ? s->fifo_head % words : 0;
    }
    case R_FIFO_DATA_OUT_L:
        fifo_catch_up(s);
        if (s->fifo_len == 0) {
            s->latch[14] = s->latch[15] = 0;
            return 0;
        }
        stw_le_p(&s->latch[14], s->fifo[s->fifo_head]);
        s->fifo_head = (s->fifo_head + 1) % ARTEMIS_LSM6_FIFO_WORDS;
        s->fifo_len--;
        return s->latch[14];
    case R_FIFO_DATA_OUT_H:
        return s->latch[15];

    case R_TIMESTAMP0:
        return timestamp_counts(s);
    case R_TIMESTAMP1:
        return timestamp_counts(s) >> 8;
    case R_TIMESTAMP2:
        return timestamp_counts(s) >> 16;
    case R_STEP_COUNTER_L:
        return s->step_count;
    case R_STEP_COUNTER_H:
        return s->step_count >> 8;

    /* Event sources clear when read while interrupts are latched. */
    case R_TAP_SRC:
        v = u[reg];
        u[reg] = 0;
        update_irqs(s);
        return v;
    case R_FUNC_SRC1:
    case R_FUNC_SRC2:
    case R_WRIST_TILT_IA:
        v = u[reg];
        u[reg] = 0;
        if (reg == R_WRIST_TILT_IA) {
            u[R_FUNC_SRC2] &= ~FUNC2_WRIST_TILT;
        }
        update_irqs(s);
        return v;

    default:
        return u[reg];
    }
}

static void write_user_reg(ArtemisLsm6State *s, uint8_t reg, uint8_t value)
{
    uint8_t *u = s->bank_user;

    switch (reg) {
    case R_WHO_AM_I:
        return;
    case R_CTRL3_C:
        if (value & CTRL3_SW_RESET) {
            chip_reset(s);
            update_irqs(s);
            return;
        }
        u[reg] = value;
        return;
    case R_FIFO_CTRL5:
        /* Entering bypass mode discards the contents. */
        if ((value & 7) == 0) {
            s->fifo_head = 0;
            s->fifo_len = 0;
        }
        s->fifo_last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        u[reg] = value;
        return;
    case R_CTRL10_C:
        if (value & BIT(1)) {
            s->step_count = 0; /* PEDO_RST_STEP */
        }
        u[reg] = value;
        return;
    default:
        u[reg] = value;
        update_irqs(s);
        return;
    }
}

static uint8_t *selected_bank(ArtemisLsm6State *s, uint8_t reg)
{
    uint8_t access = s->bank_user[R_FUNC_CFG_ACCESS];

    if (reg == R_FUNC_CFG_ACCESS || !(access & ACCESS_BANK_A)) {
        return NULL;
    }
    return (access & ACCESS_BANK_B) ? s->bank_b : s->bank_a;
}

static void advance_pointer(ArtemisLsm6State *s)
{
    if (s->bank_user[R_CTRL3_C] & CTRL3_IF_INC) {
        s->pointer++;
    }
}

static int lsm6_event(I2CSlave *i2c, enum i2c_event event)
{
    ArtemisLsm6State *s = ARTEMIS_LSM6DS3TRC(i2c);

    if (event == I2C_START_SEND) {
        s->have_pointer = false;
    }
    return 0;
}

static uint8_t lsm6_recv(I2CSlave *i2c)
{
    ArtemisLsm6State *s = ARTEMIS_LSM6DS3TRC(i2c);
    uint8_t *bank = selected_bank(s, s->pointer);
    uint8_t v = bank ? bank[s->pointer] : read_user_reg(s, s->pointer);

    advance_pointer(s);
    return v;
}

static int lsm6_send(I2CSlave *i2c, uint8_t data)
{
    ArtemisLsm6State *s = ARTEMIS_LSM6DS3TRC(i2c);

    if (!s->have_pointer) {
        s->pointer = data;
        s->have_pointer = true;
        return 0;
    }

    uint8_t *bank = selected_bank(s, s->pointer);

    if (bank) {
        bank[s->pointer] = data;
    } else {
        write_user_reg(s, s->pointer, data);
    }
    advance_pointer(s);
    return 0;
}

static void lsm6_realize(DeviceState *dev, Error **errp)
{
    ArtemisLsm6State *s = ARTEMIS_LSM6DS3TRC(dev);

    qdev_init_gpio_out_named(dev, &s->int1, "int1", 1);
    qdev_init_gpio_out_named(dev, &s->int2, "int2", 1);

    s->accel_mg[2] = 1000.0f;
    s->temp_c = 25.0f;
    chip_reset(s);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tick, s);
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + TICK_NS);
}

static void lsm6_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->realize = lsm6_realize;
    k->event = lsm6_event;
    k->recv = lsm6_recv;
    k->send = lsm6_send;
}

static const TypeInfo lsm6_info = {
    .name = TYPE_ARTEMIS_LSM6DS3TRC,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(ArtemisLsm6State),
    .class_init = lsm6_class_init,
};

static void lsm6_register_types(void)
{
    type_register_static(&lsm6_info);
}

type_init(lsm6_register_types)
