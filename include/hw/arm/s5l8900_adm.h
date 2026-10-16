#ifndef HW_ARM_S5L8900_ADM_H
#define HW_ARM_S5L8900_ADM_H

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/arm/s5l8900_nand.h"

/*
 * The S5L8900's ADM, the core that runs the NAND firmware the 1.1.x kernel uploads (AppleS5L8900XADM and
 * AppleS5L8900XADMFMC), modelled at the level of that firmware's commands (s5l8900_adm.c).
 */
#define TYPE_S5L8900_ADM "s5l8900.adm"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900ADMState, S5L8900_ADM)

#define S5L8900_ADM_REGS        0x100
#define S5L8900_ADM_RESULTS     8

typedef struct S5L8900ADMState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    S5L8900NANDState *nand;

    uint32_t ctrl;
    uint32_t status;                        // the interrupts it raises to the host
    uint32_t results[S5L8900_ADM_RESULTS];
    uint32_t regs[S5L8900_ADM_REGS / 4];    // the section addresses and the rest, as written
} S5L8900ADMState;

#endif
