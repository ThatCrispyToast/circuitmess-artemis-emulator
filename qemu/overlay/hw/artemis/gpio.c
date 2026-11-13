/*
 * ESP32-S3 GPIO matrix, IO_MUX and RTC_IO.
 *
 * Models pad levels, the output matrix (so PWM from LEDC reaches the pads),
 * pin interrupts and the sleep wakeup conditions.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/artemis/artemis.h"

#define A_BT_SELECT     0x000
#define A_OUT           0x004
#define A_OUT_W1TS      0x008
#define A_OUT_W1TC      0x00C
#define A_OUT1          0x010
#define A_OUT1_W1TS     0x014
#define A_OUT1_W1TC     0x018
#define A_SDIO_SELECT   0x01C
#define A_ENABLE        0x020
#define A_ENABLE_W1TS   0x024
#define A_ENABLE_W1TC   0x028
#define A_ENABLE1       0x02C
#define A_ENABLE1_W1TS  0x030
#define A_ENABLE1_W1TC  0x034
#define A_STRAP         0x038
#define A_IN            0x03C
#define A_IN1           0x040
#define A_STATUS        0x044
#define A_STATUS_W1TS   0x048
#define A_STATUS_W1TC   0x04C
#define A_STATUS1       0x050
#define A_STATUS1_W1TS  0x054
#define A_STATUS1_W1TC  0x058
#define A_PCPU_INT      0x05C
#define A_PCPU_NMI_INT  0x060
#define A_CPUSDIO_INT   0x064
#define A_PCPU_INT1     0x068
#define A_PCPU_NMI_INT1 0x06C
#define A_CPUSDIO_INT1  0x070
#define A_PIN0          0x074
#define A_PIN53         0x148
#define A_STATUS_NEXT   0x14C
#define A_STATUS_NEXT1  0x150
#define A_FUNC0_IN      0x154
#define A_FUNC255_IN    0x550
#define A_FUNC0_OUT     0x554
#define A_FUNC53_OUT    0x628
#define A_CLOCK_GATE    0x62C
#define A_DATE          0x6FC

#define PIN_PAD_DRIVER   BIT(2)
#define PIN_INT_TYPE_S   7
#define PIN_INT_TYPE_M   (7 << PIN_INT_TYPE_S)
#define PIN_WAKEUP_EN    BIT(10)
#define PIN_INT_ENA_S    13

#define OUT_SEL_MASK     0x1FF
#define OUT_INV          BIT(9)
#define OEN_SEL          BIT(10)
#define OEN_INV          BIT(11)

#define SIG_GPIO_OUT     256
#define SIG_LEDC0        73

#define PIN_U0TXD        43

#define MUX_FUN_PD       BIT(7)
#define MUX_FUN_PU       BIT(8)
#define MUX_FUN_IE       BIT(9)
#define MUX_SEL_S        12
#define MUX_SEL_M        (7 << MUX_SEL_S)

/* RTC_GPIO_IN_REG, RTC_GPIO_OUT_REG and friends. */
#define A_RTC_GPIO_IN    0x24
#define RTCIO_BASE       0x60008400

enum {
    INT_NONE = 0,
    INT_RISING = 1,
    INT_FALLING = 2,
    INT_ANY = 3,
    INT_LOW = 4,
    INT_HIGH = 5,
};

/*
 * Nets held at a level from outside the chip: pull resistors on the watch, and
 * the flash and PSRAM lines as they sit between accesses. The charger's nets
 * are driven by the board model.
 */
