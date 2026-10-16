/*
 * Wolfson WM8758 audio codec of the iPod touch 1G, driven by AppleWM8758Audio over I2C (I2C1, 0x1a).
 *
 * The codec is write-only: a write transfer is two bytes, the register address in bits 7:1 of the first with bit 8
 * of the value in its bit 0, then bits 7:0 of the value. The driver keeps its own copy of the registers.
 *
 * The codec sets the sample rate and the output level of the I2S controller that feeds it (i2s1). For each rate the
 * driver runs SYSCLK from the PLL (N 10, K 0x7fbe8 for 44.1 kHz and its fractions, 0xeb15aa for 48 kHz and its
 * fractions), and picks MCLKDIV so that SYSCLK (the PLL output / 4 / MCLKDIV) is 256 fs; that puts the codec's
 * MCLK at about 9 MHz. A channel is heard when its DAC, its mixer and the DAC's path into the mixer are on; its level
 * is the DAC's digital volume plus the louder of the powered, unmuted outputs, as the host hears them:
 * - OUT1, the headphones. They follow the volume; without headphones the driver mutes them.
 * - OUT2, the piezo clicker. The 1G has no speaker: its system sounds (lock, unlock, keyboard) are clicks the firmware
 *   synthesises ("piezo-clicker" in SpringBoard's N45AP.plist), which the driver plays on OUT2, kept at 0 dB.
 */
#include "hw/arm/ipod_touch_wm8758.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include <math.h>
#include "trace.h"

static const uint32_t wm8758_rates[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 };
// MCLKDIV, times 2
static const uint8_t wm8758_mclkdiv2[] = { 2, 3, 4, 6, 8, 12, 16, 24 };

// The sample rate the clock registers give (256 fs is SYSCLK), or 0 if it isn't a standard rate.
static uint32_t wm8758_rate(WM8758State *s)
{
    uint16_t clk = s->regs[WM8758_CLK_GEN];
    double sysclk = s->mclk_hz;

    if (clk & 0x100) {
        uint16_t n = s->regs[WM8758_PLL_N];
        uint32_t k = ((s->regs[WM8758_PLL_K1] & 0x3f) << 18) | ((s->regs[WM8758_PLL_K2] & 0x1ff) << 9) |
                     (s->regs[WM8758_PLL_K3] & 0x1ff);
        sysclk = sysclk / ((n & 0x10) ? 2 : 1) * ((n & 0xf) + k / (double)(1 << 24)) / 4;
    }
    double fs = sysclk * 2 / wm8758_mclkdiv2[(clk >> 5) & 7] / 256;

    for (int i = 0; i < ARRAY_SIZE(wm8758_rates); i++) {
        if (fabs(fs - wm8758_rates[i]) < wm8758_rates[i] / 50.0) {
            return wm8758_rates[i];
        }
    }
    return 0;
}

// Gain of an output in dB, or INT_MIN when it is off or muted.
static int wm8758_out_gain(WM8758State *s, bool on, uint8_t vol_reg)
{
    uint16_t vol = s->regs[vol_reg];

    if (!on || (vol & WM8758_VOL_MUTE)) {
        return INT_MIN;
    }
    return (vol & WM8758_VOL_GAIN) - WM8758_VOL_0DB;
}

// Level of one channel in dB, or INT_MIN when it is silent.
static int wm8758_channel_gain(WM8758State *s, int ch)
{
    uint16_t pwr2 = s->regs[WM8758_PWR2], pwr3 = s->regs[WM8758_PWR3];
    uint16_t dac_vol = s->regs[WM8758_DAC_VOL_L + ch] & 0xff;
    bool dac_on = (pwr3 & (1 << ch)) && (pwr3 & (4 << ch)) && (s->regs[WM8758_MIX_L + ch] & 1) &&
                  !(s->regs[WM8758_DAC_CTL] & 0x40) && dac_vol;
    int out = MAX(wm8758_out_gain(s, pwr2 & (ch ? 0x100 : 0x80), WM8758_OUT1_VOL_L + ch),
                  wm8758_out_gain(s, pwr3 & (ch ? 0x20 : 0x40), WM8758_OUT2_VOL_L + ch));

    if (!dac_on || out == INT_MIN) {
        return INT_MIN;
    }
    return out + ((int)dac_vol - 0xff) / 2;
}

