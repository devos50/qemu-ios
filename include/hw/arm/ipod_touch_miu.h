#ifndef HW_ARM_IPOD_TOUCH_MIU_H
#define HW_ARM_IPOD_TOUCH_MIU_H

#include "qemu/osdep.h"
#include "hw/sysbus.h"

#define TYPE_IPOD_TOUCH_MIU "ipodtouch.miu"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchMIUState, IPOD_TOUCH_MIU)

#define MIU_REG_SIZE 0x200

// names are inferred from how LLB's miu_init uses the registers
#define MIU_CTRL        0x100 // bit 20 is set once DRAM is initialized
#define MIU_DIRECT_CMD  0x104 // 0x233 precharge all, 0x333 auto refresh, 0x133 (extended) mode register set
#define MIU_REFRESH     0x108 // bit 12 enables auto refresh
#define MIU_MODE        0x110 // value for the next mode register set command
#define MIU_STATUS0     0x11C // read and written back, probably write-one-to-clear
#define MIU_STATUS1     0x120
#define MIU_PHY_CTRL    0x140 // DLL start/on; later the lock value is forced into bits 20+
#define MIU_PHY_STATUS  0x144 // bits 0-1: DLL locked, bits 18-25: lock value

#define MIU_PHY_STATUS_LOCKED 0x3

typedef struct IPodTouchMIUState {
    SysBusDevice busdev;
    MemoryRegion iomem;
    uint32_t regs[MIU_REG_SIZE / 4];
} IPodTouchMIUState;

#endif