static const struct {
    int pin;
    int level;
} board_pulls[] = {
    { ARTEMIS_PIN_I2C_SDA, 1 },
    { ARTEMIS_PIN_I2C_SCL, 1 },
    { ARTEMIS_PIN_LEVER_RIGHT, 0 },
    { ARTEMIS_PIN_LEVER_LEFT, 0 },
    { ARTEMIS_PIN_LEVER_PUSH, 0 },
    { ARTEMIS_PIN_BUTTON, 0 },
    { ARTEMIS_PIN_USB_DETECT, 0 },
    { ARTEMIS_PIN_POWEROFF, 1 },
    /* An LED passes nothing below its forward voltage, so leakage lifts its pad. */
    { ARTEMIS_PIN_LED5, ARTEMIS_GPIO_EXT_WEAK_HIGH },
    { ARTEMIS_PIN_LED6, ARTEMIS_GPIO_EXT_WEAK_HIGH },
    /* The RGB LED's anode is on the supply: the red one drops little enough to read high. */
    { ARTEMIS_PIN_RGB_R, ARTEMIS_GPIO_EXT_WEAK_HIGH },
    { ARTEMIS_PIN_TFT_SDA, ARTEMIS_GPIO_EXT_WEAK_HIGH },
    { 26, 1 }, { 27, 1 }, { 28, 0 }, { 29, 1 }, { 30, 0 }, { 31, 1 }, { 32, 1 },
};

/* IO_MUX registers as a watch has them out of reset. */
static const uint16_t iomux_reset[49] = {
    0xb02, 0xa02, 0xa02, 0xa02, 0x802, 0x802, 0x802, 0x802, 0x802, 0xa02,
    0xa02, 0xa02, 0xa02, 0xa02, 0xa02, 0x802, 0x802, 0xa02, 0xa02, 0xc02,
    0xc02, 0x802, 0x802, 0x802, 0x802, 0x802, 0x1b02, 0x70a, 0x70a, 0x70a,
    0x70a, 0x70a, 0x70a, 0xa02, 0xa02, 0xa02, 0xa02, 0xa02, 0xa02, 0xa02,
    0xa02, 0xa02, 0xa02, 0xb02, 0xb02, 0xa82, 0xa82, 0x1a02, 0x1a02,
};

/*
 * Peripheral inputs start out tied to a constant: which of the 256 signals
 * read high (0x3C) and which low (0x38). The rest have no input.
 */
#define IN_SEL_HIGH 0x3C
#define IN_SEL_LOW  0x38
static const uint32_t in_sel_high[8] = {
    0xffffff8f, 0x7fffffff, 0x001e02fc, 0x0f007fe0,
    0xf9ffffee, 0xf003ffff, 0xe01f00ff, 0xf800001f,
};
static const uint32_t in_sel_low[8] = {
    0x00000000, 0x00000000, 0x1e000000, 0x00100000,
    0x00000000, 0x0ffc0000, 0x1fe00000, 0x00000000,
};

/* LEDs, RGB and the piezo driver: what the UI shows. */
static const int visible_pins[] = {
    ARTEMIS_PIN_LED1, ARTEMIS_PIN_LED2, ARTEMIS_PIN_LED3,
    ARTEMIS_PIN_LED4, ARTEMIS_PIN_LED5, ARTEMIS_PIN_LED6,
    ARTEMIS_PIN_RGB_R, ARTEMIS_PIN_RGB_G, ARTEMIS_PIN_RGB_B,
    ARTEMIS_PIN_TFT_BL, ARTEMIS_PIN_BUZZER,
};

/* UART0's transmitter owns its pad through IO_MUX until firmware moves it. */
static bool pin_is_uart_tx(ArtemisGpioState *s, int pin)
{
    return pin == PIN_U0TXD && !(s->iomux[pin] & MUX_SEL_M);
}

static bool pin_output_enabled(ArtemisGpioState *s, int pin)
{
    uint32_t sel = s->func_out_sel[pin];
    bool en;

    if (pin >= 49) {
        return false;
    }
    if (pin_is_uart_tx(s, pin)) {
        return true;
    }

    /* With OEN_SEL the GPIO_ENABLE bit decides; otherwise the peripheral. */
    if (sel & OEN_SEL || (sel & OUT_SEL_MASK) == SIG_GPIO_OUT) {
        en = s->enable >> pin & 1;
    } else {
        en = s->enable >> pin & 1 ||
             ((sel & OUT_SEL_MASK) >= SIG_LEDC0 &&
              (sel & OUT_SEL_MASK) < SIG_LEDC0 + ARTEMIS_LEDC_CHANNELS);
    }
    return (sel & OEN_INV) ? !en : en;
}

