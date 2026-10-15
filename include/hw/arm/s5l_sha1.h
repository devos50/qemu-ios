#ifndef HW_ARM_S5L_SHA1_H
#define HW_ARM_S5L_SHA1_H

#include "qemu/osdep.h"
#include "hw/core/platform-bus.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/hw-error.h"
#include "exec/hwaddr.h"
#include "system/memory.h"

#define TYPE_S5L_SHA1                "s5l.sha1"
OBJECT_DECLARE_SIMPLE_TYPE(S5LSHA1State, S5L_SHA1)

#define SHA_CONFIG         0x0
#define SHA_RESET          0x4
#define SHA_IRQ_STATUS     0x8  // write 1 to acknowledge the completion interrupt
#define SHA_IRQ_ENABLE     0xc
#define SHA_HASH           0x20 // h0..h4; written to continue a hash, read for the result
#define SHA_MEMORY_MODE    0x80 // whether we read from the memory
#define SHA_MEMORY_START   0x84
#define SHA_INSIZE         0x8c

// SHA_CONFIG bits
#define SHA_CONFIG_BUSY     (1 << 0)
#define SHA_CONFIG_START    (1 << 1)
#define SHA_CONFIG_IRQ      (1 << 2) // signal completion with an interrupt instead of being polled
#define SHA_CONFIG_CONTINUE (1 << 3) // continue from the state in SHA_HASH

typedef struct S5LSHA1State {
	SysBusDevice busdev;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t config;
    uint32_t irq_status;
    uint32_t irq_enable;
    uint32_t memory_start;
    uint32_t memory_mode;
    uint32_t insize;
    uint32_t hw_buffer[0x10]; // hardware buffer
    uint8_t hashout[0x14];
    bool hw_buffer_dirty;
    bool hash_computed;
    uint32_t state[5]; // running SHA-1 state h0..h4
} S5LSHA1State;

#endif