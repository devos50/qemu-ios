#ifndef IPOD_TOUCH_LCD_H
#define IPOD_TOUCH_LCD_H

#include <math.h>
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/arm/ipod_touch_multitouch.h"

#define TYPE_IPOD_TOUCH_LCD                "ipodtouch.lcd"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchLCDState, IPOD_TOUCH_LCD)

#define LCD_REFRESH_RATE_FREQUENCY 10

#define LCD_REG_INT_ENABLE 0x8
#define LCD_REG_INT_STATUS 0xC  // write 1 to clear

#define LCD_INT_VSYNC      0x1

typedef struct IPodTouchLCDState
{
    SysBusDevice parent_obj;
    MemoryRegion *sysmem;
    MemoryRegion iomem;
    QemuConsole *con;
    IPodTouchMultitouchState *mt;
    int invalidate;
    uint8_t brightness;
    MemoryRegionSection fbsection;
    qemu_irq irq;
    uint32_t lcd_con;

    uint32_t w1_display_resolution_info;
    uint32_t w1_framebuffer_base;
    uint32_t w1_hspan;
    uint32_t w1_display_depth_info;

    uint32_t int_enable;
    uint32_t int_status;

    QEMUTimer *refresh_timer;
} IPodTouchLCDState;

void lcd_changebrightness(int brightness);

#endif