/* What the chip is trying to drive onto the pad, as a duty of 65535. */
static void pin_drive(ArtemisGpioState *s, int pin, uint32_t *level,
                      uint32_t *freq)
{
    uint32_t cfg = s->func_out_sel[pin];
    uint32_t sig = cfg & OUT_SEL_MASK;
    uint32_t duty = 0;
    uint32_t f = 0;

    if (pin_is_uart_tx(s, pin)) {
        /* The line idles high; the bursts of data aren't worth showing. */
        *level = 0xFFFF;
        *freq = 0;
        return;
    }

    if (sig >= SIG_LEDC0 && sig < SIG_LEDC0 + ARTEMIS_LEDC_CHANNELS && s->ledc) {
        artemis_ledc_channel_output(s->ledc, sig - SIG_LEDC0, &duty, &f);
    } else {
        duty = (s->out >> pin & 1) ? 0xFFFF : 0;
    }

    if (cfg & OUT_INV) {
        duty = 0xFFFF - duty;
    }

    *level = duty;
    *freq = f;
}

/* A pad nothing is driving: an outside level, a pull, or else what it last held. */
static bool pin_undriven_level(ArtemisGpioState *s, int pin)
{
    uint32_t mux = s->iomux[pin];

    if (s->ext[pin] >= 0) {
        return s->ext[pin];
    }
    if (mux & MUX_FUN_PU) {
        return true;
    }
    if (mux & MUX_FUN_PD) {
        return false;
    }
    if (s->ext[pin] != ARTEMIS_GPIO_EXT_FLOATING) {
        return s->ext[pin] == ARTEMIS_GPIO_EXT_WEAK_HIGH;
    }
    return s->level >> pin & 1;
}

/* The PWM channel on a pad's output, or -1. */
static int pin_ledc_channel(ArtemisGpioState *s, int pin)
{
    uint32_t sig = s->func_out_sel[pin] & OUT_SEL_MASK;

    if (s->ledc && !pin_is_uart_tx(s, pin) && sig >= SIG_LEDC0 &&
        sig < SIG_LEDC0 + ARTEMIS_LEDC_CHANNELS) {
        return sig - SIG_LEDC0;
    }
    return -1;
}

/*
 * The level on a pad. PWM edges aren't events in this model, so a PWM output
 * counts as its average unless the caller wants the waveform sampled.
 */
static bool pin_input_level(ArtemisGpioState *s, int pin, bool sample_pwm)
{
    if (pin_output_enabled(s, pin)) {
        bool open_drain = s->pin[pin] & PIN_PAD_DRIVER;
        int ch = sample_pwm ? pin_ledc_channel(s, pin) : -1;
        bool high;

        if (ch >= 0) {
            high = artemis_ledc_channel_level(s->ledc, ch);
            if (s->func_out_sel[pin] & OUT_INV) {
                high = !high;
            }
        } else {
            uint32_t duty, freq;

            pin_drive(s, pin, &duty, &freq);
            high = duty >= 0x8000;
        }
        if (high && open_drain) {
            /* Released open-drain line: whoever else holds the net wins. */
            return pin_undriven_level(s, pin);
        }
        return high;
    }

    return pin_undriven_level(s, pin);
}

static uint64_t in_register(ArtemisGpioState *s)
{
    uint64_t v = 0;

    for (int i = 0; i < 49; i++) {
        bool level = s->level >> i & 1;

        if (!(s->iomux[i] & MUX_FUN_IE)) {
            continue;
        }
        if (pin_ledc_channel(s, i) >= 0 && pin_output_enabled(s, i)) {
            level = pin_input_level(s, i, true);
        }
        if (level) {
            v |= 1ULL << i;
        }
    }
    return v;
}

