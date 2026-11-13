/*
 * ESP32-S3 I2S: its clocking and its DMA side, so audio code keeps its timing.
 * A transmitter takes its DMA buffers at the rate the dividers give and reports
 * each one sent; a receiver hands back silence at that rate. No pin on the watch
 * carries I2S, so the samples themselves go nowhere and none come in.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/host-utils.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/artemis/artemis.h"

#define A_INT_RAW         0x0C
#define A_INT_ST          0x10
#define A_INT_ENA         0x14
#define A_INT_CLR         0x18
#define A_RX_CONF         0x20
#define A_TX_CONF         0x24
#define A_RX_CONF1        0x28
#define A_TX_CONF1        0x2C
#define A_RX_CLKM_CONF    0x30
#define A_TX_CLKM_CONF    0x34
#define A_RX_CLKM_DIV     0x38
#define A_TX_CLKM_DIV     0x3C
#define A_TX_PCM2PDM1     0x44
#define A_RX_TDM_CTRL     0x50
#define A_TX_TDM_CTRL     0x54
#define A_STATE           0x6C

#define INT_TX_DONE       BIT(1)

/* The same places in the receiver's registers and the transmitter's. */
#define CONF_RESET        BIT(0)
#define CONF_START        BIT(2)
#define CONF_SLAVE        BIT(3)
#define CONF_MONO         BIT(5)
#define CONF_24_FILL      BIT(16)
#define CONF_PDM          BIT(20)
#define RX_CONF_PDM_DSR16 BIT(22)

#define CONF1_BCK_DIV(v)      ((v) >> 7 & 0x3F)
#define CONF1_SAMPLE_BITS(v)  (((v) >> 13 & 0x1F) + 1)
#define CONF1_SLOT_BITS(v)    (((v) >> 24 & 0x1F) + 1)

#define CLKM_DIV_NUM(v)   ((v) & 0xFF)
#define CLKM_ACTIVE       BIT(26)
#define CLKM_SEL(v)       ((v) >> 27 & 3)
#define CLKM_DIV_Z(v)     ((v) & 0x1FF)
#define CLKM_DIV_Y(v)     ((v) >> 9 & 0x1FF)
#define CLKM_DIV_X(v)     ((v) >> 18 & 0x1FF)
#define CLKM_DIV_YN1      BIT(27)

#define TDM_SLOTS(v)      (((v) >> 16 & 0xF) + 1)
#define TX_TDM_SKIP_MASK  BIT(20)

#define STATE_TX_IDLE     BIT(0)

/* How soon a side that's started, but has no clock or no buffer, looks again. */
#define IDLE_POLL_NS      SCALE_MS

static void update_irq(ArtemisI2sState *s)
{
    qemu_set_irq(s->irq, !!(s->regs[A_INT_RAW / 4] & s->regs[A_INT_ENA / 4]));
}

