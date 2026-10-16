#ifndef HW_ARM_S5L8900_TVOUT_H
#define HW_ARM_S5L8900_TVOUT_H

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "qemu/timer.h"

/* The S5L8900's TV out (s5l8900_tvout.c): the SDO, the mixer and a third block, each a 0x1000-byte region */
#define TYPE_S5L8900_TVOUT "s5l8900.tvout"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900TVOutState, S5L8900_TVOUT)

#define S5L8900_TVOUT_REGS          0x1000
#define S5L8900_TVOUT_BLOCKS        3   // sysbus regions and register sets, in this order
#define S5L8900_TVOUT_SDO           0
#define S5L8900_TVOUT_MIXER         1
#define S5L8900_TVOUT_VP            2

#define S5L8900_TVOUT_SDO_ENABLE    0x000
#define S5L8900_TVOUT_SDO_INT       0x280   // the status, which writing 1s clears; bit 0 a frame
#define S5L8900_TVOUT_SDO_INT_MASK  0x284   // 1s mask
#define S5L8900_TVOUT_FRAME_NS      (NANOSECONDS_PER_SECOND / 60)

typedef struct S5L8900TVOutState {
    SysBusDevice parent_obj;
    MemoryRegion iomem[S5L8900_TVOUT_BLOCKS];
    qemu_irq sdo_irq;
    qemu_irq mixer_irq;
    QEMUTimer *frame_timer;
    uint32_t sdo_status;
    uint32_t regs[S5L8900_TVOUT_BLOCKS][S5L8900_TVOUT_REGS / 4];
} S5L8900TVOutState;

#endif
