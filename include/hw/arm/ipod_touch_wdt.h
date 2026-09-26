#ifndef HW_ARM_IPOD_TOUCH_WDT_H
#define HW_ARM_IPOD_TOUCH_WDT_H

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/sysbus.h"
#include "hw/irq.h"

#define TYPE_IPOD_TOUCH_WDT "ipodtouch.wdt"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchWDTState, IPOD_TOUCH_WDT)

#define WDT_CTRL 0x00
#define WDT_CNT  0x04

// CTRL register fields
#define WDT_CTRL_ENABLE    (1 << 20)
#define WDT_CTRL_PRE_SHIFT 16      // prescaler, divides by PRE + 1
#define WDT_CTRL_PRE_MASK  0xF
#define WDT_CTRL_INT_EN    (1 << 15) // raise an interrupt on expiry instead of resetting
#define WDT_CTRL_CS_SHIFT  12      // clock select, divides by 2^CS
#define WDT_CTRL_CS_MASK   0x7
#define WDT_CTRL_CLR_MASK  0xFFF
#define WDT_CTRL_CLR       0xA00   // written together with the other fields to restart the count

// the counter expires when it reaches this many (prescaled) ticks
#define WDT_COUNT_MAX (1 << 18)

typedef struct IPodTouchWDTState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    QEMUTimer *timer;
    qemu_irq irq;

    uint32_t ctrl;
    int64_t start_ns; // virtual time at which the count last restarted
    uint32_t clock_hz;
} IPodTouchWDTState;

#endif
