#ifndef HW_ARM_ATV4_AUSB_H
#define HW_ARM_ATV4_AUSB_H

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/sysbus.h"

#define TYPE_ATV4_AUSB "atv4.ausb"
OBJECT_DECLARE_SIMPLE_TYPE(ATV4AUSBState, ATV4_AUSB)

typedef struct ATV4AUSBState {
    SysBusDevice busdev;
    MemoryRegion iomem;
} ATV4AUSBState;

#endif