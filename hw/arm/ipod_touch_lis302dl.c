#include "hw/arm/ipod_touch_lis302dl.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/log.h"

static const char *axis_names[3] = { "x", "y", "z" };

// Returns noise in [-ACCEL_NOISE_LSB, ACCEL_NOISE_LSB], from a deterministic linear congruential generator.
static int32_t lis302dl_noise(LIS302DLState *s)
{
    s->noise_seed = s->noise_seed * 1103515245 + 12345;
    return (int32_t)((s->noise_seed >> 16) % (2 * ACCEL_NOISE_LSB + 1)) - ACCEL_NOISE_LSB;
}

// Converts an acceleration in mg to the signed 8-bit output register value.
static uint8_t lis302dl_output(LIS302DLState *s, int axis)
{
    int32_t val;

    // the output registers are not updated in power-down mode or when the axis is disabled
    if (!(s->ctrl_reg1 & ACCEL_CTRL1_PD) || !(s->ctrl_reg1 & (ACCEL_CTRL1_XEN << axis))) {
        return 0;
    }

    val = s->accel_mg[axis] / ACCEL_MG_PER_LSB + lis302dl_noise(s);
    return (uint8_t)(int8_t)MAX(INT8_MIN, MIN(INT8_MAX, val));
}

static uint8_t lis302dl_read_reg(LIS302DLState *s, uint8_t reg)
{
    switch (reg) {
        case ACCEL_WHOAMI:
            return ACCEL_WHOAMI_VALUE;
        case ACCEL_CTRL_REG1:
            return s->ctrl_reg1;
        case ACCEL_CTRL_REG2:
            return s->ctrl_reg2;
        case ACCEL_CTRL_REG3:
            return s->ctrl_reg3;
        case ACCEL_STATUS:
            return (s->ctrl_reg1 & ACCEL_CTRL1_PD) ? ACCEL_STATUS_ZYXDA : 0;
        case ACCEL_OUT_X:
            return lis302dl_output(s, 0);
        case ACCEL_OUT_Y:
            return lis302dl_output(s, 1);
        case ACCEL_OUT_Z:
            return lis302dl_output(s, 2);
        default:
            qemu_log_mask(LOG_UNIMP, "%s: unknown register 0x%02x\n", __func__, reg);
            return 0;
    }
}

static void lis302dl_write_reg(LIS302DLState *s, uint8_t reg, uint8_t val)
{
    switch (reg) {
        case ACCEL_CTRL_REG1:
            s->ctrl_reg1 = val;
            break;
        case ACCEL_CTRL_REG2:
            s->ctrl_reg2 = val;
            break;
        case ACCEL_CTRL_REG3:
            s->ctrl_reg3 = val;
            break;
        default:
            qemu_log_mask(LOG_UNIMP, "%s: unknown register 0x%02x (value 0x%02x)\n", __func__, reg, val);
            break;
    }
}

static int lis302dl_event(I2CSlave *i2c, enum i2c_event event)
{
    LIS302DLState *s = LIS302DL(i2c);

    if (event == I2C_START_SEND) {
        // the first byte of a write transfer selects the register
        s->reg_written = false;
    }
    return 0;
}

static uint8_t lis302dl_recv(I2CSlave *i2c)
{
    LIS302DLState *s = LIS302DL(i2c);
    uint8_t val = lis302dl_read_reg(s, s->reg);

    if (s->autoinc) {
        s->reg++;
    }
    return val;
}

static int lis302dl_send(I2CSlave *i2c, uint8_t data)
{
    LIS302DLState *s = LIS302DL(i2c);

    if (!s->reg_written) {
        s->reg = data & ~ACCEL_SUBADDR_AUTOINC;
        s->autoinc = data & ACCEL_SUBADDR_AUTOINC;
        s->reg_written = true;
        return 0;
    }

    lis302dl_write_reg(s, s->reg, data);
    if (s->autoinc) {
        s->reg++;
    }
    return 0;
}

// Turns the device a quarter turn counter-clockwise, starting from upright portrait; returns the new
// orientation. iOS keeps its orientation while the device lies flat, which is where it starts.
const char *lis302dl_rotate(LIS302DLState *s)
{
    static const struct {
        int32_t x, y;
        const char *name;
    } orientations[] = {
        { 0, -1000, "portrait" },
        { 1000, 0, "landscape, home button right" },
        { 0, 1000, "portrait upside down" },
        { -1000, 0, "landscape, home button left" },
    };

    s->orientation = (s->orientation + 1) % ARRAY_SIZE(orientations);
    s->accel_mg[0] = orientations[s->orientation].x;
    s->accel_mg[1] = orientations[s->orientation].y;
    s->accel_mg[2] = 0;
    return orientations[s->orientation].name;
}

// getter and setter of the acceleration properties
static void lis302dl_visit_accel(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    visit_type_int32(v, name, opaque, errp);
}

static void lis302dl_reset(DeviceState *dev)
{
    LIS302DLState *s = LIS302DL(dev);

    s->reg = 0;
    s->autoinc = false;
    s->reg_written = false;
    s->ctrl_reg1 = ACCEL_CTRL1_RESET_VALUE;
    s->ctrl_reg2 = 0;
    s->ctrl_reg3 = 0;
    s->noise_seed = 1;
}

static void lis302dl_init(Object *obj)
{
    LIS302DLState *s = LIS302DL(obj);

    // By default, the device is lying flat and face-up so gravity only acts on the Z axis.
    // Note that the kernel negates X and Z (device tree "orientation" = 5), so +Z here becomes -1g for iOS.
    // Held upright in portrait, Y is -1000; setting X to +/- 1000 (with Y and Z = 0) gives the landscape orientations.
    s->orientation = -1;
    s->accel_mg[0] = 0;
    s->accel_mg[1] = 0;
    s->accel_mg[2] = 1000;

    // the acceleration along each axis (in mg) can be changed at runtime, e.g., with qom-set
    for (int i = 0; i < 3; i++) {
        object_property_add(obj, axis_names[i], "int32", lis302dl_visit_accel, lis302dl_visit_accel, NULL, &s->accel_mg[i]);
    }
}

static void lis302dl_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = lis302dl_event;
    k->recv = lis302dl_recv;
    k->send = lis302dl_send;
    dc->reset = lis302dl_reset;
}

static const TypeInfo lis302dl_info = {
    .name          = TYPE_LIS302DL,
    .parent        = TYPE_I2C_SLAVE,
    .instance_init = lis302dl_init,
    .instance_size = sizeof(LIS302DLState),
    .class_init    = lis302dl_class_init,
};

static void lis302dl_register_types(void)
{
    type_register_static(&lis302dl_info);
}

type_init(lis302dl_register_types)