/* Bytes a second through the DMA buffers; 0 if this side has no clock. */
static double byte_rate(ArtemisI2sState *s, bool tx)
{
    static const double source_hz[] = { 40e6, 240e6, 160e6, 0 };
    uint32_t conf = s->regs[(tx ? A_TX_CONF : A_RX_CONF) / 4];
    uint32_t conf1 = s->regs[(tx ? A_TX_CONF1 : A_RX_CONF1) / 4];
    uint32_t clkm = s->regs[(tx ? A_TX_CLKM_CONF : A_RX_CLKM_CONF) / 4];
    uint32_t div = s->regs[(tx ? A_TX_CLKM_DIV : A_RX_CLKM_DIV) / 4];
    uint32_t tdm = s->regs[(tx ? A_TX_TDM_CTRL : A_RX_TDM_CTRL) / 4];
    uint32_t slots = TDM_SLOTS(tdm);
    uint32_t bits = CONF1_SAMPLE_BITS(conf1);
    uint32_t bytes = bits <= 8 ? 1 : bits <= 16 ? 2 : bits <= 24 && !(conf & CONF_24_FILL) ? 3 : 4;
    uint32_t samples;
    double divider = CLKM_DIV_NUM(clkm) ? CLKM_DIV_NUM(clkm) : 256;
    double bclk, frames;

    /* A slave's clock comes in on a pin, and nothing is there to give one. */
    if (conf & CONF_SLAVE || !(clkm & CLKM_ACTIVE)) {
        return 0;
    }

    if (CLKM_DIV_Z(div)) {
        double part = (double)CLKM_DIV_Z(div) /
                      ((CLKM_DIV_X(div) + 1) * CLKM_DIV_Z(div) + CLKM_DIV_Y(div));

        divider += div & CLKM_DIV_YN1 ? 1 - part : part;
    }
    bclk = source_hz[CLKM_SEL(clkm)] / divider / (CONF1_BCK_DIV(conf1) + 1);

    if (conf & CONF_PDM) {
        /* The bit clock runs 64 times the sample rate, times the up or down sampling. */
        if (tx) {
            uint32_t pdm = s->regs[A_TX_PCM2PDM1 / 4];
            uint32_t fp = pdm & 0x3FF, fs = pdm >> 10 & 0x3FF;

            frames = fp ? bclk * fs / (64.0 * fp) : 0;
        } else {
            frames = bclk / (conf & RX_CONF_PDM_DSR16 ? 128 : 64);
        }
        samples = MIN(slots, 2);
    } else {
        frames = bclk / (slots * CONF1_SLOT_BITS(conf1));
        /* Buffers carry the enabled slots only, unless the transmitter is told to skip. */
        samples = tx && (tdm & TX_TDM_SKIP_MASK) ? slots : ctpop32(tdm & (BIT(slots) - 1));
    }
    if (conf & CONF_MONO) {
        samples = 1;
    }
    return frames * samples * bytes;
}

static bool dma_channel(ArtemisI2sState *s, int dir, uint32_t *chan)
{
    ESPGdmaClass *klass = ESP_GDMA_GET_CLASS(s->gdma);

    for (uint32_t i = 0; i < klass->m_channel_count; i++) {
        if ((s->gdma->ch_conf[dir][i].peripheral & 0x3F) == s->gdma_periph) {
            *chan = i;
            return true;
        }
    }
    return false;
}

/* When the buffer taken now will be through, given when the last one was. */
static void schedule(ArtemisI2sSide *side, int64_t now, bool carried_on, int bytes, double rate)
{
    side->due = (carried_on ? side->due : now) + (int64_t)(bytes / rate * NANOSECONDS_PER_SECOND);
    timer_mod(side->timer, side->due);
}

static void wait_for_work(ArtemisI2sSide *side, int64_t now)
{
    side->due = now + IDLE_POLL_NS;
    timer_mod(side->timer, side->due);
}

static void tx_step(void *opaque)
{
    ArtemisI2sState *s = opaque;
    ArtemisI2sSide *side = &s->tx;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool carried_on = side->in_flight;
    double rate = byte_rate(s, true);
    int length;

    if (side->in_flight) {
        side->in_flight = false;
        esp_gdma_stream_out_done(s->gdma, side->chan);
    }
    if (!(s->regs[A_TX_CONF / 4] & CONF_START)) {
        return;
    }
    if (rate <= 0 || !dma_channel(s, ESP_GDMA_OUT_IDX, &side->chan)) {
        wait_for_work(side, now);
        return;
    }
    length = esp_gdma_stream_out(s->gdma, side->chan, s->buffer, sizeof(s->buffer));
    if (length <= 0) {
        wait_for_work(side, now);
        return;
    }
    side->in_flight = true;
    schedule(side, now, carried_on, length, rate);
}

static void rx_step(void *opaque)
{
    ArtemisI2sState *s = opaque;
    ArtemisI2sSide *side = &s->rx;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool carried_on = side->in_flight;
    double rate = byte_rate(s, false);
    int room;

    if (side->in_flight) {
        side->in_flight = false;
        memset(s->buffer, 0, side->bytes);
        esp_gdma_stream_in(s->gdma, side->chan, s->buffer, side->bytes);
    }
    if (!(s->regs[A_RX_CONF / 4] & CONF_START)) {
        return;
    }
    if (rate <= 0 || !dma_channel(s, ESP_GDMA_IN_IDX, &side->chan)) {
        wait_for_work(side, now);
        return;
    }
    room = esp_gdma_stream_in_room(s->gdma, side->chan);
    if (room <= 0) {
        wait_for_work(side, now);
        return;
    }
    side->in_flight = true;
    side->bytes = MIN(room, sizeof(s->buffer));
    schedule(side, now, carried_on, side->bytes, rate);
}

