/*
 * ESP32-S3 USB Serial/JTAG controller, seen from the firmware's side.
 *
 * The USB wire isn't modelled: IN endpoint data goes straight to a chardev and
 * chardev input appears on the OUT endpoint. This is the console both the stock
 * firmware and the CircuitMess MicroPython build use. A host always appears to
 * be attached, sending start-of-frame every millisecond, which is how newer
 * ESP-IDF decides whether anyone is listening.
 *
 * As on the chip, the host's next packet isn't accepted until firmware has read
 * the last one to the end, and the receive interrupt marks a packet's arrival
 * rather than data waiting: firmware that clears it without emptying the FIFO
 * hears nothing more.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/artemis/artemis.h"

#define A_EP1           0x00
#define A_EP1_CONF      0x04
#define A_INT_RAW       0x08
#define A_INT_ST        0x0C
#define A_INT_ENA       0x10
#define A_INT_CLR       0x14
#define A_CONF0         0x18
#define A_TEST          0x1C
#define A_JFIFO_ST      0x20
#define A_FRAM_NUM      0x24
#define A_IN_EP0_ST     0x28
#define A_IN_EP3_ST     0x34
#define A_OUT_EP0_ST    0x38
#define A_OUT_EP1_ST    0x3C
#define A_MISC_CONF     0x44
#define A_MEM_CONF      0x48
#define A_DATE          0x80

#define EP1_WR_DONE       BIT(0)
#define EP1_IN_FREE       BIT(1)
#define EP1_OUT_AVAIL     BIT(2)

#define INT_SOF           BIT(1)
#define INT_OUT_RECV_PKT  BIT(2)
#define INT_IN_EMPTY      BIT(3)
#define INT_IN_TOKEN_EP1  BIT(8)

#define FRAME_NS          SCALE_MS
#define FRAM_NUM_MASK     0x7FF

static uint64_t frames_now(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / FRAME_NS;
}

/* A start-of-frame has gone by since the flag was last cleared. */
static void collect_sof(ArtemisUsjState *s)
{
    uint64_t frames = frames_now();

    if (frames != s->sof_seen) {
        /* The host asks for input every frame as well. */
        s->sof_seen = frames;
        s->int_raw |= INT_SOF | INT_IN_TOKEN_EP1;
    }
}

static void update_irq(ArtemisUsjState *s)
{
    qemu_set_irq(s->irq, !!(s->int_raw & s->int_ena));
}

static void flush_tx(ArtemisUsjState *s)
{
    if (s->tx_count) {
        qemu_chr_fe_write_all(&s->chr, s->tx, s->tx_count);
        s->tx_count = 0;
    }
    s->int_raw |= INT_IN_EMPTY;
    update_irq(s);
}

static uint64_t usj_read(void *opaque, hwaddr addr, unsigned size)
{
    ArtemisUsjState *s = ARTEMIS_USJ(opaque);

    switch (addr) {
    case A_EP1:
        if (fifo8_is_empty(&s->rx_fifo)) {
            return 0;
        }
        {
            uint8_t c = fifo8_pop(&s->rx_fifo);
            if (fifo8_is_empty(&s->rx_fifo)) {
                /* The packet has been read: the host may send the next. */
                qemu_chr_fe_accept_input(&s->chr);
            }
            return c;
        }
    case A_EP1_CONF: {
        uint32_t v = 0;
        if (s->tx_count < ARTEMIS_USJ_FIFO_LEN) {
            v |= EP1_IN_FREE;
        }
        if (!fifo8_is_empty(&s->rx_fifo)) {
            v |= EP1_OUT_AVAIL;
        }
        return v;
    }
    case A_INT_RAW:
        collect_sof(s);
        return s->int_raw;
    case A_INT_ST:
        collect_sof(s);
        return s->int_raw & s->int_ena;
    case A_INT_ENA:
        return s->int_ena;
    case A_CONF0:
        return s->conf0;
    case A_TEST:
        return 0x40;
    case A_JFIFO_ST:
        return 0x44;
    case A_FRAM_NUM:
        return frames_now() & FRAM_NUM_MASK;
    case A_IN_EP0_ST ... A_IN_EP3_ST:
        return 1;
    case A_OUT_EP0_ST:
        return 0x1028;
    case A_OUT_EP1_ST:
        return 0x1020C;
    case A_MISC_CONF:
        return s->misc_conf;
    case A_MEM_CONF:
        return s->mem_conf;
    case A_DATE:
        return 0x2101200;
    default:
        return 0;
    }
}

