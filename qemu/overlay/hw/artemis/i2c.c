/*
 * ESP32-S3 I2C controller, master mode, driven through its command registers.
 * Transactions complete immediately; the bus is modelled at the byte level.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/irq.h"
#include "hw/artemis/artemis.h"

#define A_SCL_LOW_PERIOD 0x00
#define A_CTR            0x04
#define A_SR             0x08
#define A_TO             0x0C
#define A_FIFO_ST        0x14
#define A_FIFO_CONF      0x18
#define A_DATA           0x1C
#define A_INT_RAW        0x20
#define A_INT_CLR        0x24
#define A_INT_ENA        0x28
#define A_INT_STATUS     0x2C
#define A_COMD0          0x58
#define A_COMD7          0x74
#define A_DATE           0xF8
#define A_TXFIFO_START   0x100
#define A_RXFIFO_START   0x180

#define CTR_TRANS_START  BIT(5)
#define CTR_FSM_RST      BIT(10)
#define CTR_CONF_UPGATE  BIT(11)

#define SR_RESP_REC      BIT(0)
#define SR_BUS_BUSY      BIT(4)
#define SR_RXFIFO_CNT_S  8
#define SR_TXFIFO_CNT_S  18

#define FIFO_RX_RST      BIT(12)
#define FIFO_TX_RST      BIT(13)

#define INT_RXFIFO_WM       BIT(0)
#define INT_TXFIFO_WM       BIT(1)
#define INT_END_DETECT      BIT(3)
#define INT_TRANS_COMPLETE  BIT(7)
#define INT_NACK            BIT(10)

#define CMD_BYTE_NUM_M   0xFF
#define CMD_ACK_CHECK    BIT(8)
#define CMD_ACK_EXP      BIT(9)
#define CMD_ACK_VALUE    BIT(10)
#define CMD_OP_S         11
#define CMD_OP_M         7
#define CMD_DONE         BIT(31)

enum {
    OP_WRITE = 1,
    OP_STOP = 2,
    OP_READ = 3,
    OP_END = 4,
    OP_RSTART = 6,
};

static void update_irq(ArtemisI2cState *s)
{
    qemu_set_irq(s->irq, !!(s->int_raw & s->int_ena));
}

static void end_bus(ArtemisI2cState *s)
{
    if (s->started) {
        i2c_end_transfer(s->bus);
        s->started = false;
    }
}

/* Runs the command list until STOP, END, or an unacknowledged write. */
static void run_commands(ArtemisI2cState *s)
{
    for (int i = 0; i < ARTEMIS_I2C_CMD_COUNT; i++) {
        uint32_t cmd = s->cmd[i];
        uint32_t op = cmd >> CMD_OP_S & CMD_OP_M;
        uint32_t len = cmd & CMD_BYTE_NUM_M;
        bool nack = false;

        switch (op) {
        case OP_RSTART:
            s->address_phase = true;
            break;

        case OP_WRITE:
            for (uint32_t n = 0; n < len; n++) {
                uint8_t byte = fifo8_is_empty(&s->tx_fifo) ? 0xFF
                                                           : fifo8_pop(&s->tx_fifo);
                int acked;

                if (s->address_phase) {
                    end_bus(s);
                    acked = i2c_start_transfer(s->bus, byte >> 1, byte & 1) == 0;
                    s->started = acked;
                    s->address_phase = false;
                } else {
                    acked = s->started && i2c_send(s->bus, byte) == 0;
                }

                bool expect_nack = cmd & CMD_ACK_EXP;
                if (cmd & CMD_ACK_CHECK && acked == expect_nack) {
                    nack = true;
                    break;
                }
            }
            break;

        case OP_READ:
            for (uint32_t n = 0; n < len; n++) {
                uint8_t byte = s->started ? i2c_recv(s->bus) : 0xFF;

                if (!fifo8_is_full(&s->rx_fifo)) {
                    fifo8_push(&s->rx_fifo, byte);
                }
            }
            /* A NACK from the master ends the slave's turn on the bus. */
            if (cmd & CMD_ACK_VALUE && s->started) {
                i2c_nack(s->bus);
            }
            break;

        case OP_STOP:
            end_bus(s);
            s->cmd[i] |= CMD_DONE;
            s->int_raw |= INT_TRANS_COMPLETE;
            goto out;

        case OP_END:
            s->cmd[i] |= CMD_DONE;
            s->int_raw |= INT_END_DETECT;
            goto out;

        default:
            qemu_log_mask(LOG_GUEST_ERROR, "artemis.i2c: bad opcode %u\n", op);
            break;
        }

        if (nack) {
            /* The hardware abandons the list and generates a STOP itself. */
            end_bus(s);
            s->int_raw |= INT_NACK | INT_TRANS_COMPLETE;
            goto out;
        }
        s->cmd[i] |= CMD_DONE;
    }

out:
    /* Refilling these FIFOs is level-signalled by the watermarks. */
    s->int_raw |= INT_TXFIFO_WM;
    if (!fifo8_is_empty(&s->rx_fifo)) {
        s->int_raw |= INT_RXFIFO_WM;
    }
    update_irq(s);
}