static void conf_write(ArtemisI2sState *s, ArtemisI2sSide *side, hwaddr addr, uint32_t v)
{
    uint32_t before = s->regs[addr / 4];

    s->regs[addr / 4] = (before & ~s->writable[addr / 4]) | (v & s->writable[addr / 4]);

    if (v & CONF_RESET || (before & ~v & CONF_START)) {
        /* What was under way is dropped, and firmware never hears of it. */
        timer_del(side->timer);
        side->in_flight = false;
    }
    if (v & ~before & CONF_START) {
        timer_mod(side->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}

static uint64_t i2s_read(void *opaque, hwaddr addr, unsigned size)
{
    ArtemisI2sState *s = ARTEMIS_I2S(opaque);

    switch (addr) {
    case A_INT_ST:
        return s->regs[A_INT_RAW / 4] & s->regs[A_INT_ENA / 4];
    case A_INT_CLR:
        return 0;
    case A_STATE:
        return s->regs[A_TX_CONF / 4] & CONF_START ? 0 : STATE_TX_IDLE;
    default:
        return addr / 4 < ARRAY_SIZE(s->regs) ? s->regs[addr / 4] : 0;
    }
}

static void i2s_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ArtemisI2sState *s = ARTEMIS_I2S(opaque);
    uint32_t v = value;

    switch (addr) {
    case A_INT_RAW:
    case A_INT_ST:
    case A_STATE:
        break;
    case A_INT_CLR:
        s->regs[A_INT_RAW / 4] &= ~v;
        update_irq(s);
        break;
    case A_INT_ENA:
        s->regs[addr / 4] = v & s->writable[addr / 4];
        update_irq(s);
        break;
    case A_RX_CONF:
        conf_write(s, &s->rx, addr, v);
        break;
    case A_TX_CONF:
        conf_write(s, &s->tx, addr, v);
        break;
    default:
        if (addr / 4 < ARRAY_SIZE(s->regs)) {
            uint32_t writable = s->writable[addr / 4];

            s->regs[addr / 4] = (s->regs[addr / 4] & ~writable) | (v & writable);
        }
        break;
    }
}

static const MemoryRegionOps i2s_ops = {
    .read = i2s_read,
    .write = i2s_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void i2s_reset_hold(Object *obj, ResetType type)
{
    ArtemisI2sState *s = ARTEMIS_I2S(obj);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->writable, 0, sizeof(s->writable));
    artemis_socregs_describe(s->base, s->regs, s->writable, ARRAY_SIZE(s->regs));
    /* As read from a watch: the transmitter counts as done before it has sent anything. */
    s->regs[A_INT_RAW / 4] = INT_TX_DONE;
    timer_del(s->tx.timer);
    timer_del(s->rx.timer);
    s->tx.in_flight = false;
    s->rx.in_flight = false;
    update_irq(s);
}

static void i2s_init(Object *obj)
{
    ArtemisI2sState *s = ARTEMIS_I2S(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &i2s_ops, s, "artemis.i2s", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->tx.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tx_step, s);
    s->rx.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rx_step, s);
}

static Property i2s_properties[] = {
    DEFINE_PROP_UINT32("base", ArtemisI2sState, base, 0x6000F000),
    DEFINE_PROP_UINT32("gdma-periph", ArtemisI2sState, gdma_periph, GDMA_I2S0),
    DEFINE_PROP_END_OF_LIST(),
};

static void i2s_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = i2s_reset_hold;
    device_class_set_props(DEVICE_CLASS(klass), i2s_properties);
}

static const TypeInfo i2s_info = {
    .name = TYPE_ARTEMIS_I2S,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ArtemisI2sState),
    .instance_init = i2s_init,
    .class_init = i2s_class_init,
};

static void i2s_register_types(void)
{
    type_register_static(&i2s_info);
}

type_init(i2s_register_types)
