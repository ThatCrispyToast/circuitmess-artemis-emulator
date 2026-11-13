/*
 * ESP32-S3 LED PWM controller: eight low-speed channels and four timers.
 *
 * No PWM waveform is generated; what matters to the board is each channel's
 * duty and frequency, including hardware fades, which the GPIO matrix exposes
 * on the pads.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/artemis/artemis.h"

#define CH_STRIDE     0x14
#define A_CH_CONF0    0x00
#define A_CH_HPOINT   0x04
#define A_CH_DUTY     0x08
#define A_CH_CONF1    0x0C
#define A_CH_DUTY_R   0x10
#define A_TIMER0_CONF 0xA0
#define A_INT_RAW     0xC0
#define A_INT_ST      0xC4
#define A_INT_ENA     0xC8
#define A_INT_CLR     0xCC
#define A_CONF        0xD0
#define A_DATE        0xFC

#define CONF0_TIMER_SEL_M   3
#define CONF0_SIG_OUT_EN    BIT(2)
#define CONF0_IDLE_LV       BIT(3)
#define CONF0_PARA_UP       BIT(4)
#define CONF1_SCALE_M       0x3FF
#define CONF1_CYCLE_S       10
#define CONF1_CYCLE_M       0x3FF
#define CONF1_NUM_S         20
#define CONF1_NUM_M         0x3FF
#define CONF1_INC           BIT(30)
#define CONF1_START         BIT(31)
#define TIMER_RES_M         0xF
#define TIMER_DIV_S         4
#define TIMER_DIV_M         0x3FFFF
#define TIMER_RST           BIT(23)
#define TIMER_PARA_UP       BIT(25)
#define INT_FADE_END_S      4

static uint32_t clock_hz(ArtemisLedcState *s)
{
    switch (s->conf & 3) {
    case 1:
        return 80000000;
    case 2:
        return 17500000;
    case 3:
        return 40000000;
    default:
        return 0;
    }
}

static uint32_t channel_period_ns(ArtemisLedcState *s, int ch)
{
    uint32_t timer = s->timer_active[s->ch_conf0[ch] & CONF0_TIMER_SEL_M];
    uint32_t div = timer >> TIMER_DIV_S & TIMER_DIV_M;
    uint32_t res = timer & TIMER_RES_M;
    uint64_t hz256 = clock_hz(s);

    if (!div || !hz256) {
        return 0;
    }
    /* div carries eight fractional bits. */
    uint64_t freq = (hz256 << 8) / div >> res;
    return freq ? NANOSECONDS_PER_SECOND / freq : 0;
}

void artemis_ledc_channel_output(ArtemisLedcState *s, int ch, uint32_t *duty,
                                 uint32_t *freq)
{
    uint32_t timer = s->timer_active[s->ch_conf0[ch] & CONF0_TIMER_SEL_M];
    uint32_t res = timer & TIMER_RES_M;

    if (!(s->ch_conf0[ch] & CONF0_SIG_OUT_EN)) {
        *duty = (s->ch_conf0[ch] & CONF0_IDLE_LV) ? 0xFFFF : 0;
        *freq = 0;
        return;
    }

    uint64_t full = (uint64_t)1 << (res + 4);
    uint64_t on = MIN(s->ch_duty_out[ch], full);
    uint32_t period = channel_period_ns(s, ch);

    *duty = on * 0xFFFF / full;
    *freq = period ? NANOSECONDS_PER_SECOND / period : 0;
}

static void update_irq(ArtemisLedcState *s)
{
    qemu_set_irq(s->irq, !!(s->int_raw & s->int_ena));
}

static void outputs_changed(ArtemisLedcState *s)
{
    if (s->notify) {
        s->notify(s->notify_opaque);
    }
}

static void schedule_fade(ArtemisLedcState *s)
{
    int64_t next = INT64_MAX;

    for (int i = 0; i < ARTEMIS_LEDC_CHANNELS; i++) {
        if (s->ch_fading[i]) {
            next = MIN(next, s->ch_fade_next[i]);
        }
    }
    if (next != INT64_MAX) {
        timer_mod(s->fade_timer, next);
    } else {
        timer_del(s->fade_timer);
    }
}

static int64_t fade_step_ns(ArtemisLedcState *s, int ch)
{
    uint32_t cycles = s->ch_conf1[ch] >> CONF1_CYCLE_S & CONF1_CYCLE_M;
    uint32_t period = channel_period_ns(s, ch);

    /* A floor keeps pathological configurations from monopolising the host. */
    return MAX((int64_t)MAX(cycles, 1) * MAX(period, 1), 100 * SCALE_US);
}

static void fade_tick(void *opaque)
{
    ArtemisLedcState *s = ARTEMIS_LEDC(opaque);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool changed = false;

    for (int i = 0; i < ARTEMIS_LEDC_CHANNELS; i++) {
        if (!s->ch_fading[i]) {
            continue;
        }
        while (s->ch_fading[i] && s->ch_fade_next[i] <= now) {
            uint32_t scale = s->ch_conf1[i] & CONF1_SCALE_M;
            uint32_t num = s->ch_conf1[i] >> CONF1_NUM_S & CONF1_NUM_M;

            if (s->ch_conf1[i] & CONF1_INC) {
                s->ch_duty_out[i] += scale << 4;
            } else {
                uint32_t step = scale << 4;
                s->ch_duty_out[i] = s->ch_duty_out[i] > step ?
                                    s->ch_duty_out[i] - step : 0;
            }
            changed = true;

            if (num <= 1) {
                s->ch_conf1[i] &= ~(CONF1_START |
                                    (CONF1_NUM_M << CONF1_NUM_S));
                s->ch_fading[i] = false;
                s->int_raw |= BIT(INT_FADE_END_S + i);
            } else {
                s->ch_conf1[i] = (s->ch_conf1[i] &
                                  ~(CONF1_NUM_M << CONF1_NUM_S)) |
                                 ((num - 1) << CONF1_NUM_S);
                s->ch_fade_next[i] += fade_step_ns(s, i);
            }
        }
    }

    schedule_fade(s);
    update_irq(s);
    if (changed) {
        outputs_changed(s);
    }
}

