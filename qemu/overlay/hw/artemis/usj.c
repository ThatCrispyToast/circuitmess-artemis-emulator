/*
 * ESP32-S3 USB Serial/JTAG controller, seen from the firmware's side.
 *
 * The USB wire isn't modelled: IN endpoint data goes straight to a chardev and
 * chardev input appears on the OUT endpoint. This is the console both the stock
 * firmware and the CircuitMess MicroPython build use.
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
#define A_MISC_CONF     0x44
#define A_MEM_CONF      0x48
#define A_DATE          0x80

#define EP1_WR_DONE       BIT(0)
#define EP1_IN_FREE       BIT(1)
#define EP1_OUT_AVAIL     BIT(2)

#define INT_OUT_RECV_PKT  BIT(2)
#define INT_IN_EMPTY      BIT(3)

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
        return s->int_raw;
    case A_INT_ST:
        return s->int_raw & s->int_ena;
    case A_INT_ENA:
        return s->int_ena;
    case A_CONF0:
        return s->conf0;
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
        s->int_raw &= ~(uint32_t)value;
        /* These two reflect FIFO state, so they come straight back if true. */
        if (!fifo8_is_empty(&s->rx_fifo)) {
            s->int_raw |= INT_OUT_RECV_PKT;
        }
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

static int chr_can_receive(void *opaque)
{
    ArtemisUsjState *s = ARTEMIS_USJ(opaque);

    return fifo8_num_free(&s->rx_fifo);
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

static void usj_reset_hold(Object *obj, ResetType type)
{
    ArtemisUsjState *s = ARTEMIS_USJ(obj);

    s->tx_count = 0;
    s->int_raw = INT_IN_EMPTY;
    s->int_ena = 0;
    s->conf0 = 0x4200;
    s->misc_conf = 0;
    s->mem_conf = 0x2;
    fifo8_reset(&s->rx_fifo);
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