static uint64_t i2c_read(void *opaque, hwaddr addr, unsigned size)
{
    ArtemisI2cState *s = ARTEMIS_I2C(opaque);

    switch (addr) {
    case A_SR: {
        uint32_t tx = fifo8_num_used(&s->tx_fifo);
        uint32_t rx = fifo8_num_used(&s->rx_fifo);
        return (s->started ? SR_BUS_BUSY : 0) | (rx << SR_RXFIFO_CNT_S) |
               (tx << SR_TXFIFO_CNT_S);
    }
    case A_FIFO_ST:
        return 0;
    case A_DATA:
    case A_RXFIFO_START ... A_RXFIFO_START + ARTEMIS_I2C_FIFO_LEN * 4 - 4:
        if (fifo8_is_empty(&s->rx_fifo)) {
            return 0;
        }
        return fifo8_pop(&s->rx_fifo);
    case A_INT_RAW:
        return s->int_raw;
    case A_INT_ENA:
        return s->int_ena;
    case A_INT_STATUS:
        return s->int_raw & s->int_ena;
    case A_COMD0 ... A_COMD7:
        return s->cmd[(addr - A_COMD0) / 4];
    case A_DATE:
        return 0x20070201;
    default:
        if (addr / 4 < ARRAY_SIZE(s->regs)) {
            return s->regs[addr / 4];
        }
        return 0;
    }
}

static void i2c_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ArtemisI2cState *s = ARTEMIS_I2C(opaque);
    uint32_t v = value;

    switch (addr) {
    case A_CTR:
        if (v & CTR_FSM_RST) {
            end_bus(s);
        }
        s->regs[A_CTR / 4] = v & ~(CTR_TRANS_START | CTR_CONF_UPGATE |
                                   CTR_FSM_RST);
        if (v & CTR_TRANS_START) {
            run_commands(s);
        }
        break;
    case A_FIFO_CONF:
        if (v & FIFO_RX_RST) {
            fifo8_reset(&s->rx_fifo);
        }
        if (v & FIFO_TX_RST) {
            fifo8_reset(&s->tx_fifo);
        }
        s->regs[A_FIFO_CONF / 4] = v;
        break;
    case A_DATA:
    case A_TXFIFO_START ... A_TXFIFO_START + ARTEMIS_I2C_FIFO_LEN * 4 - 4:
        if (!fifo8_is_full(&s->tx_fifo)) {
            fifo8_push(&s->tx_fifo, v & 0xFF);
        }
        break;
    case A_INT_CLR:
        s->int_raw &= ~v;
        update_irq(s);
        break;
    case A_INT_ENA:
        s->int_ena = v;
        update_irq(s);
        break;
    case A_COMD0 ... A_COMD7:
        s->cmd[(addr - A_COMD0) / 4] = v;
        break;
    default:
        if (addr / 4 < ARRAY_SIZE(s->regs)) {
            s->regs[addr / 4] = v;
        }
        break;
    }
}

static const MemoryRegionOps i2c_ops = {
    .read = i2c_read,
    .write = i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void i2c_reset_hold(Object *obj, ResetType type)
{
    ArtemisI2cState *s = ARTEMIS_I2C(obj);

    end_bus(s);
    s->address_phase = false;
    fifo8_reset(&s->tx_fifo);
    fifo8_reset(&s->rx_fifo);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->cmd, 0, sizeof(s->cmd));
    s->int_raw = 0;
    s->int_ena = 0;
}

static void i2c_init(Object *obj)
{
    ArtemisI2cState *s = ARTEMIS_I2C(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &i2c_ops, s, "artemis.i2c", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->bus = i2c_init_bus(DEVICE(obj), "i2c");
    fifo8_create(&s->tx_fifo, ARTEMIS_I2C_FIFO_LEN);
    fifo8_create(&s->rx_fifo, ARTEMIS_I2C_FIFO_LEN);
}

static void i2c_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = i2c_reset_hold;
}

static const TypeInfo i2c_info = {
    .name = TYPE_ARTEMIS_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ArtemisI2cState),
    .instance_init = i2c_init,
    .class_init = i2c_class_init,
};

static void i2c_register_types(void)
{
    type_register_static(&i2c_info);
}

type_init(i2c_register_types)
