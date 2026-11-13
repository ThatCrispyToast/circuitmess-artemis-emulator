/*
 * RivieraWaves BLE core as the ESP32-S3 controller library drives it, minus the
 * radio: the slot clock, the three programmable timers, deep sleep with soft
 * wakeup, the interrupt block, and the exchange table's event queue.
 *
 * That's enough for the real controller firmware to run, answer HCI and schedule
 * its own work. Without a radio, every event it queues gets skipped when it comes
 * due, the way real hardware skips an event it can't start: nothing goes on the
 * air, so the watch looks like a device that nothing happens to connect to.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "qemu/bswap.h"
#include "exec/address-spaces.h"
#include "hw/irq.h"
#include "hw/artemis/artemis.h"

#define R_RWBLECNTL   0x000
#define R_VERSION     0x004
#define R_INTCNTL     0x00C
#define R_INTSTAT     0x010
#define R_INTRAWSTAT  0x014
#define R_INTACK      0x018
#define R_SLOTCLK     0x01C
#define R_FINETIMECNT 0x020
#define R_TGT_10MS    0x0E4
#define R_TGT_HS      0x0E8
#define R_TGT_HUS_HS  0x0EC
#define R_TGT_HUS_FINE 0x0F0
#define R_ACTSCHCNTL  0x100
#define R_EM_PAGE0    0x204
#define R_ACTFIFOSTAT 0x2D8

#define RWBLE_VERSION 0x09001B00

#define RWBLECNTL_RESET_BITS (BIT(31) | BIT(30) | BIT(29))

#define SLOTCLK_SAMP  BIT(31)
#define SLOTCLK_SET   BIT(30)
#define SLOTCLK_MASK  0x0FFFFFFF

/*
 * The ISR takes its work from an activity FIFO instead of INTSTAT: an entry holds
 * interrupt bits from bit 10 up, the number of entries sits at bit 5, and writing
 * bit 0 pops the current entry.
 */
#define ACTFIFO_POP       BIT(0)
#define ACTFIFO_COUNT_S   5
#define ACTFIFO_IRQ_S     10
#define ACTFIFO_IRQ_M     0x1FFFFF
#define ACTFIFO_RESET_VAL 0x8000001E

#define INT_SLOT      BIT(0)
#define INT_SLP       BIT(3)
#define INT_SKIP      BIT(6)
#define INT_TGT_10MS  BIT(9)
#define INT_TGT_HS    BIT(10)
#define INT_TGT_HUS   BIT(11)

/* Writing an entry's index here with the start bit set queues it. */
#define ACTSCH_START  BIT(31)
#define ACTSCH_IDX_M  0xF

/*
 * The exchange table opens exchange memory, which lives in internal RAM. Each entry
 * starts with a status field and the start time in half-slots.
 */
#define ET_ENTRY_BYTES 16
#define ET_STATUS_S    3
#define ET_STATUS_M    (7 << ET_STATUS_S)
#define ET_STATUS_SKIP 6
#define ET_START_HI_M  0xFFF
#define EM_PAGE_ADDR_M 0x3FFFF
#define EM_RAM_BASE    0x3FC00000

/* Sleep block */
#define R_DEEPSLCNTL  0x00
#define R_DEEPSLWKUP  0x04
#define R_DEEPSLSTAT  0x08

#define DEEPSL_ON          BIT(1)
#define DEEPSL_SOFT_WAKEUP BIT(4)
#define DEEPSL_STAT        BIT(15)

/* One half-slot is 312.5 us, counted in half-microsecond steps down from 624. */
#define HALF_SLOT_NS  312500
#define FINE_STEP_NS  500
#define FINE_MAX      624
#define LP_CYCLE_NS   30518 /* 32.768 kHz */
#define WAKE_LATENCY_NS (500 * SCALE_US)
/* Keeps a target that's just been written from firing before it's unmasked. */
#define TARGET_MIN_NS (20 * SCALE_US)

struct ArtemisBleState {
    SysBusDevice parent_obj;

    MemoryRegion core_mem;
    MemoryRegion sleep_mem;
    qemu_irq irq;
    QEMUTimer *target_timer;
    QEMUTimer *wake_timer;
    QEMUTimer *slot_timer;
    QEMUTimer *event_timer;

    uint32_t core[0x400];
    uint32_t sleep[0x40];

    int64_t clock_zero_ns;
    uint32_t sampled_clock;
    uint32_t sampled_fine;
    uint32_t delivered;

    bool armed_10ms;
    bool armed_hs;
    bool armed_hus;

