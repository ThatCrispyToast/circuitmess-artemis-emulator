/*
 * Sitronix ST7735S TFT controller on the watch's 1.44" 128x128 glass.
 *
 * GRAM geometry and which part of it reaches the glass depend on the panel
 * revision; see the machine setup. Coordinates follow the datasheet's MADCTL
 * semantics: MV exchanges the address counters, then MX mirrors the source
 * (column) axis and MY mirrors the gate (row) axis.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/artemis/artemis.h"

enum {
    CMD_NOP = 0x00,
    CMD_SWRESET = 0x01,
    CMD_SLPIN = 0x10,
    CMD_SLPOUT = 0x11,
    CMD_PTLON = 0x12,
    CMD_NORON = 0x13,
    CMD_INVOFF = 0x20,
    CMD_INVON = 0x21,
    CMD_DISPOFF = 0x28,
    CMD_DISPON = 0x29,
    CMD_CASET = 0x2A,
    CMD_RASET = 0x2B,
    CMD_RAMWR = 0x2C,
    CMD_MADCTL = 0x36,
    CMD_IDMOFF = 0x38,
    CMD_IDMON = 0x39,
    CMD_COLMOD = 0x3A,
};

#define MAD_MY  0x80
#define MAD_MX  0x40
#define MAD_MV  0x20
#define MAD_BGR 0x08

static void controller_reset(ArtemisSt7735sState *s)
{
    s->sleeping = true;
    s->display_on = false;
    s->inverted = false;
    s->idle = false;
    s->madctl = 0;
    s->colmod = 0x06;
    s->have_cmd = false;
    s->ram_writing = false;
    s->param_count = 0;
    s->pixel_fill = 0;
    s->x_start = 0;
    s->x_end = s->gram_w - 1;
    s->y_start = 0;
    s->y_end = s->gram_h - 1;
    s->x = 0;
    s->y = 0;
    s->dirty = true;
}

static inline void write_pixel(ArtemisSt7735sState *s, uint16_t color)
{
    uint32_t col, row;

    if (s->madctl & MAD_MV) {
        col = s->y;
        row = s->x;
    } else {
        col = s->x;
        row = s->y;
    }
    if (s->madctl & MAD_MX) {
        col = s->gram_w - 1 - col;
    }
    if (s->madctl & MAD_MY) {
        row = s->gram_h - 1 - row;
    }

    if (col < s->gram_w && row < s->gram_h) {
        s->gram[row * ARTEMIS_ST7735S_GRAM_W + col] = color;
        s->dirty = true;
    }

    if (s->x >= s->x_end) {
        s->x = s->x_start;
        s->y = s->y >= s->y_end ? s->y_start : s->y + 1;
    } else {
        s->x++;
    }
}

static inline uint16_t rgb_to_565(uint8_t r, uint8_t g, uint8_t b)
{
    return (r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3;
}

static void ram_byte(ArtemisSt7735sState *s, uint8_t byte)
{
    s->pixel_bytes[s->pixel_fill++] = byte;

    switch (s->colmod & 7) {
    case 3: /* 12 bit: three bytes carry two pixels. */
        if (s->pixel_fill == 3) {
            uint8_t *p = s->pixel_bytes;
            write_pixel(s, rgb_to_565(p[0] & 0xF0, (p[0] & 0x0F) << 4,
                                      p[1] & 0xF0));
            write_pixel(s, rgb_to_565((p[1] & 0x0F) << 4, p[2] & 0xF0,
                                      (p[2] & 0x0F) << 4));
            s->pixel_fill = 0;
        }
        break;
    case 6: /* 18 bit */
        if (s->pixel_fill == 3) {
            uint8_t *p = s->pixel_bytes;
            write_pixel(s, rgb_to_565(p[0], p[1], p[2]));
            s->pixel_fill = 0;
        }
        break;
    default: /* 16 bit */
        if (s->pixel_fill == 2) {
            write_pixel(s, s->pixel_bytes[0] << 8 | s->pixel_bytes[1]);
            s->pixel_fill = 0;
        }
        break;
    }
}

static void handle_command(ArtemisSt7735sState *s, uint8_t cmd)
{
    s->cmd = cmd;
    s->have_cmd = true;
    s->param_count = 0;
    s->ram_writing = false;

    switch (cmd) {
    case CMD_SWRESET:
        controller_reset(s);
        break;
    case CMD_SLPIN:
        s->sleeping = true;
        s->dirty = true;
        break;
    case CMD_SLPOUT:
        s->sleeping = false;
        s->dirty = true;
        break;
    case CMD_INVOFF:
        s->inverted = false;
        s->dirty = true;
        break;
    case CMD_INVON:
        s->inverted = true;
        s->dirty = true;
        break;
    case CMD_DISPOFF:
        s->display_on = false;
        s->dirty = true;
        break;
    case CMD_DISPON:
        s->display_on = true;
        s->dirty = true;
        break;
    case CMD_IDMOFF:
        s->idle = false;
        s->dirty = true;
        break;
    case CMD_IDMON:
        s->idle = true;
        s->dirty = true;
        break;
    case CMD_RAMWR:
        s->x = s->x_start;
        s->y = s->y_start;
        s->pixel_fill = 0;
        s->ram_writing = true;
        break;
    default:
        break;
    }
}

