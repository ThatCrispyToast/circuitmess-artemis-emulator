/*
 * ESP32-S3 remote control transceiver, transmit side: what NeoPixel drivers and
 * machine.RMT use. Items are taken from the channel's memory at the rate the
 * dividers give, with the threshold, loop and end interrupts raised when the
 * chip would raise them, so a driver refilling memory as it goes works too.
 * Nothing is wired to its outputs on the watch, so the waveform goes nowhere,
 * and the receive channels never hear anything.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/artemis/artemis.h"

#define RMT_BASE          0x60016000
#define A_CH0DATA         0x00
#define A_CH0CONF0        0x20
#define A_CH3CONF0        0x2C
#define A_CH0STATUS       0x50
#define A_INT_RAW         0x70
#define A_INT_ST          0x74
#define A_INT_ENA         0x78
#define A_INT_CLR         0x7C
#define A_CH0_TX_LIM      0xA0
#define A_SYS_CONF        0xC0
#define A_DATE            0xCC
#define A_MEM             0x800
#define RMT_DATE          0x2101181

#define CONF0_TX_START    BIT(0)
#define CONF0_MEM_RD_RST  BIT(1)
#define CONF0_APB_MEM_RST BIT(2)
#define CONF0_TX_CONTI    BIT(3)
#define CONF0_TX_STOP     BIT(7)
#define CONF0_DIV_S       8
#define CONF0_MEM_SIZE_S  16
#define CONF0_AFIFO_RST   BIT(23)
#define CONF0_CONF_UPDATE BIT(24)
#define CONF0_PULSES      (CONF0_TX_START | CONF0_MEM_RD_RST | CONF0_APB_MEM_RST | \
                           CONF0_TX_STOP | CONF0_AFIFO_RST | CONF0_CONF_UPDATE)

#define LIM_TX_LIM_M      0x1FF
#define LIM_LOOP_NUM_S    9
#define LIM_LOOP_NUM_M    0x3FF
#define LIM_LOOP_CNT_EN   BIT(19)
#define LIM_LOOP_RESET    BIT(20)

#define INT_TX_END(ch)    BIT(ch)
#define INT_TX_THR(ch)    BIT(8 + (ch))
#define INT_TX_LOOP(ch)   BIT(12 + (ch))

#define ITEM_DURATION_M   0x7FFF

/* Items handled between two looks at the clock, and the shortest stretch scheduled. */
#define MAX_ITEMS_PER_STEP  4096
#define MIN_STEP_NS         (2 * SCALE_US)

/*
 * A driver that refills memory on the threshold interrupt has, on the chip, a few
 * tens of microseconds to do it in, and makes it. An interrupt here can take
 * that long just to arrive, so the transmitter holds on for a refill that's
 * still owed rather than run on through stale items. It doesn't hold on forever.
 */
#define REFILL_WAIT_NS      (2 * SCALE_MS)

static void update_irq(ArtemisRmtState *s)
{
    qemu_set_irq(s->irq, !!(s->regs[A_INT_RAW / 4] & s->regs[A_INT_ENA / 4]));
}

static uint32_t tick_ns(ArtemisRmtState *s, int ch)
{
    uint32_t sys = s->regs[A_SYS_CONF / 4];
    uint32_t divider = s->regs[A_CH0CONF0 / 4 + ch] >> CONF0_DIV_S & 0xFF;
    uint64_t hz;

    switch (sys >> 24 & 3) {
    case 2:
        hz = 17500000;
        break;
    case 3:
        hz = 40000000;
        break;
    default:
        hz = 80000000;
        break;
    }
    hz /= (sys >> 4 & 0xFF) + 1;
    return NANOSECONDS_PER_SECOND * (divider ? divider : 256) / hz;
}

/* A channel's memory starts at its own block and may run on into the next ones. */
static uint32_t channel_items(ArtemisRmtState *s, int ch)
{
    uint32_t blocks = s->regs[A_CH0CONF0 / 4 + ch] >> CONF0_MEM_SIZE_S & 0xF;

    return CLAMP(blocks, 1, ARTEMIS_RMT_CHANNELS - ch) * ARTEMIS_RMT_BLOCK_ITEMS;
}

