/*
 * ESP32-S3 general-purpose SPI controller, master mode.
 *
 * Handles both ways firmware pushes pixels: LovyanGFX's direct use of the W
 * buffer and GDMA, and ESP-IDF's spi_master driver. Long transfers take as
 * long as they would on the wire, so frame rates stay realistic.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/artemis/artemis.h"

#define A_CMD         0x00
#define A_ADDR        0x04
#define A_CTRL        0x08
#define A_CLOCK       0x0C
#define A_USER        0x10
#define A_USER1       0x14
#define A_USER2       0x18
#define A_MS_DLEN     0x1C
#define A_MISC        0x20
#define A_DIN_MODE    0x24
#define A_DIN_NUM     0x28
#define A_DOUT_MODE   0x2C
#define A_DMA_CONF    0x30
#define A_DMA_INT_ENA 0x34
#define A_DMA_INT_CLR 0x38
#define A_DMA_INT_RAW 0x3C
#define A_DMA_INT_ST  0x40
#define A_DMA_INT_SET 0x44
#define A_W0          0x98
#define A_W15         0xD4
#define A_SLAVE       0xE0
#define A_SLAVE1      0xE4
#define A_CLK_GATE    0xE8
#define A_DATE        0xF0

#define CMD_USR       BIT(24)
#define CMD_UPDATE    BIT(23)

#define USER_DOUTDIN       BIT(0)
#define USER_MISO_HIGHPART BIT(24)
#define USER_MOSI_HIGHPART BIT(25)
#define USER_USR_MOSI      BIT(27)
#define USER_USR_MISO      BIT(28)
#define USER_USR_DUMMY     BIT(29)
#define USER_USR_ADDR      BIT(30)
#define USER_USR_COMMAND   BIT(31)

#define USER1_DUMMY_M      0xFF
#define USER1_ADDR_LEN_S   27
#define USER2_CMD_VAL_M    0xFFFF
#define USER2_CMD_LEN_S    28

#define DMA_CONF_RX_ENA    BIT(27)
#define DMA_CONF_TX_ENA    BIT(28)

#define INT_TRANS_DONE     BIT(12)

#define CLOCK_EQU_SYSCLK   BIT(31)
#define CLOCK_PRE_S        18
#define CLOCK_PRE_M        0xF
#define CLOCK_N_S          12
#define CLOCK_N_M          0x3F

/* Below this a real busy-wait would outlast the timer's own overhead. */
#define SYNC_COMPLETE_NS   (20 * SCALE_US)

static uint32_t sclk_hz(ArtemisGpspiState *s)
{
    if (s->clock & CLOCK_EQU_SYSCLK) {
        return 80000000;
    }
    uint32_t pre = (s->clock >> CLOCK_PRE_S & CLOCK_PRE_M) + 1;
    uint32_t n = (s->clock >> CLOCK_N_S & CLOCK_N_M) + 1;
    return 80000000 / (pre * n);
}

static void update_irq(ArtemisGpspiState *s)
{
    qemu_set_irq(s->irq, !!(s->dma_int_raw & s->dma_int_ena));
}

static void finish_transaction(ArtemisGpspiState *s)
{
    s->cmd &= ~CMD_USR;
    s->dma_int_raw |= INT_TRANS_DONE;
    update_irq(s);
}

static void done_tick(void *opaque)
{
    finish_transaction(ARTEMIS_GPSPI(opaque));
}

static bool find_dma_channel(ArtemisGpspiState *s, int dir, uint32_t *chan)
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

static void shift_bytes(ArtemisGpspiState *s, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        ssi_transfer(s->bus, data[i]);
    }
}

