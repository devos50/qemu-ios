#ifndef HW_ARM_ATV4_CHIPID_H
#define HW_ARM_ATV4_CHIPID_H

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/sysbus.h"

#define TYPE_ATV4_CHIPID "atv4.chipid"
OBJECT_DECLARE_SIMPLE_TYPE(ATV4ChipIDState, ATV4_CHIPID)

#define rCFG_FUSE0      0x0
#define CHIPID_REVISION 0x10

typedef struct ATV4ChipIDState {
    SysBusDevice busdev;
    MemoryRegion iomem;
} ATV4ChipIDState;

#endif