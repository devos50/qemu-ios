#ifndef HW_ARM_ATV4_ANC_H
#define HW_ARM_ATV4_ANC_H

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/sysbus.h"

#define TYPE_ATV4_ANC "atv4.anc"
OBJECT_DECLARE_SIMPLE_TYPE(ATV4ANCState, ATV4_ANC)

typedef struct ATV4ANCState {
    SysBusDevice busdev;
    MemoryRegion ppnnpl_iomem;
    MemoryRegion anc_iomem;
} ATV4ANCState;

#endif