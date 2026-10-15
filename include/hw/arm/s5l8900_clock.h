#ifndef HW_ARM_S5L8900_CLOCK_H
#define HW_ARM_S5L8900_CLOCK_H

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"

#define TYPE_S5L8900_CLOCK "s5l8900.clock"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900ClockState, S5L8900_CLOCK)

#define S5L8900_CLOCK_CONFIG0   0x00
#define S5L8900_CLOCK_CONFIG1   0x04
#define S5L8900_CLOCK_CONFIG2   0x08
#define S5L8900_CLOCK_PLL0CON   0x20
#define S5L8900_CLOCK_PLL1CON   0x24
#define S5L8900_CLOCK_PLL2CON   0x28
#define S5L8900_CLOCK_PLL3CON   0x2C
#define S5L8900_CLOCK_PLLLOCK   0x40
#define S5L8900_CLOCK_PLLMODE   0x44
#define S5L8900_CLOCK_REGS      0x400

typedef struct S5L8900ClockState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[S5L8900_CLOCK_REGS / 4];
} S5L8900ClockState;

#endif
