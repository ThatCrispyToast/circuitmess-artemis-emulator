/*
 * ESP32-S3 radio blocks: analog I2C master, RF front end, baseband, Wi-Fi MAC.
 * The BLE core has a model of its own in ble.c.
 *
 * There's no radio to emulate behind these, so they behave as plain storage and
 * firmware that brings up Wi-Fi or Bluetooth initializes normally, then simply
 * never sees a packet. PHY calibration and the controllers poll many undocumented
 * status bits; a register read repeatedly with nothing written in between is
 * assumed to be such a wait, and starts alternating between all-ones and all-zeros
 * so that a loop waiting for bits either way gets to continue.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/artemis/artemis.h"

#define ANA_I2C_CMD_START 0x6000E000
#define ANA_I2C_CMD_END   0x6000E040

#define POLL_THRESHOLD 16

/* In the Wi-Fi MAC: the transmitter's state, which a watch reads as idle. */
#define MAC_TX_STATUS     0x60033D14
#define MAC_TX_IDLE       1


static const struct {
    hwaddr base;
    uint32_t size;
} windows[] = {
    { 0x60005000, 0x1000 }, /* FE2 */
    { 0x60006000, 0x1000 }, /* FE */
    { 0x6000E000, 0x1000 }, /* analog I2C master */
    { 0x60011000, 0x2000 }, /* Bluetooth baseband */
    { 0x6001C000, 0x2000 }, /* NRX, BB */
    { 0x60030000, 0x8000 }, /* Wi-Fi MAC, modem clocks */
};

typedef struct RadioWindow {
    MemoryRegion mr;
    struct ArtemisRadioState *radio;
    hwaddr base;
    uint32_t *regs;
} RadioWindow;

struct ArtemisRadioState {
    SysBusDevice parent_obj;

    RadioWindow win[ARRAY_SIZE(windows)];
    hwaddr last_read;
    uint32_t repeats;
};

static uint64_t radio_read(void *opaque, hwaddr addr, unsigned size)
{
    RadioWindow *w = opaque;
    ArtemisRadioState *s = w->radio;

    if (w->base + addr == MAC_TX_STATUS) {
        return MAC_TX_IDLE;
    }
    if (s->last_read == w->base + addr) {
        if (++s->repeats >= POLL_THRESHOLD) {
            return (s->repeats & 1) ? UINT32_MAX : 0;
        }
    } else {
        s->last_read = w->base + addr;
        s->repeats = 0;
    }
    qemu_log_mask(LOG_UNIMP, "radio: read %08" HWADDR_PRIx " = %08x\n",
                  w->base + addr, w->regs[addr / 4]);
    return w->regs[addr / 4];
}

static void radio_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    RadioWindow *w = opaque;

    w->radio->last_read = 0;
    qemu_log_mask(LOG_UNIMP, "radio: write %08" HWADDR_PRIx " = %08x\n",
                  w->base + addr, (uint32_t)value);

    /* The analog I2C master's command registers latch a busy bit: never set. */
    if (w->base + addr >= ANA_I2C_CMD_START && w->base + addr < ANA_I2C_CMD_END) {
        return;
    }
    w->regs[addr / 4] = value;
}

static const MemoryRegionOps radio_ops = {
    .read = radio_read,
    .write = radio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

void artemis_radio_map(ArtemisRadioState *s, MemoryRegion *sys_mem)
{
    for (size_t i = 0; i < ARRAY_SIZE(windows); i++) {
        /* Below any real device that shares the window, such as the RNG. */
        memory_region_add_subregion_overlap(sys_mem, windows[i].base,
                                            &s->win[i].mr, -1);
    }
}

static void radio_init(Object *obj)
{
    ArtemisRadioState *s = ARTEMIS_RADIO(obj);

    for (size_t i = 0; i < ARRAY_SIZE(windows); i++) {
        RadioWindow *w = &s->win[i];

        w->radio = s;
        w->base = windows[i].base;
        w->regs = g_new0(uint32_t, windows[i].size / 4);
        memory_region_init_io(&w->mr, obj, &radio_ops, w, "artemis.radio",
                              windows[i].size);
    }
}

static const TypeInfo radio_info = {
    .name = TYPE_ARTEMIS_RADIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ArtemisRadioState),
    .instance_init = radio_init,
};

static void radio_register_types(void)
{
    type_register_static(&radio_info);
}

type_init(radio_register_types)
