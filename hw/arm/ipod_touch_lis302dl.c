#include "hw/arm/ipod_touch_lis302dl.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/log.h"

static const char *axis_names[3] = { "x", "y", "z" };

// Converts an acceleration in mg to the signed 8-bit output register value.
static uint8_t lis302dl_output(LIS302DLState *s, int axis)
{
    int32_t val;

    // the output registers are not updated in power-down mode or when the axis is disabled
    if (!(s->ctrl_reg1 & ACCEL_CTRL1_PD) || !(s->ctrl_reg1 & (ACCEL_CTRL1_XEN << axis))) {
        return 0;
    }

    val = s->accel_mg[axis] / ACCEL_MG_PER_LSB;
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
}

static void lis302dl_init(Object *obj)
{
    LIS302DLState *s = LIS302DL(obj);

    // By default, the device is lying flat and face-up so gravity only acts on the Z axis.
    // Note that the kernel negates X and Z (device tree "orientation" = 5), so +Z here becomes -1g for iOS
    // and setting X to +/- 1000 (with Z = 0) corresponds to the landscape orientations.
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
