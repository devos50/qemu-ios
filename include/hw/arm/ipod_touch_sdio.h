#ifndef IPOD_TOUCH_SDIO_H
#define IPOD_TOUCH_SDIO_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/arm/ipod_touch_bcm4325.h"

#define TYPE_IPOD_TOUCH_SDIO                "ipodtouch.sdio"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchSDIOState, IPOD_TOUCH_SDIO)

#define SDIO_CTRL       0x00
#define SDIO_DCTRL      0x04
#define SDIO_CMD        0x08
#define SDIO_ARGU       0x0C
#define SDIO_STATE      0x10
#define SDIO_STAC       0x14
#define SDIO_DSTA       0x18
#define SDIO_FSTA       0x1C
#define SDIO_RESP0      0x20
#define SDIO_RESP1      0x24
#define SDIO_RESP2      0x28
#define SDIO_RESP3      0x2C
#define SDIO_CLKDIV     0x30
#define SDIO_CSR        0x34
#define SDIO_IRQ        0x38
#define SDIO_IRQMASK    0x3C
#define SDIO_BADDR      0x44
#define SDIO_BLKLEN     0x48
#define SDIO_NUMBLK     0x4C
#define SDIO_REMBLK     0x50

// CMD register
#define SDIO_CMD_START     (1u << 31)
#define SDIO_CMD_INDEX     0x3F

// DCTRL register
#define SDIO_DCTRL_START   (1 << 4)

// DSTA register
#define SDIO_DSTA_CMD_RDY  (1 << 0)
#define SDIO_DSTA_CMD_DONE (1 << 4)
#define SDIO_DSTA_TIMEOUT  (1 << 15)

// CSR register: the S5L8720 driver toggles this bit off and on to re-arm the card interrupt
#define SDIO_CSR_CARD_INT  (1 << 1)

// IRQ / IRQMASK registers
#define SDIO_IRQ_DAT_DONE  (1 << 0)
#define SDIO_IRQ_CARD      (1 << 1)

typedef struct IPodTouchSDIOState
{
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t ctrl;
    uint32_t dctrl;
    uint32_t cmd;
    uint32_t arg;
    uint32_t stac;
    uint32_t dsta;
    uint32_t resp[4];
    uint32_t clkdiv;
    uint32_t csr;
    uint32_t irq_status;
    uint32_t irq_mask;
    uint32_t baddr;
    uint32_t blklen;
    uint32_t numblk;
    bool data_done; // the last CMD53 moved its data; reported once the driver starts the DMA
    bool card_irq;  // level of the card interrupt line

    BCM4325State card;
} IPodTouchSDIOState;

#endif