static void usj_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ArtemisUsjState *s = ARTEMIS_USJ(opaque);

    switch (addr) {
    case A_EP1:
        if (s->tx_count < ARTEMIS_USJ_FIFO_LEN) {
            s->tx[s->tx_count++] = value;
        }
        if (s->tx_count == ARTEMIS_USJ_FIFO_LEN) {
            flush_tx(s);
        }
        break;
    case A_EP1_CONF:
        if (value & EP1_WR_DONE) {
            flush_tx(s);
        }
        break;
    case A_INT_ENA:
        s->int_ena = value;
        update_irq(s);
        break;
    case A_INT_CLR:
        collect_sof(s);
        s->int_raw &= ~(uint32_t)value;
        /* An empty transmit FIFO is a state, so its flag comes straight back. */
        if (s->tx_count == 0) {
            s->int_raw |= INT_IN_EMPTY;
        }
        update_irq(s);
        break;
    case A_CONF0:
        s->conf0 = value;
        break;
    case A_MISC_CONF:
        s->misc_conf = value;
        break;
    case A_MEM_CONF:
        s->mem_conf = value;
        break;
    }
}

static const MemoryRegionOps usj_ops = {
    .read = usj_read,
    .write = usj_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/* One packet at a time: nothing more until the last has been read out. */
static int chr_can_receive(void *opaque)
{
    ArtemisUsjState *s = ARTEMIS_USJ(opaque);

    return fifo8_is_empty(&s->rx_fifo) ? ARTEMIS_USJ_FIFO_LEN : 0;
}

static void chr_receive(void *opaque, const uint8_t *buf, int size)
{
    ArtemisUsjState *s = ARTEMIS_USJ(opaque);

    fifo8_push_all(&s->rx_fifo, buf, size);
    s->int_raw |= INT_OUT_RECV_PKT;
    update_irq(s);
}

static void usj_realize(DeviceState *dev, Error **errp)
{
    ArtemisUsjState *s = ARTEMIS_USJ(dev);

    qemu_chr_fe_set_handlers(&s->chr, chr_can_receive, chr_receive, NULL, NULL,
                             s, NULL, true);
}

void artemis_usj_power_cycle(ArtemisUsjState *s)
{
    s->tx_count = 0;
    s->int_raw &= ~INT_OUT_RECV_PKT;
    fifo8_reset(&s->rx_fifo);
    qemu_chr_fe_accept_input(&s->chr);
}

/*
 * A chip reset doesn't reach the bridge's FIFOs. Bytes a panic handler wrote and
 * never flushed go out with whatever the ROM prints next, and a packet the host
 * sent that nothing read is still waiting, holding up the ones after it.
 */
static void usj_reset_hold(Object *obj, ResetType type)
{
    ArtemisUsjState *s = ARTEMIS_USJ(obj);

    s->int_raw = (s->tx_count ? 0 : INT_IN_EMPTY) | (s->int_raw & INT_OUT_RECV_PKT);
    s->int_ena = 0;
    s->conf0 = 0x4200;
    s->misc_conf = 0;
    s->mem_conf = 0x2;
}

static void usj_init(Object *obj)
{
    ArtemisUsjState *s = ARTEMIS_USJ(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &usj_ops, s,
                          "artemis.usb-serial-jtag", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    fifo8_create(&s->rx_fifo, ARTEMIS_USJ_FIFO_LEN);
}

static Property usj_properties[] = {
    DEFINE_PROP_CHR("chardev", ArtemisUsjState, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void usj_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = usj_realize;
    rc->phases.hold = usj_reset_hold;
    device_class_set_props(dc, usj_properties);
}

static const TypeInfo usj_info = {
    .name = TYPE_ARTEMIS_USJ,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ArtemisUsjState),
    .instance_init = usj_init,
    .class_init = usj_class_init,
};

static void usj_register_types(void)
{
    type_register_static(&usj_info);
}

type_init(usj_register_types)