/* Applies a channel's shadow registers, as PARA_UP does on the hardware. */
static void channel_para_up(ArtemisLedcState *s, int ch)
{
    uint32_t conf1 = s->ch_conf1[ch];
    uint32_t num = conf1 >> CONF1_NUM_S & CONF1_NUM_M;
    uint32_t scale = conf1 & CONF1_SCALE_M;

    if (!(conf1 & CONF1_START)) {
        return;
    }

    s->ch_duty_out[ch] = s->ch_duty[ch];

    if (num > 0 && scale > 0) {
        s->ch_fading[ch] = true;
        s->ch_fade_next[ch] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                              fade_step_ns(s, ch);
        schedule_fade(s);
    } else {
        /* No ramp to run, so the change takes effect at the next period. */
        s->ch_conf1[ch] &= ~CONF1_START;
        s->int_raw |= BIT(INT_FADE_END_S + ch);
        update_irq(s);
    }
}

static uint64_t ledc_read(void *opaque, hwaddr addr, unsigned size)
{
    ArtemisLedcState *s = ARTEMIS_LEDC(opaque);

    if (addr < A_TIMER0_CONF) {
        int ch = addr / CH_STRIDE;
        switch (addr % CH_STRIDE) {
        case A_CH_CONF0:
            return s->ch_conf0[ch] & ~CONF0_PARA_UP;
        case A_CH_HPOINT:
            return s->ch_hpoint[ch];
        case A_CH_DUTY:
            return s->ch_duty[ch];
        case A_CH_CONF1:
            return s->ch_conf1[ch];
        case A_CH_DUTY_R:
            return s->ch_duty_out[ch];
        }
    }

    if (addr < A_INT_RAW) {
        int t = (addr - A_TIMER0_CONF) / 8;
        if ((addr - A_TIMER0_CONF) % 8 == 0) {
            return s->timer_conf[t];
        }
        return 0; /* timer counter value */
    }

    switch (addr) {
    case A_INT_RAW:
        return s->int_raw;
    case A_INT_ST:
        return s->int_raw & s->int_ena;
    case A_INT_ENA:
        return s->int_ena;
    case A_CONF:
        return s->conf;
    case A_DATE:
        return 0x2101220;
    default:
        return 0;
    }
}

static void ledc_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ArtemisLedcState *s = ARTEMIS_LEDC(opaque);
    uint32_t v = value;

    if (addr < A_TIMER0_CONF) {
        int ch = addr / CH_STRIDE;
        switch (addr % CH_STRIDE) {
        case A_CH_CONF0:
            s->ch_conf0[ch] = v;
            if (v & CONF0_PARA_UP) {
                channel_para_up(s, ch);
            }
            outputs_changed(s);
            break;
        case A_CH_HPOINT:
            s->ch_hpoint[ch] = v;
            break;
        case A_CH_DUTY:
            s->ch_duty[ch] = v & 0x1FFFFFF;
            break;
        case A_CH_CONF1:
            s->ch_conf1[ch] = v;
            break;
        }
        return;
    }

    if (addr < A_INT_RAW) {
        if ((addr - A_TIMER0_CONF) % 8 == 0) {
            int t = (addr - A_TIMER0_CONF) / 8;

            /* The divider and resolution only take effect on PARA_UP. */
            s->timer_conf[t] = v & ~TIMER_PARA_UP;
            if (v & TIMER_PARA_UP) {
                s->timer_active[t] = s->timer_conf[t];
                outputs_changed(s);
            }
        }
        return;
    }

    switch (addr) {
    case A_INT_ENA:
        s->int_ena = v;
        update_irq(s);
        break;
    case A_INT_CLR:
        s->int_raw &= ~v;
        update_irq(s);
        break;
    case A_CONF:
        s->conf = v;
        outputs_changed(s);
        break;
    }
}

static const MemoryRegionOps ledc_ops = {
    .read = ledc_read,
    .write = ledc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void ledc_reset_hold(Object *obj, ResetType type)
{
    ArtemisLedcState *s = ARTEMIS_LEDC(obj);

    memset(s->ch_conf0, 0, sizeof(s->ch_conf0));
    memset(s->ch_hpoint, 0, sizeof(s->ch_hpoint));
    memset(s->ch_duty, 0, sizeof(s->ch_duty));
    memset(s->ch_conf1, 0, sizeof(s->ch_conf1));
    memset(s->ch_duty_out, 0, sizeof(s->ch_duty_out));
    memset(s->ch_fading, 0, sizeof(s->ch_fading));
    memset(s->timer_conf, 0, sizeof(s->timer_conf));
    memset(s->timer_active, 0, sizeof(s->timer_active));
    s->int_raw = 0;
    s->int_ena = 0;
    s->conf = 0;
    timer_del(s->fade_timer);
}

static void ledc_init(Object *obj)
{
    ArtemisLedcState *s = ARTEMIS_LEDC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &ledc_ops, s, "artemis.ledc", 0x100);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->fade_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, fade_tick, s);
}

static void ledc_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = ledc_reset_hold;
}

static const TypeInfo ledc_info = {
    .name = TYPE_ARTEMIS_LEDC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ArtemisLedcState),
    .instance_init = ledc_init,
    .class_init = ledc_class_init,
};

static void ledc_register_types(void)
{
    type_register_static(&ledc_info);
}

type_init(ledc_register_types)
