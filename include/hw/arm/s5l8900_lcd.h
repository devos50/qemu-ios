#ifndef HW_ARM_S5L8900_LCD_H
#define HW_ARM_S5L8900_LCD_H

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "ui/console.h"
#include "hw/display/framebuffer.h"
#include "qemu/timer.h"

#define TYPE_S5L8900_LCD "s5l8900.lcd"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900LCDState, S5L8900_LCD)

#define S5L8900_LCD_REGS        0x1000

/*
 * The control register: bits 2-6 turn the windows on, window n at bit 6 - n. iBoot clears bits 2-7, sets up window 1
 * and sets bit 5 (0x1800a7ea in 1.1.5's iBoot, CON |= mask << 2); the kernel then sets up window 0, sets bit 6 and
 * clears bit 5, leaving iBoot's window as it was but off. Only windows 0 and 1 have been seen.
 */
#define S5L8900_LCD_CON         0x08
#define S5L8900_LCD_CON_WINDOW_ON(n) (1 << (6 - (n)))

// five windows of six registers each, from 0x58 (iBoot 1.1.5's display driver)
#define S5L8900_LCD_WINDOWS     5
#define S5L8900_LCD_WINDOW(n)   (0x58 + (n) * 0x18)
#define S5L8900_LCD_WND_HSPAN   0x00  // bytes per line
#define S5L8900_LCD_WND_CON     0x04  // the pixel format in bits 8-10
#define S5L8900_LCD_WND_BASE    0x08  // the framebuffer's physical address
#define S5L8900_LCD_WND_SIZE    0x0c  // width << 16 | height
#define S5L8900_LCD_WND_QLEN    0x10

// the interrupt: enables, and the status, which writing 1s clears
#define S5L8900_LCD_INT_ENABLE  0x14
#define S5L8900_LCD_INT_STATUS  0x18
#define S5L8900_LCD_INT_FRAME   (1 << 0)
#define S5L8900_LCD_FRAME_NS    (NANOSECONDS_PER_SECOND / 60)

#define S5L8900_LCD_WIDTH       320
#define S5L8900_LCD_HEIGHT      480

typedef struct S5L8900LCDState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    MemoryRegion *sysmem;
    qemu_irq irq;
    QEMUTimer *frame_timer;
    uint32_t status;
    QemuConsole *con;
    MemoryRegionSection fbsection;
    bool invalidate;
    int shown;      // the window on screen, -1 for none
    int32_t brightness; // of the backlight, 0-255 (the PMU's "backlight" line)
    uint32_t regs[S5L8900_LCD_REGS / 4];
} S5L8900LCDState;

#endif