static uint32_t next_item(ArtemisRmtState *s, int ch)
{
    ArtemisRmtTx *tx = &s->tx[ch];
    uint32_t item = s->mem[ch * ARTEMIS_RMT_BLOCK_ITEMS + tx->read];

    tx->read = (tx->read + 1) % channel_items(s, ch);
    return item;
}

/*
 * Sends until something firmware should hear about, and sets the timer for when
 * that is: the end marker, or the threshold count of items having gone out.
 */
static void transmit(ArtemisRmtState *s, int ch)
{
    ArtemisRmtTx *tx = &s->tx[ch];
    uint32_t limit = s->regs[A_CH0_TX_LIM / 4 + ch] & LIM_TX_LIM_M;
    uint64_t ticks = 0;

    tx->event = 0;
    for (int n = 0; n < MAX_ITEMS_PER_STEP && !tx->event; n++) {
        uint32_t item = next_item(s, ch);
        uint32_t first = item & ITEM_DURATION_M;
        uint32_t second = item >> 16 & ITEM_DURATION_M;

        ticks += first;
        if (first && second) {
            ticks += second;
        }
        if (!first || !second) {
            tx->event = INT_TX_END(ch);
        } else if (limit && ++tx->sent % limit == 0) {
            tx->event = INT_TX_THR(ch);
        }
    }

    tx->busy = true;
    timer_mod(tx->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                         MAX(ticks * tick_ns(s, ch), MIN_STEP_NS));
}

static void tx_timer(void *opaque)
{
    ArtemisRmtTx *tx = opaque;
    ArtemisRmtState *s = tx->rmt;
    int ch = tx - s->tx;
    uint32_t conf = s->regs[A_CH0CONF0 / 4 + ch];
    uint32_t lim = s->regs[A_CH0_TX_LIM / 4 + ch];

    if (tx->event & INT_TX_THR(ch)) {
        bool owed = s->regs[A_INT_RAW / 4] & s->regs[A_INT_ENA / 4] & INT_TX_THR(ch);

        if (owed && !tx->waiting) {
            tx->waiting = true;
            timer_mod(tx->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + REFILL_WAIT_NS);
            return;
        }
    }
    tx->waiting = false;

    if (tx->event & INT_TX_END(ch)) {
        if (conf & CONF0_TX_CONTI) {
            /* Continuous mode starts over, counting the laps if asked to. */
            tx->read = 0;
            if (lim & LIM_LOOP_CNT_EN && ++tx->loops >= (lim >> LIM_LOOP_NUM_S & LIM_LOOP_NUM_M)) {
                tx->loops = 0;
                s->regs[A_INT_RAW / 4] |= INT_TX_LOOP(ch);
            }
            transmit(s, ch);
        } else {
            tx->busy = false;
            s->regs[A_INT_RAW / 4] |= INT_TX_END(ch);
        }
    } else {
        s->regs[A_INT_RAW / 4] |= tx->event;
        transmit(s, ch);
    }
    update_irq(s);
}

static void tx_conf_write(ArtemisRmtState *s, int ch, uint32_t v)
{
    ArtemisRmtTx *tx = &s->tx[ch];

    s->regs[A_CH0CONF0 / 4 + ch] = v & ~CONF0_PULSES;

    if (v & CONF0_MEM_RD_RST) {
        tx->read = 0;
    }
    if (v & CONF0_APB_MEM_RST) {
        tx->write = 0;
    }
    if (v & CONF0_TX_STOP && tx->busy) {
        timer_del(tx->timer);
        tx->busy = false;
        tx->waiting = false;
        s->regs[A_INT_RAW / 4] |= INT_TX_END(ch);
        update_irq(s);
    }
    if (v & CONF0_TX_START && !tx->busy) {
        tx->sent = 0;
        tx->loops = 0;
        transmit(s, ch);
    }
}

