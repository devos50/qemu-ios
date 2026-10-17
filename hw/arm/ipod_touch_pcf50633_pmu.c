#include "hw/arm/ipod_touch_pcf50633_pmu.h"
#include "hw/arm/ipod_touch_lcd.h"
#include "trace.h"
#include "qemu/cutils.h"
#include "system/rtc.h"
#include "migration/vmstate.h"
#include "hw/core/qdev-properties.h"

static void pcf50633_update_backlight(Pcf50633State *s)
{
    uint32_t level = MIN(s->backlight_level, s->backlight_level_max);
    int brightness = (s->backlight_ctrl & PMU_BACKLIGHT_CTRL_ON) ? level * 255 / s->backlight_level_max : 0;
    lcd_changebrightness(brightness);
    qemu_set_irq(s->backlight, brightness);
}

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

static uint8_t to_bcd(int value)
{
    return ((value / 10) << 4) | (value % 10);
}

// A register of the BCD clock. Reading the seconds latches the time, so that a block read doesn't tear.
static uint8_t pcf50633_rtc_bcd(Pcf50633State *s)
{
    struct tm tm;

    if (s->cmd == PMU_RTC_BCD) {
        qemu_get_timedate(&tm, 0);
        s->rtc_count = mktimegm(&tm);
    }
    time_t t = s->rtc_count;
    gmtime_r(&t, &tm);
    switch (s->cmd - PMU_RTC_BCD) {
    case 0: return to_bcd(tm.tm_sec);
    case 1: return to_bcd(tm.tm_min);
    case 2: return to_bcd(tm.tm_hour);
    case 3: return tm.tm_wday;
    case 4: return to_bcd(tm.tm_mday);
    case 5: return to_bcd(tm.tm_mon + 1);
    default: return to_bcd(tm.tm_year % 100);
    }
}

static uint8_t pcf50633_recv(I2CSlave *i2c)
{
    Pcf50633State *s = PCF50633(i2c);
    int res = 0;

    if (s->rtc_bcd && s->cmd >= PMU_RTC_BCD && s->cmd < PMU_RTC_BCD + PMU_RTC_BCD_LEN) {
        res = pcf50633_rtc_bcd(s);
    } else if (s->cmd == s->backlight_level_reg) {
        res = s->backlight_level;
    } else if (s->cmd == s->backlight_ctrl_reg) {
        res = s->backlight_ctrl;
    } else switch(s->cmd) {
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
            // The kernel reads the counter as one 4-byte block. The time follows -rtc base=... (default: the host's).
            if (s->cmd == PMU_RTC_COUNT) {
                struct tm tm;
                qemu_get_timedate(&tm, 0);
                s->rtc_count = mktimegm(&tm);
            }
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

    if (s->cmd == s->backlight_level_reg) {
        s->backlight_level = data;
        pcf50633_update_backlight(s);
    } else if (s->cmd == s->backlight_ctrl_reg) {
        s->backlight_ctrl = data;
        pcf50633_update_backlight(s);
    } else switch(s->cmd) {
        case PMU_RTC_OFFSET ... PMU_RTC_OFFSET + 3:
            s->rtc_offset[s->cmd - PMU_RTC_OFFSET] = data;
            break;
    }

    s->cmd += 1;
    return 0;
}

static void pcf50633_init(Object *obj)
{
    Pcf50633State *s = PCF50633(obj);

    // full brightness until the bootloader programs the backlight
    s->backlight_level = PMU_BACKLIGHT_LEVEL_MAX;
    s->backlight_ctrl = PMU_BACKLIGHT_CTRL_ON;
    // the brightness (0-255), for a display that doesn't take it from lcd_changebrightness()
    qdev_init_gpio_out_named(DEVICE(obj), &s->backlight, "backlight", 1);
}

static int pcf50633_post_load(void *opaque, int version_id)
{
    pcf50633_update_backlight(opaque);
    return 0;
}

// usb_present is board wiring. The RTC counter (or the latched BCD time) is reread from the QEMU RTC.
static const VMStateDescription vmstate_pcf50633 = {
    .name = TYPE_PCF50633,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = pcf50633_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(i2c, Pcf50633State),
        VMSTATE_UINT32(cmd, Pcf50633State),
        VMSTATE_BOOL(pointer_set, Pcf50633State),
        VMSTATE_UINT32(rtc_count, Pcf50633State),
        VMSTATE_UINT8_ARRAY(rtc_offset, Pcf50633State, 4),
        VMSTATE_UINT8(backlight_level, Pcf50633State),
        VMSTATE_UINT8(backlight_ctrl, Pcf50633State),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * Where the backlight is: the D1759's level and control registers by default. The iPod touch 1G's PCF50635 has its
 * level in LEDOUT (0x28) and switches it on with bit 0 of LEDENA (0x29). rtc-bcd: the PCF50635's BCD clock registers
 * (0x59-0x5f) instead of the D1759's seconds counter.
 */
static const Property pcf50633_properties[] = {
    DEFINE_PROP_UINT8("backlight-level-reg", Pcf50633State, backlight_level_reg, PMU_BACKLIGHT_LEVEL),
    DEFINE_PROP_UINT8("backlight-ctrl-reg", Pcf50633State, backlight_ctrl_reg, PMU_BACKLIGHT_CTRL),
    DEFINE_PROP_UINT8("backlight-level-max", Pcf50633State, backlight_level_max, PMU_BACKLIGHT_LEVEL_MAX),
    DEFINE_PROP_BOOL("rtc-bcd", Pcf50633State, rtc_bcd, false),
};

static void pcf50633_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->vmsd = &vmstate_pcf50633;
    device_class_set_props(dc, pcf50633_properties);
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
