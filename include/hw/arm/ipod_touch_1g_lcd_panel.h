#ifndef HW_ARM_IPOD_TOUCH_1G_LCD_PANEL_H
#define HW_ARM_IPOD_TOUCH_1G_LCD_PANEL_H

#include "qemu/osdep.h"
#include "hw/ssi/ssi.h"

#define TYPE_IPOD_TOUCH_1G_LCD_PANEL "ipod-touch-1g.lcd-panel"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouch1GLCDPanelState, IPOD_TOUCH_1G_LCD_PANEL)

#define IT1G_PANEL_REGS         0x80

typedef struct IPodTouch1GLCDPanelState {
    SSIPeripheral parent_obj;

    uint8_t regs[IT1G_PANEL_REGS];
    uint8_t addr;       // the register the current command names
    bool read;          // whether it reads it
    uint32_t pos;       // the byte of the current command
} IPodTouch1GLCDPanelState;

#endif