static uint64_t rmt_read(void *opaque, hwaddr addr, unsigned size)
{
    ArtemisRmtState *s = ARTEMIS_RMT(opaque);

    if (addr >= A_MEM && addr < A_MEM + sizeof(s->mem)) {
        return s->mem[(addr - A_MEM) / 4];
    }
    if (addr >= A_CH0STATUS && addr < A_CH0STATUS + 4 * 4) {
        /* Where the transmitter is reading from and firmware writing to. */
        int ch = (addr - A_CH0STATUS) / 4;
        uint32_t start = ch * ARTEMIS_RMT_BLOCK_ITEMS;

        return (start + s->tx[ch].read) | (start + s->tx[ch].write) << 11;
    }
    switch (addr) {
    case A_INT_ST:
        return s->regs[A_INT_RAW / 4] & s->regs[A_INT_ENA / 4];
    case A_INT_CLR:
        return 0;
    case A_DATE:
        return RMT_DATE;
    default:
        return addr / 4 < ARRAY_SIZE(s->regs) ? s->regs[addr / 4] : 0;
    }
}

static void rmt_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ArtemisRmtState *s = ARTEMIS_RMT(opaque);
    uint32_t v = value;

    if (addr >= A_MEM && addr < A_MEM + sizeof(s->mem)) {
        s->mem[(addr - A_MEM) / 4] = v;
        return;
    }
    if (addr >= A_CH0DATA && addr < A_CH0DATA + 4 * 4) {
        /* The other way in: each write lands on the next item. */
        int ch = (addr - A_CH0DATA) / 4;
        ArtemisRmtTx *tx = &s->tx[ch];

        s->mem[ch * ARTEMIS_RMT_BLOCK_ITEMS + tx->write] = v;
        tx->write = (tx->write + 1) % channel_items(s, ch);
        return;
    }
    if (addr >= A_CH0CONF0 && addr <= A_CH3CONF0) {
        tx_conf_write(s, (addr - A_CH0CONF0) / 4, v);
        return;
    }
    if (addr >= A_CH0_TX_LIM && addr < A_CH0_TX_LIM + 4 * 4) {
        if (v & LIM_LOOP_RESET) {
            s->tx[(addr - A_CH0_TX_LIM) / 4].loops = 0;
        }
        s->regs[addr / 4] = v & ~LIM_LOOP_RESET;
        return;
    }
    switch (addr) {
    case A_INT_RAW:
    case A_INT_ST:
        break;
    case A_INT_CLR:
        s->regs[A_INT_RAW / 4] &= ~v;
        update_irq(s);
        for (int ch = 0; ch < ARRAY_SIZE(s->tx); ch++) {
            if (s->tx[ch].waiting && (v & INT_TX_THR(ch))) {
                timer_mod(s->tx[ch].timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
            }
        }
        break;
    case A_INT_ENA:
        s->regs[addr / 4] = v;
        update_irq(s);
        break;
    default:
        if (addr / 4 < ARRAY_SIZE(s->regs)) {
            s->regs[addr / 4] = v;
        }
        break;
    }
}

static const MemoryRegionOps rmt_ops = {
    .read = rmt_read,
    .write = rmt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void rmt_reset_hold(Object *obj, ResetType type)
{
    ArtemisRmtState *s = ARTEMIS_RMT(obj);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->mem, 0, sizeof(s->mem));
    artemis_socregs_load(RMT_BASE, s->regs, ARRAY_SIZE(s->regs));
    for (int ch = 0; ch < ARRAY_SIZE(s->tx); ch++) {
        timer_del(s->tx[ch].timer);
        s->tx[ch].busy = false;
        s->tx[ch].waiting = false;
        s->tx[ch].read = 0;
        s->tx[ch].write = 0;
        s->tx[ch].sent = 0;
        s->tx[ch].loops = 0;
    }
}

static void rmt_init(Object *obj)
{
    ArtemisRmtState *s = ARTEMIS_RMT(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &rmt_ops, s, "artemis.rmt", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    for (int ch = 0; ch < ARRAY_SIZE(s->tx); ch++) {
        s->tx[ch].rmt = s;
        s->tx[ch].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tx_timer, &s->tx[ch]);
    }
}

static void rmt_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = rmt_reset_hold;
}

static const TypeInfo rmt_info = {
    .name = TYPE_ARTEMIS_RMT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ArtemisRmtState),
    .instance_init = rmt_init,
    .class_init = rmt_class_init,
};

static void rmt_register_types(void)
{
    type_register_static(&rmt_info);
}

type_init(rmt_register_types)
