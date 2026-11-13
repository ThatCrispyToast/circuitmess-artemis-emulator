/*
 * ST LSM6DS3TR-C accelerometer and gyroscope, I2C interface.
 *
 * Output registers come from a physical state the front end sets. Modelled
 * beyond plain sampling: register banks, data-ready flags on the sensor's own
 * sampling clock, the FIFO with its flags and interrupts, the timestamp
 * counter, and the events firmware can have routed to the interrupt pins:
 * wrist tilt (which wakes the stock firmware), tilt, free fall, wake-up,
 * inactivity and 6D orientation, worked out from the acceleration; taps and
 * steps, which the front end injects.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/log.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
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
    R_CTRL9_XL = 0x18,
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
    R_TAP_THS_6D = 0x59,
    R_WAKE_UP_THS = 0x5B,
    R_WAKE_UP_DUR = 0x5C,
    R_FREE_FALL = 0x5D,
    R_MD1_CFG = 0x5E,
    R_MD2_CFG = 0x5F,
};

/* Embedded-function bank A */
enum {
    A_CONFIG_PEDO_THS_MIN = 0x0F,
    A_SM_THS = 0x13,
    A_PEDO_DEB_REG = 0x14,
    A_MAG_SI_XX = 0x24,
    A_MAG_SI_YY = 0x28,
    A_MAG_SI_ZZ = 0x2C,
};

/* Embedded-function bank B */
enum {
    B_WRIST_TILT_LAT = 0x50,
    B_WRIST_TILT_THS = 0x54,
    B_WRIST_TILT_MASK = 0x59,
};

/* Bits the part returns set in FUNC_SRC2 beyond the documented ones. */
#define FUNC_SRC2_FIXED 0x84

#define ACCESS_BANK_A       BIT(7)
#define ACCESS_BANK_B       BIT(5)
#define CTRL3_SW_RESET      BIT(0)
#define CTRL3_IF_INC        BIT(2)
#define CTRL10_PEDO_RST     BIT(1)
#define CTRL10_FUNC_EN      BIT(2)
#define CTRL10_TILT_EN      BIT(3)
#define CTRL10_TIMER_EN     BIT(5)
#define CTRL10_WRIST_TILT   BIT(7)
#define TAP_CFG_LIR         BIT(0)
#define TAP_CFG_INACT_S     5
#define TAP_CFG_INT_EN      BIT(7)
#define DRDY_INT2_TILT      BIT(0)
#define WAKE_DUR_TIMER_HR   BIT(4)
#define WAKE_DUR_FF_DUR5    BIT(7)

/* INT1_CTRL and INT2_CTRL */
#define INT_DRDY_XL         BIT(0)
#define INT_DRDY_G          BIT(1)
#define INT_FTH             BIT(3)
#define INT_FIFO_OVR        BIT(4)
#define INT_FULL_FLAG       BIT(5)
#define INT1_STEP_DETECTOR  BIT(7)

/* MD1_CFG and MD2_CFG */
#define MD_TILT             BIT(1)
#define MD_6D               BIT(2)
#define MD_DOUBLE_TAP       BIT(3)
#define MD_FF               BIT(4)
#define MD_WU               BIT(5)
#define MD_SINGLE_TAP       BIT(6)
#define MD_INACT_STATE      BIT(7)

#define STATUS_XLDA         BIT(0)
#define STATUS_GDA          BIT(1)
#define STATUS_TDA          BIT(2)
#define FUNC2_WRIST_TILT    BIT(0)
#define FUNC1_STEP_DETECTED BIT(4)
#define FUNC1_TILT_IA       BIT(5)
#define FIFO2_EMPTY         BIT(4)
#define FIFO2_FULL          BIT(5)
#define FIFO2_OVER_RUN      BIT(6)
#define FIFO2_WATERM        BIT(7)
#define TAP_SRC_Z           BIT(0)
#define TAP_SRC_DOUBLE      BIT(4)
#define TAP_SRC_SINGLE      BIT(5)
#define TAP_SRC_IA          BIT(6)
#define WAKE_SRC_WU_AXES    7
#define WAKE_SRC_WU_IA      BIT(3)
#define WAKE_SRC_SLEEP      BIT(4)
#define WAKE_SRC_FF_IA      BIT(5)
#define D6D_SRC_ZONES       0x3F
#define D6D_SRC_IA          BIT(6)

#define TICK_NS (5 * SCALE_MS)

/* The tilt function wants about 35 degrees held for two seconds. */
#define TILT_COS_35         0.819f
#define TILT_HOLD_MS        2000

