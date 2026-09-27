/*
 * Cirrus Logic CS42L58 audio codec, driven by AppleCS42L58Audio over I2C.
 *
 * A write transfer starts with the register address (bit 7 selects auto-increment) followed by data bytes; a read
 * transfer returns registers from the last address written.
 *
 * The codec sets the sample rate and the output level of the I2S controller that feeds it. The driver drives one of
 * two output paths: path 1 (volume 0x1A) for the speaker, which on this device is fed from the codec's headphone
 * output (the "hpout-spkr" property), and path 2 (volume 0x1C) for the headphones. It only writes 0x1B/0x1D at
 * initialisation, to mute them, so the path volume applies to both channels. The host hears the louder of the powered,
 * unmuted paths. The speaker path includes the fixed gain of the external speaker amplifier (LM48821, whose I2C
 * interface is not modelled; the device tree gives "external-gain-db" = 22), which the driver compensates for.
 */
#include "hw/arm/ipod_touch_cs42l58.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "trace.h"

static const struct {
    uint8_t code;
    uint32_t rate;
} cs42l58_rates[] = {
    { 0x1D, 8000 }, { 0x1B, 11025 }, { 0x19, 12000 }, { 0x15, 16000 }, { 0x13, 22050 },
    { 0x11, 24000 }, { 0x0D, 32000 }, { 0x0B, 44100 }, { 0x09, 48000 },
};

// Gain of an output path in dB, or INT_MIN when it is off or muted.
static int cs42l58_path_gain(CS42L58State *s, int power_shift, uint8_t vol_reg, int external_db)
{
    uint8_t power = (s->regs[CS42L58_PWR_OUT] >> power_shift) & 0xF;
    uint8_t vol = s->regs[vol_reg];

    if (power == 0xF || (vol & CS42L58_VOL_MUTE)) {
        return INT_MIN;
    }
    return sextract32(vol, 0, 7) + external_db;
}

static void cs42l58_update(CS42L58State *s)
{
    int db = MAX(cs42l58_path_gain(s, 4, CS42L58_VOL_PATH1, s->speaker_gain_db),
                 cs42l58_path_gain(s, 0, CS42L58_VOL_PATH2, 0));
    bool on = !(s->regs[CS42L58_PWR_CTL1] & CS42L58_PWR_CTL1_PDN) && db != INT_MIN;
    uint8_t mute = s->regs[CS42L58_PB_CTL];
    uint8_t code = s->regs[CS42L58_CLK_CTL] & CS42L58_CLK_RATE_MASK;

    for (int i = 0; i < ARRAY_SIZE(cs42l58_rates); i++) {
        if (cs42l58_rates[i].code == code) {
            ipod_touch_i2s_set_sample_rate(s->i2s, cs42l58_rates[i].rate);
        }
    }
    trace_cs42l58_output(on ? db : -999, mute & 3);
    ipod_touch_i2s_set_output_gain(s->i2s, on && !(mute & CS42L58_PB_CTL_MUTE_A) ? db : INT_MIN,
                                   on && !(mute & CS42L58_PB_CTL_MUTE_B) ? db : INT_MIN);
}

static int cs42l58_event(I2CSlave *i2c, enum i2c_event event)
{
    CS42L58State *s = CS42L58(i2c);

    if (event == I2C_START_SEND) {
        s->map_written = false;
    }
    return 0;
}

static void cs42l58_next(CS42L58State *s)
{
    if (s->incr) {
        s->map = (s->map + 1) % CS42L58_NUM_REGS;
    }
}

static uint8_t cs42l58_recv(I2CSlave *i2c)
{
    CS42L58State *s = CS42L58(i2c);
    uint8_t val = s->regs[s->map];

    trace_cs42l58_read(s->map, val);
    cs42l58_next(s);
    return val;
}

static int cs42l58_send(I2CSlave *i2c, uint8_t data)
{
    CS42L58State *s = CS42L58(i2c);

    if (!s->map_written) {
        s->map = data & (CS42L58_NUM_REGS - 1);
        s->incr = data & CS42L58_MAP_INCR;
        s->map_written = true;
        return 0;
    }
    trace_cs42l58_write(s->map, data);
    s->regs[s->map] = data;
    switch (s->map) {
    case CS42L58_PWR_CTL1:
    case CS42L58_PWR_OUT:
    case CS42L58_CLK_CTL:
    case CS42L58_PB_CTL:
    case CS42L58_VOL_PATH1:
    case CS42L58_VOL_PATH2:
        cs42l58_update(s);
        break;
    }
    cs42l58_next(s);
    return 0;
}

static void cs42l58_reset(DeviceState *dev)
{
    CS42L58State *s = CS42L58(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[CS42L58_PWR_CTL1] = CS42L58_PWR_CTL1_PDN;
    s->regs[CS42L58_PWR_OUT] = 0xFF;
    s->regs[CS42L58_CLK_CTL] = 0x0B; // 44.1 kHz
    s->map = 0;
    s->incr = false;
    s->map_written = false;
    cs42l58_update(s);
}

static void cs42l58_realize(DeviceState *dev, Error **errp)
{
    CS42L58State *s = CS42L58(dev);

    if (!s->i2s) {
        error_setg(errp, "cs42l58: 'i2s' link not set");
    }
}

static int cs42l58_post_load(void *opaque, int version_id)
{
    cs42l58_update(opaque);
    return 0;
}

static Property cs42l58_properties[] = {
    DEFINE_PROP_LINK("i2s", CS42L58State, i2s, TYPE_IPOD_TOUCH_I2S, IPodTouchI2SState *),
    DEFINE_PROP_INT32("speaker-gain-db", CS42L58State, speaker_gain_db, 22),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_cs42l58 = {
    .name = TYPE_CS42L58,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = cs42l58_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_I2C_SLAVE(i2c, CS42L58State),
        VMSTATE_UINT8_ARRAY(regs, CS42L58State, CS42L58_NUM_REGS),
        VMSTATE_UINT8(map, CS42L58State),
        VMSTATE_BOOL(incr, CS42L58State),
        VMSTATE_BOOL(map_written, CS42L58State),
        VMSTATE_END_OF_LIST()
    }
};

static void cs42l58_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = cs42l58_event;
    k->recv = cs42l58_recv;
    k->send = cs42l58_send;
    dc->realize = cs42l58_realize;
    dc->reset = cs42l58_reset;
    dc->vmsd = &vmstate_cs42l58;
    device_class_set_props(dc, cs42l58_properties);
}

static const TypeInfo cs42l58_info = {
    .name          = TYPE_CS42L58,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(CS42L58State),
    .class_init    = cs42l58_class_init,
};

static void cs42l58_register_types(void)
{
    type_register_static(&cs42l58_info);
}

type_init(cs42l58_register_types)
