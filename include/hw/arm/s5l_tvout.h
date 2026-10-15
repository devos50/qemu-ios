#ifndef S5L_TVOUT_H
#define S5L_TVOUT_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/hw-error.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"

#define SDO_CLKCON 0x0
#define SDO_CONFIG 0x8
#define SDO_IRQ 0x280
#define SDO_IRQMASK 0x284

#define MXR_STATUS 0x0
#define MXR_CFG 0x4

#define MXR_STATUS_ENABLE 0x1
#define SDO_IRQ_VSYNC 0x1
#define TVOUT_REFRESH_RATE 60

#define TYPE_S5L_TVOUT                "s5l.tvout"
OBJECT_DECLARE_SIMPLE_TYPE(S5LTVOutState, S5L_TVOUT)

typedef struct S5LTVOutState {
    SysBusDevice parent_obj;

    MemoryRegion mixer1_iomem;
    MemoryRegion mixer2_iomem;
    MemoryRegion sdo_iomem;
    qemu_irq irq;

    uint32_t mixer1_status;
    uint32_t mixer1_cfg;

    uint32_t mixer2_status;
    uint32_t mixer2_cfg;

    uint32_t sdo_clkcon;
    uint32_t sdo_config;
    uint32_t sdo_irq;
    uint32_t sdo_irq_mask;

    bool mixer_enabled;
    QEMUTimer *vsync_timer;
} S5LTVOutState;

#endif