/* Typical output noise at the rates the watch uses. */
#define NOISE_ACCEL_MG      1.5f
#define NOISE_GYRO_MDPS     60.0f

/* Hz*10 for the four-bit ODR fields shared by both sensors and the FIFO. */
static const uint32_t odr_tenths[16] = {
    0, 125, 260, 520, 1040, 2080, 4160, 8330, 16660, 33330, 66660, 0, 0, 0, 0, 0,
};

static int64_t now_ns(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static uint32_t xl_odr(ArtemisLsm6State *s)
{
    return odr_tenths[s->bank_user[R_CTRL1_XL] >> 4];
}

static uint32_t g_odr(ArtemisLsm6State *s)
{
    return odr_tenths[s->bank_user[R_CTRL2_G] >> 4];
}

static int64_t sample_period_ns(uint32_t odr_tenths_hz)
{
    return odr_tenths_hz ? NANOSECONDS_PER_SECOND * 10 / odr_tenths_hz : 0;
}

/* Roughly normal, from four uniform draws; zero unless noise is switched on. */
static float noise(ArtemisLsm6State *s, float rms)
{
    float sum = 0;

    if (!s->noise) {
        return 0;
    }
    for (int i = 0; i < 4; i++) {
        s->rng ^= s->rng << 13;
        s->rng ^= s->rng >> 17;
        s->rng ^= s->rng << 5;
        sum += (float)(s->rng & 0xFFFF) / 65535.0f - 0.5f;
    }
    /* Four uniforms on [-0.5, 0.5] add up to a deviation of 1/sqrt(3). */
    return sum * 1.732f * rms;
}

static float accel_full_scale_mg(ArtemisLsm6State *s)
{
    static const float full_scale[4] = { 2000, 16000, 4000, 8000 };

    return full_scale[s->bank_user[R_CTRL1_XL] >> 2 & 3];
}

static int16_t accel_counts(ArtemisLsm6State *s, int axis)
{
    /* 0.061, 0.488, 0.122, 0.244 mg/LSB for FS_XL 00, 01, 10, 11. */
    static const float mg_per_lsb[4] = { 0.061f, 0.488f, 0.122f, 0.244f };
    float mg = s->accel_mg[axis] + noise(s, NOISE_ACCEL_MG);
    float v = mg / mg_per_lsb[s->bank_user[R_CTRL1_XL] >> 2 & 3];

    return (int16_t)CLAMP(v, -32768.0f, 32767.0f);
}

static int16_t gyro_counts(ArtemisLsm6State *s, int axis)
{
    uint8_t ctrl = s->bank_user[R_CTRL2_G];
    /* FS_125 overrides FS_G; otherwise 8.75, 17.5, 35, 70 mdps/LSB. */
    static const float mdps_per_lsb[4] = { 8.75f, 17.5f, 35.0f, 70.0f };
    float sens = (ctrl & BIT(1)) ? 4.375f : mdps_per_lsb[ctrl >> 2 & 3];
    float v = (s->gyro_mdps[axis] + noise(s, NOISE_GYRO_MDPS)) / sens;

    return (int16_t)CLAMP(v, -32768.0f, 32767.0f);
}

static int16_t temp_counts(ArtemisLsm6State *s)
{
    float v = (s->temp_c - 25.0f) * 256.0f;

    return (int16_t)CLAMP(v, -32768.0f, 32767.0f);
}

static uint32_t fifo_threshold(ArtemisLsm6State *s)
{
    return (s->bank_user[R_FIFO_CTRL2] & 7) << 8 | s->bank_user[R_FIFO_CTRL1];
}

static uint8_t fifo_flags(ArtemisLsm6State *s)
{
    uint32_t threshold = fifo_threshold(s);
    uint8_t v = 0;

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

static void update_irqs(ArtemisLsm6State *s)
{
    uint8_t *u = s->bank_user;
    uint8_t status = u[R_STATUS_REG];
    uint8_t fifo = fifo_flags(s);
    uint8_t ctrl[2] = { u[R_INT1_CTRL], u[R_INT2_CTRL] };
    uint8_t md[2] = { u[R_MD1_CFG], u[R_MD2_CFG] };
    bool level[2] = { false, false };

    for (int i = 0; i < 2; i++) {
        level[i] |= (ctrl[i] & INT_DRDY_XL) && (status & STATUS_XLDA);
        level[i] |= (ctrl[i] & INT_DRDY_G) && (status & STATUS_GDA);
        level[i] |= (ctrl[i] & INT_FTH) && (fifo & FIFO2_WATERM);
        level[i] |= (ctrl[i] & INT_FIFO_OVR) && (fifo & FIFO2_OVER_RUN);
        level[i] |= (ctrl[i] & INT_FULL_FLAG) && (fifo & FIFO2_FULL);
    }
    level[0] |= (ctrl[0] & INT1_STEP_DETECTOR) && (u[R_FUNC_SRC1] & FUNC1_STEP_DETECTED);

    if (u[R_DRDY_PULSE_CFG_G] & DRDY_INT2_TILT &&
        u[R_FUNC_SRC2] & FUNC2_WRIST_TILT) {
        level[1] = true;
    }

    if (u[R_TAP_CFG] & TAP_CFG_INT_EN) {
        uint8_t tap = u[R_TAP_SRC];
        uint8_t wake = u[R_WAKE_UP_SRC];

        for (int i = 0; i < 2; i++) {
            level[i] |= (md[i] & MD_SINGLE_TAP) && (tap & TAP_SRC_SINGLE);
            level[i] |= (md[i] & MD_DOUBLE_TAP) && (tap & TAP_SRC_DOUBLE);
            level[i] |= (md[i] & MD_FF) && (wake & WAKE_SRC_FF_IA);
            level[i] |= (md[i] & MD_WU) && (wake & WAKE_SRC_WU_IA);
            level[i] |= (md[i] & MD_INACT_STATE) && (wake & WAKE_SRC_SLEEP);
            level[i] |= (md[i] & MD_6D) && (u[R_D6D_SRC] & D6D_SRC_IA);
            level[i] |= (md[i] & MD_TILT) && (u[R_FUNC_SRC1] & FUNC1_TILT_IA);
        }
    }

    qemu_set_irq(s->int1, level[0]);
    qemu_set_irq(s->int2, level[1]);
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
    int64_t now = now_ns();

    if (mode == 0 || odr == 0) {
        s->fifo_last_ns = now;
        return;
    }

    bool want_g = u[R_FIFO_CTRL3] >> 3 & 7;
    bool want_xl = u[R_FIFO_CTRL3] & 7;
    int64_t period = sample_period_ns(odr);

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

/* ------------------------------------------------------------------------ */
/* Events worked out from the acceleration                                   */

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

/* Tilt: the watch has stayed turned away from where it was. */
static void evaluate_tilt(ArtemisLsm6State *s, uint32_t elapsed_ms)
{
    uint8_t *u = s->bank_user;
    float *a = s->accel_mg;
    float *ref = s->tilt_ref_mg;
    float norm = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    float ref_norm = sqrtf(ref[0] * ref[0] + ref[1] * ref[1] + ref[2] * ref[2]);

    if (!(u[R_CTRL10_C] & CTRL10_TILT_EN) || !(u[R_CTRL10_C] & CTRL10_FUNC_EN) ||
        !xl_odr(s)) {
        s->tilt_ref_valid = false;
        return;
    }
    if (!s->tilt_ref_valid || ref_norm < 1 || norm < 1) {
        memcpy(ref, a, sizeof(s->tilt_ref_mg));
        s->tilt_ref_valid = true;
        s->tilt_away_ms = 0;
        return;
    }

    float cosine = (a[0] * ref[0] + a[1] * ref[1] + a[2] * ref[2]) / (norm * ref_norm);

    if (cosine > TILT_COS_35) {
        s->tilt_away_ms = 0;
        return;
    }
    s->tilt_away_ms += elapsed_ms;
    if (s->tilt_away_ms >= TILT_HOLD_MS) {
        u[R_FUNC_SRC1] |= FUNC1_TILT_IA;
        memcpy(ref, a, sizeof(s->tilt_ref_mg));
        s->tilt_away_ms = 0;
    }
}

/* Free fall: every axis below the threshold for the set number of samples. */
static void evaluate_free_fall(ArtemisLsm6State *s, uint32_t elapsed_ms)
{
    static const uint16_t threshold_mg[8] = { 156, 219, 250, 312, 344, 406, 469, 500 };
    uint8_t *u = s->bank_user;
    uint32_t odr = xl_odr(s);
    uint32_t samples = u[R_FREE_FALL] >> 3 | (u[R_WAKE_UP_DUR] & WAKE_DUR_FF_DUR5 ? 32 : 0);
    uint32_t threshold = threshold_mg[u[R_FREE_FALL] & 7];
    bool falling = odr != 0;

    for (int i = 0; i < 3; i++) {
        falling &= fabsf(s->accel_mg[i]) < threshold;
    }

    if (!falling) {
        s->free_fall_ms = 0;
        if (!(u[R_TAP_CFG] & TAP_CFG_LIR)) {
            u[R_WAKE_UP_SRC] &= ~WAKE_SRC_FF_IA;
        }
        return;
    }
    s->free_fall_ms += elapsed_ms;
    if ((uint64_t)s->free_fall_ms * odr >= (uint64_t)samples * 10000) {
        u[R_WAKE_UP_SRC] |= WAKE_SRC_FF_IA;
    }
}

/* Inactivity: nothing has moved past the wake-up threshold for the sleep time. */
static void evaluate_inactivity(ArtemisLsm6State *s, uint32_t elapsed_ms)
{
    uint8_t *u = s->bank_user;
    uint32_t odr = xl_odr(s);
    uint32_t sleep_dur = u[R_WAKE_UP_DUR] & 0xF;

    if (!(u[R_TAP_CFG] >> TAP_CFG_INACT_S & 3) || !odr) {
        s->still_ms = 0;
        u[R_WAKE_UP_SRC] &= ~WAKE_SRC_SLEEP;
        return;
    }
    s->still_ms += elapsed_ms;
    /* One step of SLEEP_DUR is 512 samples; zero means 16 samples. */
    uint64_t samples = sleep_dur ? sleep_dur * 512 : 16;

    if ((uint64_t)s->still_ms * odr >= samples * 10000) {
        u[R_WAKE_UP_SRC] |= WAKE_SRC_SLEEP;
    }
}

/* 6D: which axis points up or down, past the configured angle. */
static void evaluate_orientation(ArtemisLsm6State *s)
{
    /* Sines of 80, 70, 60 and 50 degrees. */
    static const float threshold_g[4] = { 0.985f, 0.940f, 0.866f, 0.766f };
    uint8_t *u = s->bank_user;
    float threshold = threshold_g[u[R_TAP_THS_6D] >> 5 & 3] * 1000.0f;
    uint8_t zones = 0;

    if (!xl_odr(s)) {
        return;
    }
    for (int axis = 0; axis < 3; axis++) {
        if (s->accel_mg[axis] > threshold) {
            zones |= BIT(axis * 2 + 1);
        } else if (s->accel_mg[axis] < -threshold) {
            zones |= BIT(axis * 2);
        }
    }

    if (zones != (u[R_D6D_SRC] & D6D_SRC_ZONES)) {
        u[R_D6D_SRC] = zones | (s->orientation_known && zones ? D6D_SRC_IA : 0);
        s->orientation_known = true;
    } else if (!(u[R_TAP_CFG] & TAP_CFG_LIR)) {
        u[R_D6D_SRC] &= ~D6D_SRC_IA;
    }
}

/*
 * Wake-up: the slope filter sees half the change between two samples, so a
 * move is judged when it happens rather than on a timer.
 */
static void evaluate_wake_up(ArtemisLsm6State *s, const float previous_mg[3])
{
    uint8_t *u = s->bank_user;
    float threshold = (u[R_WAKE_UP_THS] & 0x3F) * accel_full_scale_mg(s) / 64.0f;
    uint8_t axes = 0;

    if (!xl_odr(s)) {
        return;
    }
    for (int axis = 0; axis < 3; axis++) {
        if (fabsf(s->accel_mg[axis] - previous_mg[axis]) / 2 > threshold) {
            /* X, Y and Z are bits 2, 1 and 0. */
            axes |= BIT(2 - axis);
        }
    }
    if (axes) {
        u[R_WAKE_UP_SRC] = (u[R_WAKE_UP_SRC] & ~(WAKE_SRC_WU_AXES | WAKE_SRC_SLEEP)) |
                           WAKE_SRC_WU_IA | axes;
        s->still_ms = 0;
    }
}

/* Whether a sample has been taken since this sensor's data was last read. */
static bool sample_pending(int64_t now, int64_t start, int64_t last_read,
                           uint32_t odr_tenths_hz)
{
    int64_t period = sample_period_ns(odr_tenths_hz);

    if (!period) {
        return false;
    }
    return (now - start) / period > (last_read - start) / period;
}

/*
 * The sensors sample on their own clock, from when they were switched on. A
 * flag stays up until its data is read, also once the sensor is powered down.
 */
static void refresh_status(ArtemisLsm6State *s)
{
    int64_t now = now_ns();
    uint8_t status = s->bank_user[R_STATUS_REG];
    uint32_t xl = xl_odr(s);
    uint32_t g = g_odr(s);

    if (sample_pending(now, s->xl_start_ns, s->xl_read_ns, xl)) {
        status |= STATUS_XLDA;
    }
    if (sample_pending(now, s->g_start_ns, s->g_read_ns, g)) {
        status |= STATUS_GDA;
    }
    if (sample_pending(now, xl ? s->xl_start_ns : s->g_start_ns, s->t_read_ns, MAX(xl, g))) {
        status |= STATUS_TDA;
    }
    s->bank_user[R_STATUS_REG] = status;
}

static void tick(void *opaque)
{
    ArtemisLsm6State *s = ARTEMIS_LSM6DS3TRC(opaque);
    int64_t now = now_ns();
    uint32_t elapsed_ms = (now - s->last_tick_ns) / SCALE_MS;

    if (elapsed_ms) {
        s->last_tick_ns = now;
        evaluate_wrist_tilt(s, elapsed_ms);
        evaluate_tilt(s, elapsed_ms);
        evaluate_free_fall(s, elapsed_ms);
        evaluate_inactivity(s, elapsed_ms);
        evaluate_orientation(s);
    }

    fifo_catch_up(s);
    refresh_status(s);
    update_irqs(s);
    timer_mod(s->timer, now + TICK_NS);
}

/*
 * SW_RESET: the user registers go back to their defaults. The embedded-function
 * banks and the last samples in the output registers are left as they were,
 * and the data-ready flags of those samples stay up.
 */
static void chip_reset(ArtemisLsm6State *s)
{
    uint8_t status = s->bank_user[R_STATUS_REG];
    int64_t now = now_ns();

    memset(s->bank_user, 0, sizeof(s->bank_user));
    memset(s->tilt_armed, 1, sizeof(s->tilt_armed));
    memset(s->tilt_hold_ms, 0, sizeof(s->tilt_hold_ms));
    s->bank_user[R_WHO_AM_I] = 0x6A;
    s->bank_user[R_CTRL3_C] = CTRL3_IF_INC;
    s->bank_user[R_CTRL9_XL] = 0xE0;
    s->bank_user[R_STATUS_REG] = status;
    s->fifo_head = 0;
    s->fifo_len = 0;
    s->step_count = 0;
    s->pointer = 0;
    s->have_pointer = false;
    s->timestamp_start_ns = now;
    s->fifo_last_ns = now;
    s->last_tick_ns = now;
    s->xl_start_ns = s->g_start_ns = now;
    s->xl_read_ns = s->g_read_ns = s->t_read_ns = now;
    s->tilt_ref_valid = false;
    s->orientation_known = false;
    s->free_fall_ms = 0;
    s->still_ms = 0;
}

void artemis_lsm6_power_cycle(ArtemisLsm6State *s)
{
    memset(s->bank_user, 0, sizeof(s->bank_user));
    memset(s->bank_a, 0, sizeof(s->bank_a));
    memset(s->bank_b, 0, sizeof(s->bank_b));
    memset(s->latch, 0, sizeof(s->latch));
    s->bank_a[A_CONFIG_PEDO_THS_MIN] = 0x10;
    s->bank_a[A_SM_THS] = 0x06;
    s->bank_a[A_PEDO_DEB_REG] = 0x6E;
    s->bank_a[A_MAG_SI_XX] = 0x08;
    s->bank_a[A_MAG_SI_YY] = 0x08;
    s->bank_a[A_MAG_SI_ZZ] = 0x08;
    s->bank_b[B_WRIST_TILT_LAT] = 0x0F;
    s->bank_b[B_WRIST_TILT_THS] = 0x20;
    s->bank_b[B_WRIST_TILT_MASK] = 0xC0;
    chip_reset(s);
    update_irqs(s);
}

void artemis_lsm6_set_motion(ArtemisLsm6State *s, const float accel_mg[3],
                             const float gyro_mdps[3], float temp_c)
{
    float previous[3];

    memcpy(previous, s->accel_mg, sizeof(previous));
    memcpy(s->accel_mg, accel_mg, sizeof(s->accel_mg));
    memcpy(s->gyro_mdps, gyro_mdps, sizeof(s->gyro_mdps));
    s->temp_c = temp_c;

    evaluate_wake_up(s, previous);
    update_irqs(s);
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

static uint8_t read_data_reg(ArtemisLsm6State *s, uint8_t reg)
{
    uint32_t idx = reg - R_OUT_TEMP_L;

    /* Reading the low byte latches the word so a pair can't tear. */
    if (!(idx & 1)) {
        int64_t now = now_ns();
        int16_t v;

        if (reg == R_OUT_TEMP_L) {
            v = temp_counts(s);
            s->bank_user[R_STATUS_REG] &= ~STATUS_TDA;
            s->t_read_ns = now;
        } else if (reg < R_OUTX_L_XL) {
            v = g_odr(s) ? gyro_counts(s, (reg - R_OUTX_L_G) / 2)
                         : (int16_t)lduw_le_p(&s->latch[idx]);
            s->bank_user[R_STATUS_REG] &= ~STATUS_GDA;
            s->g_read_ns = now;
        } else {
            v = xl_odr(s) ? accel_counts(s, (reg - R_OUTX_L_XL) / 2)
                          : (int16_t)lduw_le_p(&s->latch[idx]);
            s->bank_user[R_STATUS_REG] &= ~STATUS_XLDA;
            s->xl_read_ns = now;
        }
        stw_le_p(&s->latch[idx], v);
    }
    return s->latch[idx];
}

static uint32_t timestamp_counts(ArtemisLsm6State *s)
{
    /* 6.4 ms a count, or 25 us with TIMER_HR. */
    int64_t resolution = (s->bank_user[R_WAKE_UP_DUR] & WAKE_DUR_TIMER_HR) ?
                         25 * SCALE_US : 6400 * SCALE_US;

    if (!(s->bank_user[R_CTRL10_C] & CTRL10_TIMER_EN)) {
        return 0;
    }
    return (now_ns() - s->timestamp_start_ns) / resolution;
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
    case R_FIFO_STATUS2:
        fifo_catch_up(s);
        return (s->fifo_len >> 8 & 7) | fifo_flags(s);
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
        update_irqs(s);
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

    /* Event sources clear when read. */
    case R_TAP_SRC:
    case R_WAKE_UP_SRC:
        v = u[reg];
        u[reg] = 0;
        update_irqs(s);
        return v;
    case R_D6D_SRC:
        v = u[reg];
        u[reg] &= ~D6D_SRC_IA;
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
        return reg == R_FUNC_SRC2 ? v | FUNC_SRC2_FIXED : v;

    default:
        return u[reg];
    }
}

static void write_user_reg(ArtemisLsm6State *s, uint8_t reg, uint8_t value)
{
    uint8_t *u = s->bank_user;
    int64_t now = now_ns();

    switch (reg) {
    case R_WHO_AM_I:
        return;
    case R_CTRL1_XL:
        /* Switching the sensor on, or changing its rate, restarts its clock. */
        if (value >> 4 != u[reg] >> 4) {
            s->xl_start_ns = s->xl_read_ns = now;
        }
        u[reg] = value;
        break;
    case R_CTRL2_G:
        if (value >> 4 != u[reg] >> 4) {
            s->g_start_ns = s->g_read_ns = now;
        }
        u[reg] = value;
        break;
    case R_CTRL3_C:
        if (value & CTRL3_SW_RESET) {
            chip_reset(s);
            break;
        }
        u[reg] = value;
        return;
    case R_FIFO_CTRL5:
        /* Entering bypass mode discards the contents. */
        if ((value & 7) == 0) {
            s->fifo_head = 0;
            s->fifo_len = 0;
        }
        s->fifo_last_ns = now;
        u[reg] = value;
        break;
    case R_CTRL10_C:
        if (value & CTRL10_PEDO_RST) {
            s->step_count = 0;
        }
        if ((value & CTRL10_TIMER_EN) && !(u[reg] & CTRL10_TIMER_EN)) {
            s->timestamp_start_ns = now;
        }
        u[reg] = value;
        break;
    case R_TIMESTAMP2:
        /* Writing 0xAA here restarts the timestamp counter. */
        if (value == 0xAA) {
            s->timestamp_start_ns = now;
        }
        return;
    default:
        u[reg] = value;
        break;
    }
    update_irqs(s);
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
    s->rng = 0x2545F491;
    artemis_lsm6_power_cycle(s);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tick, s);
    timer_mod(s->timer, now_ns() + TICK_NS);
}

static Property lsm6_properties[] = {
    /* Adds the part's typical output noise to every sample. */
    DEFINE_PROP_BOOL("noise", ArtemisLsm6State, noise, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void lsm6_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->realize = lsm6_realize;
    device_class_set_props(dc, lsm6_properties);
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