static void start_transaction(ArtemisGpspiState *s)
{
    uint64_t bits = 0;
    uint32_t dlen_bits = (s->ms_dlen & 0x3FFFF) + 1;
    uint32_t dlen = DIV_ROUND_UP(dlen_bits, 8);

    if (s->user & USER_USR_COMMAND) {
        uint32_t cmd_bits = (s->user2 >> USER2_CMD_LEN_S) + 1;
        uint8_t raw[2] = { s->user2 & 0xFF, s->user2 >> 8 & 0xFF };

        shift_bytes(s, raw, DIV_ROUND_UP(cmd_bits, 8));
        bits += cmd_bits;
    }

    if (s->user & USER_USR_ADDR) {
        uint32_t addr_bits = (s->user1 >> USER1_ADDR_LEN_S) + 1;
        uint8_t raw[4];

        stl_le_p(raw, s->addr);
        shift_bytes(s, raw, DIV_ROUND_UP(addr_bits, 8));
        bits += addr_bits;
    }

    if (s->user & USER_USR_DUMMY) {
        bits += (s->user1 & USER1_DUMMY_M) + 1;
    }

    uint8_t *mosi = NULL;
    uint32_t chan;

    if (s->user & USER_USR_MOSI) {
        mosi = g_malloc(dlen);

        if (s->dma_conf & DMA_CONF_TX_ENA && s->gdma &&
            find_dma_channel(s, ESP_GDMA_OUT_IDX, &chan)) {
            if (!esp_gdma_read_channel(s->gdma, chan, mosi, dlen)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "artemis.gpspi: GDMA read of %u bytes failed\n",
                              dlen);
                memset(mosi, 0xFF, dlen);
            }
        } else {
            unsigned first = (s->user & USER_MOSI_HIGHPART) ? 8 : 0;
            uint32_t space = (16 - first) * 4;
            uint8_t window[64];

            for (unsigned i = 0; i < 16; i++) {
                stl_le_p(window + i * 4, s->data[i]);
            }
            memcpy(mosi, window + first * 4, MIN(dlen, space));
            if (dlen > space) {
                memset(mosi + space, 0, dlen - space);
            }
        }

        bits += dlen_bits;
    }

    if (s->user & USER_USR_MISO) {
        /* The only device on the bus is the write-only panel. */
        uint8_t *miso = g_malloc(dlen);

        for (uint32_t i = 0; i < dlen; i++) {
            uint8_t tx = (mosi && s->user & USER_DOUTDIN) ? mosi[i] : 0xFF;
            miso[i] = ssi_transfer(s->bus, tx);
        }

        if (s->dma_conf & DMA_CONF_RX_ENA && s->gdma &&
            find_dma_channel(s, ESP_GDMA_IN_IDX, &chan)) {
            esp_gdma_write_channel(s->gdma, chan, miso, dlen);
        } else {
            unsigned first = (s->user & USER_MISO_HIGHPART) ? 8 : 0;
            uint32_t space = (16 - first) * 4;
            uint8_t window[64] = { 0 };

            memcpy(window, miso, MIN(dlen, space));
            for (unsigned i = 0; i < 16 - first; i++) {
                s->data[first + i] = ldl_le_p(window + i * 4);
            }
        }

        if (!(s->user & USER_USR_MOSI)) {
            bits += dlen_bits;
        }
        g_free(miso);
    } else if (mosi) {
        shift_bytes(s, mosi, dlen);
    }

    g_free(mosi);

    int64_t ns = bits * NANOSECONDS_PER_SECOND / sclk_hz(s);

    s->cmd |= CMD_USR;
    if (ns < SYNC_COMPLETE_NS) {
        finish_transaction(s);
    } else {
        timer_mod(s->done_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns);
    }
}

static uint64_t gpspi_read(void *opaque, hwaddr addr, unsigned size)
{
    ArtemisGpspiState *s = ARTEMIS_GPSPI(opaque);

    switch (addr) {
    case A_CMD:
        return s->cmd;
    case A_ADDR:
        return s->addr;
    case A_CTRL:
        return s->ctrl;
    case A_CLOCK:
        return s->clock;
    case A_USER:
        return s->user;
    case A_USER1:
        return s->user1;
    case A_USER2:
        return s->user2;
    case A_MS_DLEN:
        return s->ms_dlen;
    case A_MISC:
        return s->misc;
    case A_DIN_MODE:
        return s->din_mode;
    case A_DIN_NUM:
        return s->din_num;
    case A_DOUT_MODE:
        return s->dout_mode;
    case A_DMA_CONF:
        return s->dma_conf;
    case A_DMA_INT_ENA:
        return s->dma_int_ena;
    case A_DMA_INT_RAW:
        return s->dma_int_raw;
    case A_DMA_INT_ST:
        return s->dma_int_raw & s->dma_int_ena;
    case A_W0 ... A_W15:
        return s->data[(addr - A_W0) / 4];
    case A_SLAVE:
        return s->slave;
    case A_SLAVE1:
        return s->slave1;
    case A_CLK_GATE:
        return s->clk_gate;
    case A_DATE:
        return 0x2101220;
    default:
        return 0;
    }
}

