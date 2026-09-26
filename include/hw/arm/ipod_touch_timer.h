#ifndef IPOD_TOUCH_TIMER_H
#define IPOD_TOUCH_TIMER_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/sysbus.h"
#include "hw/irq.h"

#define TYPE_IPOD_TOUCH_TIMER                "ipodtouch.timer"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchTimerState, IPOD_TOUCH_TIMER)

// all timers run from the 24 MHz fixed-frequency clock
#define TIMER_SRC_HZ 24000000

// the 64-bit free running counter (iBoot's "timer 8"), the source of mach_absolute_time
#define TIMER_TICKSHIGH  0x80
#define TIMER_TICKSLOW   0x84
#define TIMER_TICKS_CFG  0x88
#define TIMER_TICKS_CFG_STOP_MASK 0xF   // iBoot sets 0xA to stop the counter and clears the nibble to start it
#define TIMER_TICKS_CFG_DIV_SHIFT 4     // divides the source clock by DIV + 1
#define TIMER_TICKS_CFG_DIV_MASK  0xFF

// interrupt status of timers 4-7, write 1 to clear. Timer n uses bits [(7 - n) * 8 + 2 : (7 - n) * 8].
#define TIMER_IRQSTAT 0x118
#define TIMER_IRQSTAT_SHIFT(n) ((7 - (n)) * 8)

// per-timer registers; timers 0-3 start at 0x00, timers 4-7 at 0xA0, with a stride of 0x20
#define NUM_TIMERS 8
#define TIMER_BASE(n) ((n) < 4 ? (n) * 0x20 : 0xA0 + ((n) - 4) * 0x20)
#define TIMER_CONFIG        0x0
#define TIMER_STATE         0x4
#define TIMER_COUNT_BUFFER  0x8
#define TIMER_COUNT_BUFFER2 0xC
#define TIMER_PRESCALER     0x10  // written by iBoot (always 0), not used by the model
#define TIMER_COUNT         0x14  // counts up from 0 to COUNT_BUFFER
#define TIMER_REG_SIZE      0x18

#define TIMER_STATE_START        1
#define TIMER_STATE_MANUALUPDATE 2  // restart the count from the count buffers

// CONFIG register fields
#define TIMER_CONFIG_CLK_SHIFT 8    // clock select, see ipod_touch_timer_hz()
#define TIMER_CONFIG_CLK_MASK  0x7
#define TIMER_CONFIG_IE_SHIFT  12   // one enable per status bit
#define TIMER_CONFIG_IE_MASK   0x7

#define TIMER_IRQ_COUNT_REACHED 1   // status bit 0, the only interrupt the model raises

typedef struct IPodTouchTimerChannel {
    uint32_t config;
    uint32_t state;
    uint32_t count_buffer;
    uint32_t count_buffer2;
    uint32_t prescaler;
    uint64_t base_count;  // count at start_ns
    int64_t start_ns;
    QEMUTimer *timer;
} IPodTouchTimerChannel;

typedef struct IPodTouchTimerState
{
    SysBusDevice busdev;
    MemoryRegion iomem;
    qemu_irq irq;

    IPodTouchTimerChannel channels[NUM_TIMERS];
    uint32_t irqstat;

    // free running counter
    uint32_t ticks_cfg;
    uint32_t ticks_regs[4]; // 0x8C-0x98, written by iBoot, not used by the model
    uint64_t ticks_base;    // count at ticks_base_ns
    int64_t ticks_base_ns;
    uint32_t ticks_low;     // latched when the high word is read
} IPodTouchTimerState;

#endif
