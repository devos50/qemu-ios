#ifndef HW_ARM_IPOD_TOUCH_MBX_H
#define HW_ARM_IPOD_TOUCH_MBX_H

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/sysbus.h"

#define TYPE_IPOD_TOUCH_MBX "ipodtouch.mbx"

// MMU control: the kernel sets or clears ENABLE and waits for ENABLED to follow, and starts a TLB invalidation of
// the page in 0x1024/0x1028 with INVALIDATE and waits for INVALIDATE_BUSY to clear.
#define MBX_MMU_CTRL                  0x1020
#define MBX_MMU_CTRL_ENABLE           (1 << 0)
#define MBX_MMU_CTRL_INVALIDATE       (1 << 8)
#define MBX_MMU_CTRL_ENABLED          (1 << 16)
#define MBX_MMU_CTRL_INVALIDATE_BUSY  (1 << 24)
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchMBXState, IPOD_TOUCH_MBX)

typedef struct IPodTouchMBXState {
    SysBusDevice busdev;
    MemoryRegion iomem1;
    MemoryRegion iomem2;
    uint32_t mmu_ctrl;
    bool alreadypatched;
} IPodTouchMBXState;

#endif