static void handle_data(ArtemisSt7735sState *s, uint8_t byte)
{
    if (s->ram_writing) {
        ram_byte(s, byte);
        return;
    }
    if (!s->have_cmd) {
        return;
    }

    if (s->param_count < sizeof(s->params)) {
        s->params[s->param_count] = byte;
    }
    s->param_count++;

    switch (s->cmd) {
    case CMD_CASET:
        if (s->param_count == 4) {
            s->x_start = s->params[0] << 8 | s->params[1];
            s->x_end = s->params[2] << 8 | s->params[3];
        }
        break;
    case CMD_RASET:
        if (s->param_count == 4) {
            s->y_start = s->params[0] << 8 | s->params[1];
            s->y_end = s->params[2] << 8 | s->params[3];
        }
        break;
    case CMD_MADCTL:
        if (s->param_count == 1) {
            s->madctl = byte;
            s->dirty = true;
        }
        break;
    case CMD_COLMOD:
        if (s->param_count == 1) {
            s->colmod = byte;
            s->pixel_fill = 0;
        }
        break;
    default:
        break;
    }
}

static uint32_t st7735s_transfer(SSIPeripheral *dev, uint32_t value)
{
    ArtemisSt7735sState *s = ARTEMIS_ST7735S(dev);

    if (s->in_reset) {
        return 0;
    }
    if (s->dc) {
        handle_data(s, value);
    } else {
        handle_command(s, value);
    }
    return 0;
}

static void dc_set(void *opaque, int n, int level)
{
    ARTEMIS_ST7735S(opaque)->dc = level;
}

static void reset_set(void *opaque, int n, int level)
{
    ArtemisSt7735sState *s = ARTEMIS_ST7735S(opaque);
    bool asserted = !level;

    if (asserted && !s->in_reset) {
        controller_reset(s);
    }
    s->in_reset = asserted;
}

void artemis_st7735s_snapshot(ArtemisSt7735sState *s, uint16_t *out)
{
    if (s->sleeping || !s->display_on || s->in_reset) {
        memset(out, 0, ARTEMIS_LCD_W * ARTEMIS_LCD_H * sizeof(uint16_t));
        return;
    }

    bool swap = !(s->madctl & MAD_BGR);

    for (uint32_t y = 0; y < ARTEMIS_LCD_H; y++) {
        for (uint32_t x = 0; x < ARTEMIS_LCD_W; x++) {
            uint32_t row = s->glass_row + (ARTEMIS_LCD_W - 1) - x;
            uint32_t col = s->glass_col + y;
            uint16_t c = s->gram[row * ARTEMIS_ST7735S_GRAM_W + col];

            if (swap) {
                c = (c & 0x07E0) | c >> 11 | c << 11;
            }
            if (s->inverted) {
                c = ~c;
            }
            if (s->idle) {
                /* Idle mode keeps the MSB of each colour channel only. */
                c = ((c & 0x8000) ? 0xF800 : 0) | ((c & 0x0400) ? 0x07E0 : 0) |
                    ((c & 0x0010) ? 0x001F : 0);
            }
            out[y * ARTEMIS_LCD_W + x] = c;
        }
    }
}

bool artemis_st7735s_take_dirty(ArtemisSt7735sState *s)
{
    bool d = s->dirty;

    s->dirty = false;
    return d;
}

void artemis_st7735s_power_cycle(ArtemisSt7735sState *s)
{
    controller_reset(s);
    memset(s->gram, 0, ARTEMIS_ST7735S_GRAM_W * ARTEMIS_ST7735S_GRAM_H *
                       sizeof(uint16_t));
}

static void st7735s_realize(SSIPeripheral *dev, Error **errp)
{
    ArtemisSt7735sState *s = ARTEMIS_ST7735S(dev);

    if (s->gram_w > ARTEMIS_ST7735S_GRAM_W ||
        s->gram_h > ARTEMIS_ST7735S_GRAM_H ||
        s->glass_col + ARTEMIS_LCD_H > s->gram_w ||
        s->glass_row + ARTEMIS_LCD_W > s->gram_h) {
        error_setg(errp, "st7735s: glass doesn't fit in GRAM");
        return;
    }

    s->gram = g_new0(uint16_t, ARTEMIS_ST7735S_GRAM_W * ARTEMIS_ST7735S_GRAM_H);
    qdev_init_gpio_in_named(DEVICE(dev), dc_set, "dc", 1);
    qdev_init_gpio_in_named(DEVICE(dev), reset_set, "reset", 1);
    controller_reset(s);
}

static Property st7735s_properties[] = {
    DEFINE_PROP_UINT32("gram-w", ArtemisSt7735sState, gram_w, 128),
    DEFINE_PROP_UINT32("gram-h", ArtemisSt7735sState, gram_h, 160),
    DEFINE_PROP_UINT32("glass-col", ArtemisSt7735sState, glass_col, 0),
    DEFINE_PROP_UINT32("glass-row", ArtemisSt7735sState, glass_row, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void st7735s_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *sc = SSI_PERIPHERAL_CLASS(klass);

    sc->realize = st7735s_realize;
    sc->transfer = st7735s_transfer;
    sc->cs_polarity = SSI_CS_NONE;
    device_class_set_props(dc, st7735s_properties);
}

static const TypeInfo st7735s_info = {
    .name = TYPE_ARTEMIS_ST7735S,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(ArtemisSt7735sState),
    .class_init = st7735s_class_init,
};

static void st7735s_register_types(void)
{
    type_register_static(&st7735s_info);
}

type_init(st7735s_register_types)
