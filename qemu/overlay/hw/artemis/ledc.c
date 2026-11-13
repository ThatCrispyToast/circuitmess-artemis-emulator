/*
 * ESP32-S3 LED PWM controller: eight low-speed channels and four timers.
 *
 * Timers run against virtual time, so their counters, the instantaneous level
 * of each output and the point at which a new duty takes effect (the next
 * overflow, as on the chip) all follow from the clock. The board is told each
 * channel's duty and frequency rather than every edge.
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
#define CONF1_RESET         CONF1_INC
#define TIMER_RES_M         0xF
#define TIMER_DIV_S         4
#define TIMER_DIV_M         0x3FFFF
#define TIMER_PAUSE         BIT(22)
#define TIMER_RST           BIT(23)
#define TIMER_PARA_UP       BIT(25)
#define TIMER_CONF_RESET    TIMER_RST
#define INT_OVF_M           0xF
#define INT_FADE_END_S      4
#define LEDC_DATE           0x19040200

/* Overflow interrupts faster than this would only starve the host. */
#define MIN_OVF_IRQ_NS      (20 * SCALE_US)

static int64_t now_ns(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

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

/* One full count of a timer; zero while it has no clock or divider. */
static uint64_t timer_period_ns(ArtemisLedcState *s, int t)
{
    uint32_t conf = s->timer_active[t];
    uint32_t div = conf >> TIMER_DIV_S & TIMER_DIV_M;
    uint32_t res = conf & TIMER_RES_M;
    uint64_t hz = clock_hz(s);

    if (!div || !hz) {
        return 0;
    }
    /* div carries eight fractional bits. */
    return ((uint64_t)div << res) * NANOSECONDS_PER_SECOND / (hz << 8);
}

static bool timer_counting(ArtemisLedcState *s, int t)
{
    return !(s->timer_conf[t] & (TIMER_RST | TIMER_PAUSE)) &&
           timer_period_ns(s, t) > 0;
}

static uint32_t timer_count(ArtemisLedcState *s, int t)
{
    uint64_t period = timer_period_ns(s, t);
    uint32_t res = s->timer_active[t] & TIMER_RES_M;

    if (s->timer_conf[t] & TIMER_RST || !period) {
        return 0;
    }
    if (s->timer_conf[t] & TIMER_PAUSE) {
        return s->timer_paused_count[t];
    }
    uint64_t phase = (uint64_t)(now_ns() - s->timer_start[t]) % period;

    return (phase << res) / period;
}

static int64_t timer_next_overflow(ArtemisLedcState *s, int t)
{
    uint64_t period = timer_period_ns(s, t);
    int64_t now = now_ns();

    if (!timer_counting(s, t)) {
        return now;
    }
    return now + period - (uint64_t)(now - s->timer_start[t]) % period;
}

static void timer_restart(ArtemisLedcState *s, int t)
{
    s->timer_start[t] = now_ns();
    s->timer_ovf_seen[t] = s->timer_start[t];
}

static int channel_timer(ArtemisLedcState *s, int ch)
{
    return s->ch_conf0[ch] & CONF0_TIMER_SEL_M;
}

void artemis_ledc_channel_output(ArtemisLedcState *s, int ch, uint32_t *duty,
                                 uint32_t *freq)
{
    int t = channel_timer(s, ch);
    uint32_t res = s->timer_active[t] & TIMER_RES_M;

    if (!(s->ch_conf0[ch] & CONF0_SIG_OUT_EN)) {
        *duty = (s->ch_conf0[ch] & CONF0_IDLE_LV) ? 0xFFFF : 0;
        *freq = 0;
        return;
    }

    uint64_t full = (uint64_t)1 << (res + 4);
    uint64_t on = MIN(s->ch_duty_out[ch], full);
    uint64_t period = timer_period_ns(s, t);

    *duty = on * 0xFFFF / full;
    *freq = period ? NANOSECONDS_PER_SECOND / period : 0;
}

bool artemis_ledc_channel_level(ArtemisLedcState *s, int ch)
{
    int t = channel_timer(s, ch);
    uint32_t res = s->timer_active[t] & TIMER_RES_M;
    uint32_t full = 1u << res;

    if (!(s->ch_conf0[ch] & CONF0_SIG_OUT_EN)) {
        return s->ch_conf0[ch] & CONF0_IDLE_LV;
    }

    uint32_t on = MIN(s->ch_duty_out[ch] >> 4, full);
    uint32_t start = s->ch_hpoint_out[ch] & (full - 1);
    uint32_t count = timer_count(s, t);

    /* High from hpoint for `on` counts, wrapping with the counter. */
    return ((count - start) & (full - 1)) < on;
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

/* Overflows are only tracked when asked about, unless their interrupt is on. */
static void collect_overflows(ArtemisLedcState *s)
{
    int64_t now = now_ns();

    for (int t = 0; t < 4; t++) {
        uint64_t period = timer_period_ns(s, t);

        if (!timer_counting(s, t)) {
            continue;
        }
        if ((uint64_t)(now - s->timer_start[t]) / period !=
            (uint64_t)(s->timer_ovf_seen[t] - s->timer_start[t]) / period) {
            s->int_raw |= BIT(t);
        }
        s->timer_ovf_seen[t] = now;
    }
}

static void schedule(ArtemisLedcState *s)
{
    int64_t next = INT64_MAX;

    for (int i = 0; i < ARTEMIS_LEDC_CHANNELS; i++) {
        if (s->ch_fading[i]) {
            next = MIN(next, s->ch_fade_next[i]);
        }
        if (s->ch_latch_pending[i]) {
            next = MIN(next, s->ch_latch_at[i]);
        }
    }
    for (int t = 0; t < 4; t++) {
        if ((s->int_ena & BIT(t)) && timer_counting(s, t)) {
            next = MIN(next, MAX(timer_next_overflow(s, t),
                                 s->timer_ovf_seen[t] + MIN_OVF_IRQ_NS));
        }
    }
    if (next != INT64_MAX) {
        timer_mod(s->timer, next);
    } else {
        timer_del(s->timer);
    }
}

static int64_t fade_step_ns(ArtemisLedcState *s, int ch)
{
    uint32_t cycles = s->ch_conf1[ch] >> CONF1_CYCLE_S & CONF1_CYCLE_M;
    uint64_t period = timer_period_ns(s, channel_timer(s, ch));

    /* A floor keeps pathological configurations from monopolising the host. */
    return MAX((int64_t)(MAX(cycles, 1) * MAX(period, 1)), 100 * SCALE_US);
}

/* The timer overflowed with new parameters waiting: they become the output. */
static void channel_latch(ArtemisLedcState *s, int ch)
{
    uint32_t conf1 = s->ch_conf1[ch];
    uint32_t num = conf1 >> CONF1_NUM_S & CONF1_NUM_M;
    uint32_t scale = conf1 & CONF1_SCALE_M;

    s->ch_latch_pending[ch] = false;
    s->ch_hpoint_out[ch] = s->ch_hpoint[ch];

    if (!(conf1 & CONF1_START)) {
        return;
    }

    s->ch_duty_out[ch] = s->ch_duty[ch];

    if (num > 0 && scale > 0) {
        s->ch_fading[ch] = true;
        s->ch_fade_next[ch] = now_ns() + fade_step_ns(s, ch);
    } else {
        s->ch_conf1[ch] &= ~CONF1_START;
        s->int_raw |= BIT(INT_FADE_END_S + ch);
    }
}

static bool fade_step(ArtemisLedcState *s, int ch)
{
    uint32_t scale = s->ch_conf1[ch] & CONF1_SCALE_M;
    uint32_t num = s->ch_conf1[ch] >> CONF1_NUM_S & CONF1_NUM_M;

    if (s->ch_conf1[ch] & CONF1_INC) {
        s->ch_duty_out[ch] += scale << 4;
    } else {
        uint32_t step = scale << 4;

        s->ch_duty_out[ch] = s->ch_duty_out[ch] > step ?
                             s->ch_duty_out[ch] - step : 0;
    }

    if (num <= 1) {
        s->ch_conf1[ch] &= ~(CONF1_START | (CONF1_NUM_M << CONF1_NUM_S));
        s->ch_fading[ch] = false;
        s->int_raw |= BIT(INT_FADE_END_S + ch);
    } else {
        s->ch_conf1[ch] = (s->ch_conf1[ch] & ~(CONF1_NUM_M << CONF1_NUM_S)) |
                          ((num - 1) << CONF1_NUM_S);
        s->ch_fade_next[ch] += fade_step_ns(s, ch);
    }
    return true;
}

static void ledc_tick(void *opaque)
{
    ArtemisLedcState *s = ARTEMIS_LEDC(opaque);
    int64_t now = now_ns();
    bool changed = false;

    for (int i = 0; i < ARTEMIS_LEDC_CHANNELS; i++) {
        if (s->ch_latch_pending[i] && s->ch_latch_at[i] <= now) {
            channel_latch(s, i);
            changed = true;
        }
        while (s->ch_fading[i] && s->ch_fade_next[i] <= now) {
            changed |= fade_step(s, i);
        }
    }

    collect_overflows(s);
    schedule(s);
    update_irq(s);
    if (changed) {
        outputs_changed(s);
    }
}

/* PARA_UP: the shadow registers apply when the channel's timer next overflows. */
static void channel_para_up(ArtemisLedcState *s, int ch)
{
    int t = channel_timer(s, ch);

    s->ch_fading[ch] = false;
    if (timer_counting(s, t)) {
        s->ch_latch_pending[ch] = true;
        s->ch_latch_at[ch] = timer_next_overflow(s, t);
    } else {
        channel_latch(s, ch);
    }
    schedule(s);
    update_irq(s);
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
        return timer_count(s, t);
    }

    switch (addr) {
    case A_INT_RAW:
        collect_overflows(s);
        return s->int_raw;
    case A_INT_ST:
        collect_overflows(s);
        return s->int_raw & s->int_ena;
    case A_INT_ENA:
        return s->int_ena;
    case A_CONF:
        return s->conf;
    case A_DATE:
        return LEDC_DATE;
    default:
        return 0;
    }
}

static void timer_conf_write(ArtemisLedcState *s, int t, uint32_t v)
{
    uint32_t old = s->timer_conf[t];

    if ((v & TIMER_PAUSE) && !(old & TIMER_PAUSE)) {
        s->timer_paused_count[t] = timer_count(s, t);
    }
    s->timer_conf[t] = v & ~TIMER_PARA_UP;

    /* The divider and resolution only take effect on PARA_UP. */
    if (v & TIMER_PARA_UP &&
        s->timer_active[t] != (s->timer_conf[t] & (TIMER_RES_M | TIMER_DIV_M << TIMER_DIV_S))) {
        s->timer_active[t] = s->timer_conf[t] & (TIMER_RES_M | TIMER_DIV_M << TIMER_DIV_S);
        timer_restart(s, t);
    }
    if ((old & (TIMER_RST | TIMER_PAUSE)) && !(v & (TIMER_RST | TIMER_PAUSE))) {
        timer_restart(s, t);
    }

    /* A latch waiting on this timer follows its new schedule. */
    for (int i = 0; i < ARTEMIS_LEDC_CHANNELS; i++) {
        if (s->ch_latch_pending[i] && channel_timer(s, i) == t) {
            s->ch_latch_at[i] = timer_next_overflow(s, t);
        }
    }
    schedule(s);
    outputs_changed(s);
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
            timer_conf_write(s, (addr - A_TIMER0_CONF) / 8, v);
        }
        return;
    }

    switch (addr) {
    case A_INT_ENA:
        collect_overflows(s);
        s->int_ena = v;
        schedule(s);
        update_irq(s);
        break;
    case A_INT_CLR:
        collect_overflows(s);
        s->int_raw &= ~v;
        update_irq(s);
        break;
    case A_CONF:
        if ((s->conf ^ v) & 3) {
            s->conf = v;
            for (int t = 0; t < 4; t++) {
                timer_restart(s, t);
            }
        }
        s->conf = v;
        schedule(s);
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
    memset(s->ch_hpoint_out, 0, sizeof(s->ch_hpoint_out));
    memset(s->ch_duty, 0, sizeof(s->ch_duty));
    memset(s->ch_duty_out, 0, sizeof(s->ch_duty_out));
    memset(s->ch_fading, 0, sizeof(s->ch_fading));
    memset(s->ch_latch_pending, 0, sizeof(s->ch_latch_pending));
    memset(s->timer_active, 0, sizeof(s->timer_active));
    memset(s->timer_paused_count, 0, sizeof(s->timer_paused_count));
    for (int i = 0; i < ARTEMIS_LEDC_CHANNELS; i++) {
        s->ch_conf1[i] = CONF1_RESET;
    }
    for (int t = 0; t < 4; t++) {
        s->timer_conf[t] = TIMER_CONF_RESET;
        s->timer_start[t] = 0;
        s->timer_ovf_seen[t] = 0;
    }
    s->int_raw = 0;
    s->int_ena = 0;
    s->conf = 0;
    timer_del(s->timer);
}

static void ledc_init(Object *obj)
{
    ArtemisLedcState *s = ARTEMIS_LEDC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &ledc_ops, s, "artemis.ledc", 0x100);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ledc_tick, s);
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