static void gpspi_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ArtemisGpspiState *s = ARTEMIS_GPSPI(opaque);
    uint32_t v = value;

    switch (addr) {
    case A_CMD:
        /* UPDATE latches configuration across clock domains: nothing to do. */
        s->cmd = v & ~(CMD_UPDATE | CMD_USR);
        if (v & CMD_USR) {
            start_transaction(s);
        }
        break;
    case A_ADDR:
        s->addr = v;
        break;
    case A_CTRL:
        s->ctrl = v;
        break;
    case A_CLOCK:
        s->clock = v;
        break;
    case A_USER:
        s->user = v;
        break;
    case A_USER1:
        s->user1 = v;
        break;
    case A_USER2:
        s->user2 = v;
        break;
    case A_MS_DLEN:
        s->ms_dlen = v;
        break;
    case A_MISC:
        s->misc = v;
        break;
    case A_DIN_MODE:
        s->din_mode = v;
        break;
    case A_DIN_NUM:
        s->din_num = v;
        break;
    case A_DOUT_MODE:
        s->dout_mode = v;
        break;
    case A_DMA_CONF:
        /* The FIFO reset bits are self-clearing. */
        s->dma_conf = v & ~(BIT(29) | BIT(30) | BIT(31));
        break;
    case A_DMA_INT_ENA:
        s->dma_int_ena = v;
        update_irq(s);
        break;
    case A_DMA_INT_CLR:
        s->dma_int_raw &= ~v;
        update_irq(s);
        break;
    case A_DMA_INT_SET:
        s->dma_int_raw |= v;
        update_irq(s);
        break;
    case A_W0 ... A_W15:
        s->data[(addr - A_W0) / 4] = v;
        break;
    case A_SLAVE:
        s->slave = v;
        break;
    case A_SLAVE1:
        s->slave1 = v;
        break;
    case A_CLK_GATE:
        s->clk_gate = v;
        break;
    }
}

static const MemoryRegionOps gpspi_ops = {
    .read = gpspi_read,
    .write = gpspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void gpspi_reset_hold(Object *obj, ResetType type)
{
    ArtemisGpspiState *s = ARTEMIS_GPSPI(obj);

    timer_del(s->done_timer);
    s->cmd = 0;
    s->addr = 0;
    s->ctrl = 0x003C0000;
    s->clock = 0x80003043;
    s->user = 0x800000C0;
    s->user1 = 0xB8410007;
    s->user2 = 0x70000000;
    s->ms_dlen = 0;
    s->misc = 0x3E;
    s->din_mode = 0;
    s->din_num = 0;
    s->dout_mode = 0;
    s->dma_conf = 0;
    s->dma_int_ena = 0;
    s->dma_int_raw = 0;
    s->slave = 0x02800000;
    s->slave1 = 0;
    s->clk_gate = 0;
    memset(s->data, 0, sizeof(s->data));
    update_irq(s);
}

static void gpspi_init(Object *obj)
{
    ArtemisGpspiState *s = ARTEMIS_GPSPI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &gpspi_ops, s, "artemis.gpspi",
                          0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->bus = ssi_create_bus(DEVICE(obj), "spi");
    s->done_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, done_tick, s);
}

static Property gpspi_properties[] = {
    DEFINE_PROP_UINT32("gdma-periph", ArtemisGpspiState, gdma_periph, GDMA_SPI2),
    DEFINE_PROP_END_OF_LIST(),
};

static void gpspi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = gpspi_reset_hold;
    device_class_set_props(dc, gpspi_properties);
}

static const TypeInfo gpspi_info = {
    .name = TYPE_ARTEMIS_GPSPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ArtemisGpspiState),
    .instance_init = gpspi_init,
    .class_init = gpspi_class_init,
};

static void gpspi_register_types(void)
{
    type_register_static(&gpspi_info);
}

type_init(gpspi_register_types)
