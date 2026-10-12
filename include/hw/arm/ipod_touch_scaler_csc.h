#ifndef HW_ARM_IPOD_TOUCH_SCALER_CSC_H
#define HW_ARM_IPOD_TOUCH_SCALER_CSC_H

#include "qemu/osdep.h"
#include "hw/core/platform-bus.h"
#include "hw/core/hw-error.h"
#include "exec/hwaddr.h"
#include "system/memory.h"

#define TYPE_IPOD_TOUCH_SCALER_CSC                "ipodtouch.scalercsc"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchScalerCSCState, IPOD_TOUCH_SCALER_CSC)


typedef struct IPodTouchScalerCSCState {
	SysBusDevice busdev;
    MemoryRegion iomem;
} IPodTouchScalerCSCState;

#endif