static void update_irq(ArtemisGpioState *s)
{
    uint64_t masked = 0;

    for (int i = 0; i < 49; i++) {
        if ((s->status >> i & 1) && (s->pin[i] >> PIN_INT_ENA_S & 0x1F)) {
            masked |= 1ULL << i;
        }
    }
    qemu_set_irq(s->irq, masked != 0);
}

/* Level-triggered sources stay asserted for as long as the level holds. */
static void reassert_level_interrupts(ArtemisGpioState *s)
{
    for (int i = 0; i < 49; i++) {
        uint32_t type = (s->pin[i] & PIN_INT_TYPE_M) >> PIN_INT_TYPE_S;
        bool lvl = s->level >> i & 1;

        if ((type == INT_HIGH && lvl) || (type == INT_LOW && !lvl)) {
            s->status |= 1ULL << i;
        }
    }
}

static void publish_pin(ArtemisGpioState *s, int pin)
{
    uint32_t level, freq;

    /* A pad that isn't driving shows as low: LEDs off, buzzer quiet. */
    if (pin_output_enabled(s, pin)) {
        pin_drive(s, pin, &level, &freq);
    } else {
        level = 0;
        freq = 0;
    }

    if (s->published_valid[pin] && s->published_level[pin] == level &&
        s->published_freq[pin] == freq) {
        return;
    }
    s->published_valid[pin] = true;
    s->published_level[pin] = level;
    s->published_freq[pin] = freq;

    if (s->pin_notify) {
        s->pin_notify(s->pin_notify_opaque, pin, level, freq);
    }
}

/* Recomputes every pad; called after anything that can move a level. */
static void update_pads(ArtemisGpioState *s)
{
    uint64_t old = s->level;
    uint64_t now = 0;

    for (int i = 0; i < 49; i++) {
        if (pin_input_level(s, i, false)) {
            now |= 1ULL << i;
        }
    }
    s->level = now;

    uint64_t changed = old ^ now;
    for (int i = 0; changed && i < 49; i++) {
        if (!(changed >> i & 1)) {
            continue;
        }
        bool lvl = now >> i & 1;
        uint32_t type = (s->pin[i] & PIN_INT_TYPE_M) >> PIN_INT_TYPE_S;

        if (type == INT_ANY || (type == INT_RISING && lvl) ||
            (type == INT_FALLING && !lvl)) {
            s->status |= 1ULL << i;
        }
        qemu_set_irq(s->output_line[i], lvl);
    }

    reassert_level_interrupts(s);
    update_irq(s);

    for (size_t i = 0; i < ARRAY_SIZE(visible_pins); i++) {
        publish_pin(s, visible_pins[i]);
    }

    if (changed && s->wake_notify) {
        s->wake_notify(s->wake_notify_opaque);
    }

    /* The board reacts to what the chip drives and pulls, not only to levels. */
    uint64_t driving = 0, pulling = 0;
    for (int i = 0; i < 49; i++) {
        if (pin_output_enabled(s, i)) {
            driving |= 1ULL << i;
        }
        if (artemis_gpio_pull(s, i)) {
            pulling |= 1ULL << i;
        }
    }
    if ((changed || driving != s->driving || pulling != s->pulling) &&
        s->drive_notify && !s->in_drive_notify) {
        s->driving = driving;
        s->pulling = pulling;
        s->in_drive_notify = true;
        s->drive_notify(s->drive_notify_opaque);
        s->in_drive_notify = false;
    }
    s->driving = driving;
    s->pulling = pulling;
}

void artemis_gpio_set_external(ArtemisGpioState *s, int pin, int level)
{
    assert(pin >= 0 && pin < ARTEMIS_GPIO_COUNT);
    s->ext[pin] = level < 0 ? level : !!level;
    update_pads(s);
}

int artemis_gpio_pull(ArtemisGpioState *s, int pin)
{
    return s->iomux[pin] & MUX_FUN_PU ? 1 : s->iomux[pin] & MUX_FUN_PD ? -1 : 0;
}

