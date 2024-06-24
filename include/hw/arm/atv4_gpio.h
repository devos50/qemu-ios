#ifndef ATV4_GPIO_H
#define ATV4_GPIO_H

#include <math.h>
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/sysbus.h"

#define TYPE_ATV4_GPIO                "atv4.gpio"
OBJECT_DECLARE_SIMPLE_TYPE(ATV4GPIOState, ATV4_GPIO)

#define GPIO_FORCE_DFU      0xD04

typedef struct ATV4GPIOState
{
    SysBusDevice parent_obj;
    MemoryRegion iomem;
} ATV4GPIOState;

#endif