    /* Exchange table entries queued and not yet due. */
    uint16_t queued_events;

    bool sleeping;
    int64_t sleep_start_ns;
};

static int64_t now_ns(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

/* Activity events reach the FIFO whatever the mask register says. */
static uint32_t pending_irqs(ArtemisBleState *s)
{
    return s->core[R_INTRAWSTAT / 4] & (s->core[R_INTCNTL / 4] | INT_SKIP);
}

static void update_irq(ArtemisBleState *s)
{
    qemu_set_irq(s->irq, !!pending_irqs(s));
}

/* A raised event is new work for the FIFO, even if it fired once before. */
static void raise_int(ArtemisBleState *s, uint32_t bit)
{
    s->core[R_INTRAWSTAT / 4] |= bit;
    s->delivered &= ~bit;
}

static uint32_t clock_at(ArtemisBleState *s, int64_t ns)
{
    return ((ns - s->clock_zero_ns) / HALF_SLOT_NS) & SLOTCLK_MASK;
}

/* Absolute time of a half-slot target, allowing for the counter wrapping. */
static int64_t target_ns(ArtemisBleState *s, int64_t now, uint32_t hs, uint32_t fine)
{
    uint32_t delta = (hs - clock_at(s, now)) & SLOTCLK_MASK;

    if (delta >= (SLOTCLK_MASK >> 1)) {
        return now; /* already behind us */
    }

    int64_t hs_start = now - ((now - s->clock_zero_ns) % HALF_SLOT_NS);

    return hs_start + (int64_t)delta * HALF_SLOT_NS +
           (int64_t)(FINE_MAX - MIN(fine, FINE_MAX)) * FINE_STEP_NS;
}

/* The per-slot interrupt only exists while someone has it unmasked. */
static void schedule_slot_int(ArtemisBleState *s)
{
    if (s->core[R_INTCNTL / 4] & INT_SLOT) {
        int64_t now = now_ns();

        timer_mod(s->slot_timer,
                  now + HALF_SLOT_NS - (now - s->clock_zero_ns) % HALF_SLOT_NS);
    } else {
        timer_del(s->slot_timer);
    }
}

static void slot_tick(void *opaque)
{
    ArtemisBleState *s = ARTEMIS_BLE(opaque);

    raise_int(s, INT_SLOT);
    update_irq(s);
    schedule_slot_int(s);
}

static void schedule_targets(ArtemisBleState *s)
{
    int64_t now = now_ns();
    int64_t next = INT64_MAX;

    if (s->armed_10ms) {
        next = MIN(next, target_ns(s, now, s->core[R_TGT_10MS / 4] << 5, FINE_MAX));
    }
    if (s->armed_hs) {
        next = MIN(next, target_ns(s, now, s->core[R_TGT_HS / 4], FINE_MAX));
    }
    if (s->armed_hus) {
        next = MIN(next, target_ns(s, now, s->core[R_TGT_HUS_HS / 4],
                                   s->core[R_TGT_HUS_FINE / 4]));
    }

    if (next == INT64_MAX) {
        timer_del(s->target_timer);
    } else {
        timer_mod(s->target_timer, MAX(next, now + TARGET_MIN_NS));
    }
}

static void target_tick(void *opaque)
{
    ArtemisBleState *s = ARTEMIS_BLE(opaque);
    int64_t now = now_ns();

    if (s->armed_10ms && target_ns(s, now, s->core[R_TGT_10MS / 4] << 5, FINE_MAX) <= now) {
        s->armed_10ms = false;
        raise_int(s, INT_TGT_10MS);
    }
    if (s->armed_hs && target_ns(s, now, s->core[R_TGT_HS / 4], FINE_MAX) <= now) {
        s->armed_hs = false;
        raise_int(s, INT_TGT_HS);
    }
    if (s->armed_hus && target_ns(s, now, s->core[R_TGT_HUS_HS / 4],
                                  s->core[R_TGT_HUS_FINE / 4]) <= now) {
        s->armed_hus = false;
        raise_int(s, INT_TGT_HUS);
    }

    schedule_targets(s);
    update_irq(s);
}

static hwaddr event_addr(ArtemisBleState *s, int index)
{
    return (EM_RAM_BASE | (s->core[R_EM_PAGE0 / 4] & EM_PAGE_ADDR_M) << 2) +
           index * ET_ENTRY_BYTES;
}

static void read_event(ArtemisBleState *s, int index, int64_t now,
                       uint16_t *status, int64_t *due_ns)
{
    uint8_t entry[6];

    address_space_read(&address_space_memory, event_addr(s, index),
                       MEMTXATTRS_UNSPECIFIED, entry, sizeof(entry));
    *status = lduw_le_p(entry);
    *due_ns = target_ns(s, now, lduw_le_p(entry + 2) |
                                (lduw_le_p(entry + 4) & ET_START_HI_M) << 16, FINE_MAX);
}

static void schedule_events(ArtemisBleState *s)
{
    int64_t now = now_ns();
    int64_t next = INT64_MAX;

    for (int i = 0; i < 16; i++) {
        if (s->queued_events & BIT(i)) {
            uint16_t status;
            int64_t due;

            read_event(s, i, now, &status, &due);
            next = MIN(next, due);
        }
    }
    if (next == INT64_MAX) {
        timer_del(s->event_timer);
    } else {
        timer_mod(s->event_timer, next);
    }
}

static void event_tick(void *opaque)
{
    ArtemisBleState *s = ARTEMIS_BLE(opaque);
    int64_t now = now_ns();

    for (int i = 0; i < 16; i++) {
        if (s->queued_events & BIT(i)) {
            uint16_t status;
            int64_t due;

            read_event(s, i, now, &status, &due);
            if (due <= now) {
                uint8_t skipped[2];

                stw_le_p(skipped, (status & ~ET_STATUS_M) |
                                  ET_STATUS_SKIP << ET_STATUS_S);
                address_space_write(&address_space_memory, event_addr(s, i),
                                    MEMTXATTRS_UNSPECIFIED, skipped, sizeof(skipped));
                s->queued_events &= ~BIT(i);
                raise_int(s, INT_SKIP);
            }
        }
    }
    schedule_events(s);
    update_irq(s);
}

static void wake_up(ArtemisBleState *s)
{
    timer_del(s->wake_timer);
    s->sleeping = false;
    s->sleep[R_DEEPSLSTAT / 4] = (now_ns() - s->sleep_start_ns) / LP_CYCLE_NS;
    s->sleep[R_DEEPSLCNTL / 4] &= ~(DEEPSL_ON | DEEPSL_SOFT_WAKEUP);
    raise_int(s, INT_SLP);
    update_irq(s);
}

static void wake_tick(void *opaque)
{
    wake_up(ARTEMIS_BLE(opaque));
}

static uint64_t core_read(void *opaque, hwaddr addr, unsigned size)
{
    ArtemisBleState *s = ARTEMIS_BLE(opaque);

    switch (addr) {
    case R_VERSION:
        return RWBLE_VERSION;
    case R_INTSTAT:
        return pending_irqs(s);
    case R_SLOTCLK:
        return s->sampled_clock;
    case R_FINETIMECNT:
        return s->sampled_fine;
    case R_ACTFIFOSTAT: {
        uint32_t bits = pending_irqs(s) & ~s->delivered & ACTFIFO_IRQ_M;

        return s->core[addr / 4] | (bits ? 1U << ACTFIFO_COUNT_S : 0) |
               bits << ACTFIFO_IRQ_S;
    }
    default:
        return s->core[addr / 4];
    }
}

static void core_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ArtemisBleState *s = ARTEMIS_BLE(opaque);
    uint32_t v = value;

    switch (addr) {
    case R_RWBLECNTL:
        if (v & RWBLECNTL_RESET_BITS) {
            s->core[R_INTRAWSTAT / 4] = 0;
            s->core[R_INTCNTL / 4] = 0;
            s->armed_10ms = s->armed_hs = s->armed_hus = false;
            s->queued_events = 0;
            timer_del(s->target_timer);
            timer_del(s->slot_timer);
            timer_del(s->event_timer);
            update_irq(s);
        }
        s->core[addr / 4] = v & ~RWBLECNTL_RESET_BITS;
        break;
    case R_INTCNTL:
        s->core[addr / 4] = v;
        schedule_slot_int(s);
        update_irq(s);
        break;
    case R_INTACK:
        s->core[R_INTRAWSTAT / 4] &= ~v;
        s->delivered &= ~v;
        update_irq(s);
        break;
    case R_ACTFIFOSTAT:
        if (v & ACTFIFO_POP) {
            s->delivered |= pending_irqs(s);
        }
        /* Entry fields read back from live state rather than from storage. */
        s->core[addr / 4] = v & ~(ACTFIFO_POP | (0x1F << ACTFIFO_COUNT_S) |
                                  (uint32_t)ACTFIFO_IRQ_M << ACTFIFO_IRQ_S);
        break;
    case R_SLOTCLK:
        if (v & SLOTCLK_SET) {
            /* Move the counter to the written value without losing its phase. */
            int64_t now = now_ns();
            s->clock_zero_ns = now - (int64_t)(v & SLOTCLK_MASK) * HALF_SLOT_NS;
            schedule_targets(s);
            schedule_events(s);
        }
        if (v & SLOTCLK_SAMP) {
            int64_t elapsed = now_ns() - s->clock_zero_ns;

            s->sampled_clock = clock_at(s, now_ns());
            s->sampled_fine = FINE_MAX - (elapsed % HALF_SLOT_NS) / FINE_STEP_NS;
        }
        break;
    case R_ACTSCHCNTL:
        s->core[addr / 4] = v & ~ACTSCH_START;
        if (v & ACTSCH_START) {
            s->queued_events |= BIT(v & ACTSCH_IDX_M);
            schedule_events(s);
        }
        break;
    case R_TGT_10MS:
        s->core[addr / 4] = v;
        s->armed_10ms = true;
        schedule_targets(s);
        break;
    case R_TGT_HS:
        s->core[addr / 4] = v;
        s->armed_hs = true;
        schedule_targets(s);
        break;
    case R_TGT_HUS_HS:
        s->core[addr / 4] = v;
        s->armed_hus = true;
        schedule_targets(s);
        break;
    case R_TGT_HUS_FINE:
        s->core[addr / 4] = v;
        schedule_targets(s);
        break;
    default:
        s->core[addr / 4] = v;
        break;
    }
}