bool artemis_gpio_get_level(ArtemisGpioState *s, int pin)
{
    return s->level >> pin & 1;
}

bool artemis_gpio_is_output(ArtemisGpioState *s, int pin)
{
    return pin_output_enabled(s, pin);
}

void artemis_gpio_refresh_outputs(ArtemisGpioState *s)
{
    update_pads(s);
}

void artemis_gpio_republish(ArtemisGpioState *s)
{
    for (size_t i = 0; i < ARRAY_SIZE(visible_pins); i++) {
        s->published_valid[visible_pins[i]] = false;
    }
    update_pads(s);
}

bool artemis_gpio_wakeup_pending(ArtemisGpioState *s)
{
    for (int i = 0; i < 49; i++) {
        if (!(s->pin[i] & PIN_WAKEUP_EN)) {
            continue;
        }
        uint32_t type = (s->pin[i] & PIN_INT_TYPE_M) >> PIN_INT_TYPE_S;
        bool lvl = s->level >> i & 1;

        if ((type == INT_HIGH && lvl) || (type == INT_LOW && !lvl)) {
            return true;
        }
    }
    return false;
}

bool artemis_gpio_ext1_pending(ArtemisGpioState *s, uint32_t mask, bool any_high)
{
    bool all_low = true;

    for (int i = 0; i < 22; i++) {
        if (!(mask >> i & 1)) {
            continue;
        }
        if (s->level >> i & 1) {
            all_low = false;
            if (any_high) {
                return true;
            }
        }
    }
    return !any_high && mask != 0 && all_low;
}

/* ------------------------------------------------------------------------ */

static uint64_t gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    ArtemisGpioState *s = ARTEMIS_GPIO(opaque);
    uint64_t in;

    switch (addr) {
    case A_BT_SELECT:
        return s->bt_select;
    case A_OUT:
        return (uint32_t)s->out;
    case A_OUT1:
        return s->out >> 32;
    case A_SDIO_SELECT:
        return s->sdio_select;
    case A_ENABLE:
        return (uint32_t)s->enable;
    case A_ENABLE1:
        return s->enable >> 32;
    case A_STRAP:
        return s->strap_mode;
    case A_IN:
        in = in_register(s);
        return (uint32_t)in;
    case A_IN1:
        in = in_register(s);
        return in >> 32;
    case A_STATUS:
        return (uint32_t)s->status;
    case A_STATUS1:
        return s->status >> 32;
    case A_PCPU_INT:
    case A_PCPU_NMI_INT:
    case A_CPUSDIO_INT: {
        uint32_t v = 0;
        for (int i = 0; i < 32; i++) {
            if ((s->status >> i & 1) && (s->pin[i] >> PIN_INT_ENA_S & 0x1F)) {
                v |= 1U << i;
            }
        }
        return addr == A_PCPU_INT ? v : 0;
    }
    case A_PCPU_INT1:
    case A_PCPU_NMI_INT1:
    case A_CPUSDIO_INT1: {
        uint32_t v = 0;
        for (int i = 32; i < 49; i++) {
            if ((s->status >> i & 1) && (s->pin[i] >> PIN_INT_ENA_S & 0x1F)) {
                v |= 1U << (i - 32);
            }
        }
        return addr == A_PCPU_INT1 ? v : 0;
    }
    case A_PIN0 ... A_PIN53:
        return s->pin[(addr - A_PIN0) / 4];
    case A_STATUS_NEXT:
        return (uint32_t)s->status;
    case A_STATUS_NEXT1:
        return s->status >> 32;
    case A_FUNC0_IN ... A_FUNC255_IN:
        return s->func_in_sel[(addr - A_FUNC0_IN) / 4];
    case A_FUNC0_OUT ... A_FUNC53_OUT:
        return s->func_out_sel[(addr - A_FUNC0_OUT) / 4];
    case A_CLOCK_GATE:
        return s->clock_gate;
    case A_DATE:
        return 0x1907040;
    default:
        return 0;
    }
}

