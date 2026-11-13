/*
 * Register storage for the parts of the ESP32-S3 that have no model of their own.
 *
 * Each known register starts at the chip's reset value and holds what firmware
 * writes to its read/write bits, so read-modify-write sequences and read-back
 * checks come out as they do on the chip. Nothing here does anything. Addresses
 * without a description read as zero.
 *
 * The debug-assist block is the one exception: it remembers where each core was
 * when the chip was reset, which the ROM prints as "Saved PC".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/artemis/artemis.h"

typedef struct {
    uint32_t address;
    uint32_t reset;
    uint32_t writable;
} ArtemisSocRegister;

#include "soc_registers.h"

#define IO_BASE   0x60000000
#define IO_WORDS  (ARTEMIS_SOCREGS_SIZE / 4)

/* ASSIST_DEBUG: each core's block is 0x90 long. */
#define ASSIST_BASE             0x600CE000
#define ASSIST_CORE_STRIDE      0x90
#define ASSIST_RCD_ENABLE       0x48
#define ASSIST_RCD_RECORDING    0x4C
#define ASSIST_RCD_PDEBUGINST   0x50
#define ASSIST_RCD_PDEBUGPC     0x5C
#define ASSIST_RCD_SP           0x6C

static uint32_t *word(ArtemisSocRegsState *s, uint32_t address)
{
    return &s->value[(address - IO_BASE) / 4];
}

static bool is_recorded(uint32_t address)
{
    uint32_t offset = (address - ASSIST_BASE) % ASSIST_CORE_STRIDE;

    return address >= ASSIST_BASE && address < ASSIST_BASE + 2 * ASSIST_CORE_STRIDE &&
           offset >= ASSIST_RCD_PDEBUGINST && offset <= ASSIST_RCD_SP;
}

void artemis_socregs_describe(uint32_t base, uint32_t *regs, uint32_t *writable, size_t count)
{
    for (size_t i = 0; i < ARRAY_SIZE(artemis_soc_registers); i++) {
        const ArtemisSocRegister *r = &artemis_soc_registers[i];

        if (r->address >= base && (r->address - base) / 4 < count) {
            regs[(r->address - base) / 4] = r->reset;
            if (writable) {
                writable[(r->address - base) / 4] = r->writable;
            }
        }
    }
}

void artemis_socregs_load(uint32_t base, uint32_t *regs, size_t count)
{
    artemis_socregs_describe(base, regs, NULL, count);
}

void artemis_socregs_reset_range(ArtemisSocRegsState *s, uint32_t base, uint32_t size)
{
    for (size_t i = 0; i < ARRAY_SIZE(artemis_soc_registers); i++) {
        const ArtemisSocRegister *r = &artemis_soc_registers[i];

        if (r->address >= base && r->address - base < size) {
            *word(s, r->address) = r->reset;
        }
    }
}

void artemis_socregs_record_reset(ArtemisSocRegsState *s, int core, uint32_t pc,
                                  uint32_t sp)
{
    uint32_t base = ASSIST_BASE + core * ASSIST_CORE_STRIDE;

    if ((*word(s, base + ASSIST_RCD_ENABLE) & 1) &&
        (*word(s, base + ASSIST_RCD_RECORDING) & 1)) {
        *word(s, base + ASSIST_RCD_PDEBUGPC) = pc;
        *word(s, base + ASSIST_RCD_SP) = sp;
    }
}

void artemis_socregs_power_cycle(ArtemisSocRegsState *s)
{
    for (int core = 0; core < 2; core++) {
        uint32_t base = ASSIST_BASE + core * ASSIST_CORE_STRIDE;

        for (uint32_t offset = ASSIST_RCD_PDEBUGINST; offset <= ASSIST_RCD_SP; offset += 4) {
            *word(s, base + offset) = 0;
        }
    }
}

static uint64_t socregs_read(void *opaque, hwaddr addr, unsigned size)
{
    ArtemisSocRegsState *s = ARTEMIS_SOCREGS(opaque);

    return s->known[addr / 4] ? s->value[addr / 4] : 0;
}

static void socregs_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ArtemisSocRegsState *s = ARTEMIS_SOCREGS(opaque);
    uint32_t writable = s->writable[addr / 4];

    s->value[addr / 4] = (s->value[addr / 4] & ~writable) | (value & writable);
}

static const MemoryRegionOps socregs_ops = {
    .read = socregs_read,
    .write = socregs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void socregs_reset_hold(Object *obj, ResetType type)
{
    ArtemisSocRegsState *s = ARTEMIS_SOCREGS(obj);

    for (size_t i = 0; i < ARRAY_SIZE(artemis_soc_registers); i++) {
        const ArtemisSocRegister *r = &artemis_soc_registers[i];

        /* What was recorded about the last run outlives the reset that ended it. */
        if (!is_recorded(r->address)) {
            *word(s, r->address) = r->reset;
        }
    }
}

static void socregs_init(Object *obj)
{
    ArtemisSocRegsState *s = ARTEMIS_SOCREGS(obj);

    s->value = g_new0(uint32_t, IO_WORDS);
    s->writable = g_new0(uint32_t, IO_WORDS);
    s->known = g_new0(bool, IO_WORDS);
    for (size_t i = 0; i < ARRAY_SIZE(artemis_soc_registers); i++) {
        const ArtemisSocRegister *r = &artemis_soc_registers[i];
        size_t index = (r->address - IO_BASE) / 4;

        s->known[index] = true;
        s->writable[index] = r->writable;
    }

    memory_region_init_io(&s->iomem, obj, &socregs_ops, s, "artemis.socregs",
                          ARTEMIS_SOCREGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void socregs_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = socregs_reset_hold;
}

static const TypeInfo socregs_info = {
    .name = TYPE_ARTEMIS_SOCREGS,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ArtemisSocRegsState),
    .instance_init = socregs_init,
    .class_init = socregs_class_init,
};

static void socregs_register_types(void)
{
    type_register_static(&socregs_info);
}

type_init(socregs_register_types)
