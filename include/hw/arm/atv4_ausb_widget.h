#ifndef HW_ARM_ATV4_AUSB_WIDGET_H
#define HW_ARM_ATV4_AUSB_WIDGET_H

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/sysbus.h"

#define TYPE_ATV4_AUSB_WIDGET "atv4.ausbwidget"
OBJECT_DECLARE_SIMPLE_TYPE(ATV4AUSBWidgetState, ATV4_AUSB_WIDGET)

typedef struct ATV4AUSBWidgetState {
    SysBusDevice busdev;
    MemoryRegion iomem;
} ATV4AUSBWidgetState;

#endif