static void gpio_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ArtemisGpioState *s = ARTEMIS_GPIO(opaque);
    uint32_t v = value;

    switch (addr) {
    case A_BT_SELECT:
        s->bt_select = v;
        return;
    case A_OUT:
        s->out = (s->out & ~0xFFFFFFFFULL) | v;
        break;
    case A_OUT_W1TS:
        s->out |= v;
        break;
    case A_OUT_W1TC:
        s->out &= ~(uint64_t)v;
        break;
    case A_OUT1:
        s->out = (s->out & 0xFFFFFFFFULL) | ((uint64_t)v << 32);
        break;
    case A_OUT1_W1TS:
        s->out |= (uint64_t)v << 32;
        break;
    case A_OUT1_W1TC:
        s->out &= ~((uint64_t)v << 32);
        break;
    case A_SDIO_SELECT:
        s->sdio_select = v;
        return;
    case A_ENABLE:
        s->enable = (s->enable & ~0xFFFFFFFFULL) | v;
        break;
    case A_ENABLE_W1TS:
        s->enable |= v;
        break;
    case A_ENABLE_W1TC:
        s->enable &= ~(uint64_t)v;
        break;
    case A_ENABLE1:
        s->enable = (s->enable & 0xFFFFFFFFULL) | ((uint64_t)v << 32);
        break;
    case A_ENABLE1_W1TS:
        s->enable |= (uint64_t)v << 32;
        break;
    case A_ENABLE1_W1TC:
        s->enable &= ~((uint64_t)v << 32);
        break;
    case A_STRAP:
        return;
    case A_STATUS:
        s->status = (s->status & ~0xFFFFFFFFULL) | v;
        break;
    case A_STATUS_W1TS:
        s->status |= v;
        break;
    case A_STATUS_W1TC:
        s->status &= ~(uint64_t)v;
        break;
    case A_STATUS1:
        s->status = (s->status & 0xFFFFFFFFULL) | ((uint64_t)v << 32);
        break;
    case A_STATUS1_W1TS:
        s->status |= (uint64_t)v << 32;
        break;
    case A_STATUS1_W1TC:
        s->status &= ~((uint64_t)v << 32);
        break;
    case A_PIN0 ... A_PIN53:
        s->pin[(addr - A_PIN0) / 4] = v;
        break;
    case A_FUNC0_IN ... A_FUNC255_IN:
        s->func_in_sel[(addr - A_FUNC0_IN) / 4] = v;
        return;
    case A_FUNC0_OUT ... A_FUNC53_OUT:
        s->func_out_sel[(addr - A_FUNC0_OUT) / 4] = v;
        break;
    case A_CLOCK_GATE:
        s->clock_gate = v;
        return;
    default:
        return;
    }

    update_pads(s);
}

static const MemoryRegionOps gpio_ops = {
    .read = gpio_read,
    .write = gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static uint64_t iomux_read(void *opaque, hwaddr addr, unsigned size)
{
    ArtemisGpioState *s = ARTEMIS_GPIO(opaque);

    if (addr == 0) {
        return s->pin_ctrl;
    }
    if (addr >= 4 && addr < 4 + 4 * ARTEMIS_GPIO_COUNT) {
        return s->iomux[(addr - 4) / 4];
    }
    return 0;
}

static void iomux_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ArtemisGpioState *s = ARTEMIS_GPIO(opaque);

    if (addr == 0) {
        s->pin_ctrl = value;
    } else if (addr >= 4 && addr < 4 + 4 * ARTEMIS_GPIO_COUNT) {
        s->iomux[(addr - 4) / 4] = value;
        update_pads(s);
    }
}

static const MemoryRegionOps iomux_ops = {
    .read = iomux_read,
    .write = iomux_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static uint64_t rtcio_read(void *opaque, hwaddr addr, unsigned size)
{
    ArtemisGpioState *s = ARTEMIS_GPIO(opaque);

    if (addr == A_RTC_GPIO_IN) {
        /* RTC pads 0-21 report from bit 10 upward. */
        uint64_t in = s->level & 0x3FFFFF;
        return (uint32_t)(in << 10);
    }
    return s->rtcio[(addr / 4) & 0xFF];
}

static void rtcio_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ArtemisGpioState *s = ARTEMIS_GPIO(opaque);

    s->rtcio[(addr / 4) & 0xFF] = value;
}