static const MemoryRegionOps core_ops = {
    .read = core_read,
    .write = core_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static uint64_t sleep_read(void *opaque, hwaddr addr, unsigned size)
{
    ArtemisBleState *s = ARTEMIS_BLE(opaque);

    if (addr == R_DEEPSLCNTL) {
        return s->sleep[0] | (s->sleeping ? DEEPSL_STAT : 0);
    }
    return s->sleep[addr / 4];
}

static void sleep_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ArtemisBleState *s = ARTEMIS_BLE(opaque);
    uint32_t v = value;

    if (addr != R_DEEPSLCNTL) {
        s->sleep[addr / 4] = v;
        return;
    }
    s->sleep[R_DEEPSLCNTL / 4] = v & ~DEEPSL_STAT;

    if (v & DEEPSL_ON && !s->sleeping) {
        s->sleeping = true;
        s->sleep_start_ns = now_ns();
        timer_mod(s->wake_timer, s->sleep_start_ns +
                  (int64_t)s->sleep[R_DEEPSLWKUP / 4] * LP_CYCLE_NS);
    }
    if (v & DEEPSL_SOFT_WAKEUP && s->sleeping) {
        timer_mod(s->wake_timer, now_ns() + WAKE_LATENCY_NS);
    }
}

static const MemoryRegionOps sleep_ops = {
    .read = sleep_read,
    .write = sleep_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void ble_reset_hold(Object *obj, ResetType type)
{
    ArtemisBleState *s = ARTEMIS_BLE(obj);

    timer_del(s->target_timer);
    timer_del(s->wake_timer);
    timer_del(s->slot_timer);
    timer_del(s->event_timer);
    s->queued_events = 0;
    memset(s->core, 0, sizeof(s->core));
    memset(s->sleep, 0, sizeof(s->sleep));
    s->armed_10ms = s->armed_hs = s->armed_hus = false;
    s->sleeping = false;
    s->clock_zero_ns = now_ns();
    s->sampled_clock = 0;
    s->sampled_fine = FINE_MAX;
    s->delivered = 0;
    s->core[R_ACTFIFOSTAT / 4] = ACTFIFO_RESET_VAL;
}

static void ble_init(Object *obj)
{
    ArtemisBleState *s = ARTEMIS_BLE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->core_mem, obj, &core_ops, s, "artemis.ble.core", 0x1000);
    memory_region_init_io(&s->sleep_mem, obj, &sleep_ops, s, "artemis.ble.sleep", 0x100);
    sysbus_init_mmio(sbd, &s->core_mem);
    sysbus_init_mmio(sbd, &s->sleep_mem);
    sysbus_init_irq(sbd, &s->irq);
    s->target_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, target_tick, s);
    s->wake_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, wake_tick, s);
    s->slot_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, slot_tick, s);
    s->event_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, event_tick, s);
}

static void ble_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = ble_reset_hold;
}

static const TypeInfo ble_info = {
    .name = TYPE_ARTEMIS_BLE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ArtemisBleState),
    .instance_init = ble_init,
    .class_init = ble_class_init,
};

static void ble_register_types(void)
{
    type_register_static(&ble_info);
}

type_init(ble_register_types)
