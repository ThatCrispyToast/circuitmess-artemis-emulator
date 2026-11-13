/*
 * ESP32-S3 SENS block: the SAR ADC as ESP-IDF's oneshot driver reads it, the
 * internal temperature sensor, and touch pads that nothing is touching.
 * Everything else in the block is plain storage.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/artemis/artemis.h"

#define A_SAR_MEAS1_CTRL2 0x0C
#define A_SAR_ATTEN1      0x14
#define A_SAR_MEAS2_CTRL2 0x30
#define A_SAR_ATTEN2      0x38
#define A_SAR_SLAVE_ADDR1 0x40
#define A_SAR_TSENS_CTRL  0x50
#define A_TOUCH_CHN_ST    0x9C
#define A_TOUCH_STATUS1   0xA4
#define A_TOUCH_STATUS14  0xD8
#define SENS_BASE         0x60008800

#define TOUCH_MEAS_DONE   BIT(31)
#define TOUCH_DATA_M      0x3FFFFF

#define CTRL2_DATA_M    0xFFFF
#define CTRL2_DONE      BIT(16)
#define CTRL2_START     BIT(17)
#define CTRL2_EN_PAD_S  19
#define CTRL2_EN_PAD_M  0xFFF

#define MEAS_STATUS_M   (0xFF << 22)

#define TSENS_OUT_M     0xFF
#define TSENS_READY     BIT(8)
/* About 30 C through the driver's conversion at the default range. */
#define TSENS_READING   115

static void measure(ArtemisSaradcState *s, int unit, uint32_t *reg, uint32_t v)
{
    bool was_started = *reg & CTRL2_START;
    bool start = v & CTRL2_START;
    uint32_t result = *reg & (CTRL2_DATA_M | CTRL2_DONE);

    if (!start) {
        result = 0;
    } else if (!was_started) {
        uint32_t pads = v >> CTRL2_EN_PAD_S & CTRL2_EN_PAD_M;
        uint32_t raw = 0;

        if (pads && s->input) {
            int chan = ctz32(pads);
            uint32_t atten = s->regs[(unit == 1 ? A_SAR_ATTEN1 : A_SAR_ATTEN2) / 4];

            raw = s->input(s->input_opaque, unit, chan, atten >> (chan * 2) & 3);
        }
        result = (raw & CTRL2_DATA_M) | CTRL2_DONE;
    }

    *reg = (v & ~(CTRL2_DATA_M | CTRL2_DONE)) | result;
}

/* Counts an untouched pad gives, pad 1 first: around what a watch reads on its free pads. */
static const uint16_t touch_idle_counts[14] = {
    11391, 11468, 11532, 11610, 11344, 11287, 11655,
    11726, 11204, 11757, 11772, 11498, 11573, 11430,
};

static uint64_t saradc_read(void *opaque, hwaddr addr, unsigned size)
{
    ArtemisSaradcState *s = ARTEMIS_SARADC(opaque);
    uint32_t v = s->regs[(addr / 4) & 0xFF];

    if (addr == A_SAR_SLAVE_ADDR1) {
        v &= ~MEAS_STATUS_M;
    } else if (addr == A_SAR_TSENS_CTRL) {
        v = (v & ~TSENS_OUT_M) | TSENS_READY | TSENS_READING;
    } else if (addr == A_TOUCH_CHN_ST) {
        v |= TOUCH_MEAS_DONE;
    } else if (addr >= A_TOUCH_STATUS1 && addr <= A_TOUCH_STATUS14) {
        v = (v & ~TOUCH_DATA_M) | touch_idle_counts[(addr - A_TOUCH_STATUS1) / 4];
    }
    return v;
}

static void saradc_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ArtemisSaradcState *s = ARTEMIS_SARADC(opaque);
    uint32_t *reg = &s->regs[(addr / 4) & 0xFF];

    switch (addr) {
    case A_SAR_MEAS1_CTRL2:
        measure(s, 1, reg, value);
        break;
    case A_SAR_MEAS2_CTRL2:
        measure(s, 2, reg, value);
        break;
    default:
        *reg = value;
        break;
    }
}

static const MemoryRegionOps saradc_ops = {
    .read = saradc_read,
    .write = saradc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void saradc_reset_hold(Object *obj, ResetType type)
{
    ArtemisSaradcState *s = ARTEMIS_SARADC(obj);

    memset(s->regs, 0, sizeof(s->regs));
    artemis_socregs_load(SENS_BASE, s->regs, ARRAY_SIZE(s->regs));
}

static void saradc_init(Object *obj)
{
    ArtemisSaradcState *s = ARTEMIS_SARADC(obj);

    memory_region_init_io(&s->iomem, obj, &saradc_ops, s, "artemis.saradc",
                          0x400);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void saradc_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = saradc_reset_hold;
}

static const TypeInfo saradc_info = {
    .name = TYPE_ARTEMIS_SARADC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ArtemisSaradcState),
    .instance_init = saradc_init,
    .class_init = saradc_class_init,
};

static void saradc_register_types(void)
{
    type_register_static(&saradc_info);
}

type_init(saradc_register_types)