static void wm8758_update(WM8758State *s)
{
    uint32_t rate = wm8758_rate(s);
    int left = wm8758_channel_gain(s, 0), right = wm8758_channel_gain(s, 1);

    trace_wm8758_output(rate, left == INT_MIN ? -999 : left, right == INT_MIN ? -999 : right);
    ipod_touch_i2s_set_sample_rate(s->i2s, rate);
    ipod_touch_i2s_set_output_gain(s->i2s, left, right);
}

static void wm8758_reset_regs(WM8758State *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[0x04] = 0x050;
    s->regs[WM8758_CLK_GEN] = 0x140;
    s->regs[WM8758_DAC_VOL_L] = 0xff;
    s->regs[WM8758_DAC_VOL_R] = 0xff;
    s->regs[WM8758_PLL_N] = 0x008;
    s->regs[WM8758_PLL_K1] = 0x00c;
    s->regs[WM8758_PLL_K2] = 0x093;
    s->regs[WM8758_PLL_K3] = 0x0e9;
    s->regs[0x31] = 0x002;
    s->regs[WM8758_MIX_L] = 0x001;
    s->regs[WM8758_MIX_R] = 0x001;
    for (int r = WM8758_OUT1_VOL_L; r <= WM8758_OUT2_VOL_R; r++) {
        s->regs[r] = WM8758_VOL_0DB;
    }
}

static int wm8758_event(I2CSlave *i2c, enum i2c_event event)
{
    WM8758State *s = WM8758(i2c);

    if (event == I2C_START_SEND) {
        s->len = 0;
    }
    return 0;
}

static uint8_t wm8758_recv(I2CSlave *i2c)
{
    qemu_log_mask(LOG_GUEST_ERROR, "wm8758: read, but the codec is write-only\n");
    return 0xff;
}

static int wm8758_send(I2CSlave *i2c, uint8_t data)
{
    WM8758State *s = WM8758(i2c);

    if (s->len >= 2) {
        qemu_log_mask(LOG_GUEST_ERROR, "wm8758: more than two bytes in a write\n");
        return 1;
    }
    s->buf[s->len++] = data;
    if (s->len < 2) {
        return 0;
    }

    uint8_t reg = s->buf[0] >> 1;
    uint16_t val = ((s->buf[0] & 1) << 8) | s->buf[1];

    trace_wm8758_write(reg, val);
    if (reg >= WM8758_NUM_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR, "wm8758: write to register 0x%02x\n", reg);
        return 0;
    }
    if (reg == WM8758_RESET) {
        wm8758_reset_regs(s);
    } else {
        s->regs[reg] = val;
    }
    wm8758_update(s);
    return 0;
}

static void wm8758_reset(DeviceState *dev)
{
    WM8758State *s = WM8758(dev);

    wm8758_reset_regs(s);
    s->len = 0;
    wm8758_update(s);
}

static void wm8758_realize(DeviceState *dev, Error **errp)
{
    WM8758State *s = WM8758(dev);

    if (!s->i2s) {
        error_setg(errp, "wm8758: 'i2s' link not set");
    }
}

static int wm8758_post_load(void *opaque, int version_id)
{
    wm8758_update(opaque);
    return 0;
}

static const Property wm8758_properties[] = {
    DEFINE_PROP_LINK("i2s", WM8758State, i2s, TYPE_IPOD_TOUCH_I2S, IPodTouchI2SState *),
    DEFINE_PROP_UINT32("mclk-hz", WM8758State, mclk_hz, 9000000),
};

static const VMStateDescription vmstate_wm8758 = {
    .name = TYPE_WM8758,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = wm8758_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(i2c, WM8758State),
        VMSTATE_UINT16_ARRAY(regs, WM8758State, WM8758_NUM_REGS),
        VMSTATE_UINT8_ARRAY(buf, WM8758State, 2),
        VMSTATE_UINT8(len, WM8758State),
        VMSTATE_END_OF_LIST()
    }
};

static void wm8758_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = wm8758_event;
    k->recv = wm8758_recv;
    k->send = wm8758_send;
    dc->realize = wm8758_realize;
    device_class_set_legacy_reset(dc, wm8758_reset);
    dc->vmsd = &vmstate_wm8758;
    device_class_set_props(dc, wm8758_properties);
}

static const TypeInfo wm8758_info = {
    .name          = TYPE_WM8758,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(WM8758State),
    .class_init    = wm8758_class_init,
};

static void wm8758_register_types(void)
{
    type_register_static(&wm8758_info);
}

type_init(wm8758_register_types)