static const MemoryRegionOps rtcio_ops = {
    .read = rtcio_read,
    .write = rtcio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void gpio_reset_hold(Object *obj, ResetType type)
{
    ArtemisGpioState *s = ARTEMIS_GPIO(obj);

    s->out = 0;
    s->enable = 0;
    s->status = 0;
    s->bt_select = 0;
    s->sdio_select = 0;
    s->clock_gate = 1;
    s->pin_ctrl = 0x27FF;
    memset(s->pin, 0, sizeof(s->pin));
    memset(s->rtcio, 0, sizeof(s->rtcio));
    artemis_socregs_load(RTCIO_BASE, s->rtcio, ARRAY_SIZE(s->rtcio));
    for (int i = 0; i < 256; i++) {
        s->func_in_sel[i] = (in_sel_high[i / 32] >> (i % 32) & 1) ? IN_SEL_HIGH :
                            (in_sel_low[i / 32] >> (i % 32) & 1) ? IN_SEL_LOW : 0;
    }
    for (int i = 0; i < ARTEMIS_GPIO_COUNT; i++) {
        s->func_out_sel[i] = SIG_GPIO_OUT;
        s->iomux[i] = i < ARRAY_SIZE(iomux_reset) ? iomux_reset[i] : 0x802;
    }
}

static void gpio_reset_exit(Object *obj, ResetType type)
{
    ArtemisGpioState *s = ARTEMIS_GPIO(obj);

    s->level = 0;
    update_pads(s);
    /* Whatever is wired to the pads has to see the new levels after a reset. */
    for (int i = 0; i < 49; i++) {
        qemu_set_irq(s->output_line[i], s->level >> i & 1);
    }
}

static void gpio_realize(DeviceState *dev, Error **errp)
{
    ArtemisGpioState *s = ARTEMIS_GPIO(dev);

    for (int i = 0; i < ARTEMIS_GPIO_COUNT; i++) {
        s->ext[i] = ARTEMIS_GPIO_EXT_FLOATING;
    }
    for (size_t i = 0; i < ARRAY_SIZE(board_pulls); i++) {
        s->ext[board_pulls[i].pin] = board_pulls[i].level;
    }
}

static void gpio_init(Object *obj)
{
    ArtemisGpioState *s = ARTEMIS_GPIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &gpio_ops, s, "artemis.gpio", 0xF00);
    memory_region_init_io(&s->iomux_mem, obj, &iomux_ops, s, "artemis.iomux",
                          0x1000);
    memory_region_init_io(&s->rtcio_mem, obj, &rtcio_ops, s, "artemis.rtcio",
                          0x400);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_mmio(sbd, &s->iomux_mem);
    sysbus_init_mmio(sbd, &s->rtcio_mem);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_out_named(DEVICE(obj), s->output_line, "pad",
                             ARTEMIS_GPIO_COUNT);
}

static Property gpio_properties[] = {
    /* GPIO0 high and the other strapping pads low, as the watch boots. */
    DEFINE_PROP_UINT32("strap_mode", ArtemisGpioState, strap_mode, 0x8),
    DEFINE_PROP_END_OF_LIST(),
};

static void gpio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = gpio_realize;
    rc->phases.hold = gpio_reset_hold;
    rc->phases.exit = gpio_reset_exit;
    device_class_set_props(dc, gpio_properties);
}

static const TypeInfo gpio_info = {
    .name = TYPE_ARTEMIS_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ArtemisGpioState),
    .instance_init = gpio_init,
    .class_init = gpio_class_init,
};

static void gpio_register_types(void)
{
    type_register_static(&gpio_info);
}

type_init(gpio_register_types)
