#ifndef HW_ARM_S5L_USB_PHYS_H
#define HW_ARM_S5L_USB_PHYS_H

#include "qemu/osdep.h"
#include "hw/core/hw-error.h"
#include "hw/core/sysbus.h"

#define TYPE_S5L_USB_PHYS "s5l.usbphys"
OBJECT_DECLARE_SIMPLE_TYPE(S5LUSBPhysState, S5L_USB_PHYS)

#define REG_OPHYPWR 0x0
#define REG_OPHYCLK 0x4
#define REG_ORSTCON 0x8
#define REG_UNKNOWN1 0x1C
#define REG_OPHYTUNE 0x20
#define REG_CABLE    0x28

typedef struct S5LUSBPhysState {
    SysBusDevice busdev;
    MemoryRegion iomem;

    uint32_t usb_ophypwr;
    uint32_t usb_ophyclk;
    uint32_t usb_orstcon;
    uint32_t usb_unknown1;
    uint32_t control_offset;
    uint32_t usb_ophytune;
    bool cable_connected;
    qemu_irq link_reset;
} S5LUSBPhysState;

#endif
