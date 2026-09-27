#include "hw/arm/ipod_touch_pcf50633_pmu.h"
#include "hw/arm/ipod_touch_lcd.h"
#include "trace.h"

// The first byte of a write sets the register pointer, further bytes write to
// the register it points at. Reads start at the register pointer. Both
// advance the pointer.
static int pcf50633_event(I2CSlave *i2c, enum i2c_event event)
{
    Pcf50633State *s = PCF50633(i2c);
    trace_ipod_touch_pmu_event(event, s->cmd);

    if (event == I2C_START_SEND)
        s->pointer_set = false;

    return 0;
}

static uint8_t pcf50633_recv(I2CSlave *i2c)
{
    Pcf50633State *s = PCF50633(i2c);
    int res = 0;

    switch(s->cmd) {
        case PMU_STATUS_A:
            res = s->usb_present ? PMU_STATUS_A_USB_PRESENT : 0;
            break;
        case PMU_MBCS1:
            res = 1; // battery power source
            break;
        case PMU_ADCC1:
            res = 3; // battery charge voltage
            break;
        case PMU_RTC_COUNT ... PMU_RTC_COUNT + 3:
            // The kernel reads the counter as one 4-byte block.
            if (s->cmd == PMU_RTC_COUNT)
                s->rtc_count = time(NULL);
            res = (s->rtc_count >> (8 * (s->cmd - PMU_RTC_COUNT))) & 0xff;
            break;
        case PMU_RTC_OFFSET ... PMU_RTC_OFFSET + 3:
            res = s->rtc_offset[s->cmd - PMU_RTC_OFFSET];
            break;
        case 0x69:
            res = 0; // boot count error/panic
            break;
        case 0x76:
            res = 0; // unknown register
            break;
        default:
            res = 0;
    }

    trace_ipod_touch_pmu_recv(s->cmd, res);
    s->cmd += 1;
    return res;
}

static int pcf50633_send(I2CSlave *i2c, uint8_t data)
{
    Pcf50633State *s = PCF50633(i2c);
    trace_ipod_touch_pmu_send(data, s->cmd);

    if (!s->pointer_set)
    {
        s->cmd = data;
        s->pointer_set = true;
        return 0;
    }

    switch(s->cmd) {
        case PMU_DSBL1:
            lcd_changebrightness(data);
            break;
        case PMU_RTC_OFFSET ... PMU_RTC_OFFSET + 3:
            s->rtc_offset[s->cmd - PMU_RTC_OFFSET] = data;
            break;
    }

    s->cmd += 1;
    return 0;
}

static void pcf50633_init(Object *obj)
{

}

static void pcf50633_class_init(ObjectClass *klass, void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = pcf50633_event;
    k->recv = pcf50633_recv;
    k->send = pcf50633_send;
}

static const TypeInfo pcf50633_info = {
    .name          = TYPE_PCF50633,
    .parent        = TYPE_I2C_SLAVE,
    .instance_init = pcf50633_init,
    .instance_size = sizeof(Pcf50633State),
    .class_init    = pcf50633_class_init,
};

static void pcf50633_register_types(void)
{
    type_register_static(&pcf50633_info);
}

type_init(pcf50633_register_types)
