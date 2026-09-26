#include "hw/arm/ipod_touch_isl29003dl.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "hw/qdev-core.h"

// full-scale lux range for each range setting, with the nominal 100 kOhm external scaling resistor
static const uint32_t range_fs_lux[4] = { 1000, 4000, 16000, 64000 };

// ADC resolution (in bits) for each width setting
static const uint8_t width_bits[4] = { 16, 12, 8, 4 };

// the ISL29003 on this device uses a 499 kOhm external scaling resistor, which scales the range with a factor 4.99
#define LIGHTSENSOR_REXT_SCALE_NUM 499
#define LIGHTSENSOR_REXT_SCALE_DEN 100

static bool isl29003dl_is_running(ISL29003DLState *s)
{
    uint8_t cmd = s->regs[LIGHTSENSOR_COMMAND];
    return (cmd & LIGHTSENSOR_CMD_ENABLE) && !(cmd & LIGHTSENSOR_CMD_ADC_PD);
}

static uint8_t isl29003dl_width_bits(ISL29003DLState *s)
{
    return width_bits[s->regs[LIGHTSENSOR_COMMAND] & LIGHTSENSOR_CMD_WIDTH_MASK];
}

// Returns the time it takes to complete a single conversion.
static int64_t isl29003dl_conversion_time_ns(ISL29003DLState *s)
{
    return ((int64_t)1 << isl29003dl_width_bits(s)) * NANOSECONDS_PER_SECOND / LIGHTSENSOR_ADC_CLOCK_HZ;
}

// Returns the ADC count of diode 1 (visible + infrared) for the current light level.
static uint16_t isl29003dl_d1_count(ISL29003DLState *s)
{
    uint8_t bits = isl29003dl_width_bits(s);
    uint64_t fs_lux = (uint64_t)range_fs_lux[s->regs[LIGHTSENSOR_CONTROL] & LIGHTSENSOR_CTRL_RANGE_MASK] * LIGHTSENSOR_REXT_SCALE_NUM / LIGHTSENSOR_REXT_SCALE_DEN;
    uint64_t count = ((uint64_t)s->lux << bits) / fs_lux;
    return MIN(count, (1 << bits) - 1);
}

static void isl29003dl_update_irq(ISL29003DLState *s)
{
    qemu_set_irq(s->irq, !!(s->regs[LIGHTSENSOR_CONTROL] & LIGHTSENSOR_CTRL_INT_FLAG));
}

static void isl29003dl_update_timer(ISL29003DLState *s)
{
    if (!isl29003dl_is_running(s)) {
        timer_del(s->timer);
    } else if (!timer_pending(s->timer)) {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + isl29003dl_conversion_time_ns(s));
    }
}

// Completes a conversion: latches the result of the selected diode and raises an interrupt when it is out of the threshold window.
static void isl29003dl_conversion_done(void *opaque)
{
    ISL29003DLState *s = opaque;
    uint16_t data;
    uint8_t msb;

    switch (s->regs[LIGHTSENSOR_COMMAND] & LIGHTSENSOR_CMD_MODE_MASK) {
        case LIGHTSENSOR_CMD_MODE_D2:
            data = 0; // we do not model infrared light
            break;
        case LIGHTSENSOR_CMD_MODE_D1:
        case LIGHTSENSOR_CMD_MODE_D1_D2:
        default:
            data = isl29003dl_d1_count(s);
            break;
    }
    s->regs[LIGHTSENSOR_LSB_SENSOR] = data & 0xFF;
    s->regs[LIGHTSENSOR_MSB_SENSOR] = data >> 8;

    // the interrupt thresholds are compared with the eight most significant bits of the 16-bit result
    msb = data >> 8;
    if (msb > s->regs[LIGHTSENSOR_IT_HI] || msb < s->regs[LIGHTSENSOR_IT_LO]) {
        s->regs[LIGHTSENSOR_CONTROL] |= LIGHTSENSOR_CTRL_INT_FLAG;
        isl29003dl_update_irq(s);
    }

    isl29003dl_update_timer(s);
}

static void isl29003dl_write_reg(ISL29003DLState *s, uint8_t reg, uint8_t val)
{
    switch (reg) {
        case LIGHTSENSOR_COMMAND:
            s->regs[reg] = val;
            isl29003dl_update_timer(s);
            break;
        case LIGHTSENSOR_CONTROL:
            // the interrupt flag can only be cleared by software
            s->regs[reg] = (val & ~LIGHTSENSOR_CTRL_INT_FLAG) | (val & s->regs[reg] & LIGHTSENSOR_CTRL_INT_FLAG);
            isl29003dl_update_irq(s);
            break;
        case LIGHTSENSOR_IT_HI:
        case LIGHTSENSOR_IT_LO:
            s->regs[reg] = val;
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR, "%s: write to read-only register 0x%02x (value 0x%02x)\n", __func__, reg, val);
            break;
    }
}

static int isl29003dl_event(I2CSlave *i2c, enum i2c_event event)
{
    ISL29003DLState *s = ISL29003DL(i2c);

    if (event == I2C_START_SEND) {
        // the first byte of a write transfer selects the register
        s->reg_written = false;
    }
    return 0;
}

static uint8_t isl29003dl_recv(I2CSlave *i2c)
{
    ISL29003DLState *s = ISL29003DL(i2c);
    uint8_t val = s->regs[s->reg];

    s->reg = (s->reg + 1) % LIGHTSENSOR_NUM_REGS;
    return val;
}

static int isl29003dl_send(I2CSlave *i2c, uint8_t data)
{
    ISL29003DLState *s = ISL29003DL(i2c);

    if (!s->reg_written) {
        // only the lower bits of the register address are decoded (e.g., the kernel clears the interrupt flag through register 0x41)
        s->reg = data % LIGHTSENSOR_NUM_REGS;
        s->reg_written = true;
        return 0;
    }

    isl29003dl_write_reg(s, s->reg, data);
    s->reg = (s->reg + 1) % LIGHTSENSOR_NUM_REGS;
    return 0;
}

static void isl29003dl_reset(DeviceState *dev)
{
    ISL29003DLState *s = ISL29003DL(dev);

    timer_del(s->timer);
    memset(s->regs, 0, sizeof(s->regs));
    s->reg = 0;
    s->reg_written = false;
    isl29003dl_update_irq(s);
}

static void isl29003dl_init(Object *obj)
{
    ISL29003DLState *s = ISL29003DL(obj);

    qdev_init_gpio_out(DEVICE(obj), &s->irq, 1);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, isl29003dl_conversion_done, s);

    // typical indoor light level, can be changed at runtime, e.g., with qom-set
    s->lux = 300;
    object_property_add_uint32_ptr(obj, "lux", &s->lux, OBJ_PROP_FLAG_READWRITE);
}

static void isl29003dl_finalize(Object *obj)
{
    ISL29003DLState *s = ISL29003DL(obj);
    timer_free(s->timer);
}

static void isl29003dl_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = isl29003dl_event;
    k->recv = isl29003dl_recv;
    k->send = isl29003dl_send;
    dc->reset = isl29003dl_reset;
}

static const TypeInfo isl29003dl_info = {
    .name          = TYPE_ISL29003DL,
    .parent        = TYPE_I2C_SLAVE,
    .instance_init = isl29003dl_init,
    .instance_finalize = isl29003dl_finalize,
    .instance_size = sizeof(ISL29003DLState),
    .class_init    = isl29003dl_class_init,
};

static void isl29003dl_register_types(void)
{
    type_register_static(&isl29003dl_info);
}

type_init(isl29003dl_register_types)
