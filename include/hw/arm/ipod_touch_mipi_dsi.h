#ifndef IPOD_TOUCH_MIPI_DSI_H
#define IPOD_TOUCH_MIPI_DSI_H

#include "qemu/osdep.h"
#include "hw/sysbus.h"

#define TYPE_IPOD_TOUCH_MIPI_DSI                "ipodtouch.mipidsi"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchMIPIDSIState, IPOD_TOUCH_MIPI_DSI)

/* Samsung DSIM register layout, as used by iBoot and AppleS5L8720XMIPIDSIController */
#define REG_STATUS    0x00
#define REG_SWRST     0x04
#define REG_CLKCTRL   0x08
#define REG_TIMEOUT   0x0C
#define REG_CONFIG    0x10
#define REG_ESCMODE   0x14
#define REG_MDRESOL   0x18
#define REG_MVPORCH   0x1C
#define REG_MHPORCH   0x20
#define REG_MSYNC     0x24
#define REG_SDRESOL   0x28
#define REG_INTSRC    0x2C
#define REG_INTMSK    0x30
#define REG_PKTHDR    0x34
#define REG_PAYLOAD   0x38
#define REG_RXFIFO    0x3C
#define REG_FIFOTHLD  0x40
#define REG_FIFOCTRL  0x44
#define REG_MEMACCHR  0x48
#define REG_PLLCTRL   0x4C
#define REG_PLLTMR    0x50
#define REG_PHYACCHR  0x54
#define REG_PHYACCHR1 0x58
#define MIPI_DSI_NUM_REGS ((REG_PHYACCHR1 + 4) / 4)

#define rDSIM_STATUS_StopstateClk     0x100
#define rDSIM_STATUS_TxReadyHsClk     0x400
#define rDSIM_STATUS_PllStable        0x80000000
#define rDSIM_SWRST_SwRst             0x1
#define rDSIM_CLKCTRL_TxRequestHsClk  0x80000000
#define rDSIM_CONFIG_LaneEnShift      1
#define rDSIM_INTSRC_PllStable        0x80000000
#define rDSIM_INTSRC_SwRstRelease     0x40000000
#define rDSIM_INTSRC_RxDatDone        0x00040000
#define rDSIM_FIFOCTRL_nInitMask      0x1F
#define rDSIM_FIFOCTRL_EmptyRx        0x1000000
#define rDSIM_FIFOCTRL_EmptyHSfr      0x400000
#define rDSIM_FIFOCTRL_EmptyPayload   0x100000
#define rDSIM_PLLCTRL_PllEn           0x800000

/* DSI data types */
#define DSI_GENERIC_READ_0            0x04
#define DSI_DCS_SHORT_WRITE_0         0x05
#define DSI_DCS_READ                  0x06
#define DSI_GENERIC_READ_1            0x14
#define DSI_DCS_SHORT_WRITE_1         0x15
#define DSI_GENERIC_READ_2            0x24
#define DSI_GENERIC_LONG_WRITE        0x29
#define DSI_DCS_LONG_WRITE            0x39
#define DSI_RSP_GENERIC_LONG_READ     0x1A

/* The Pinot panel returns its 3-byte ID for a generic read of this register */
#define PINOT_REG_PANEL_ID            0xB1
#define PINOT_DEFAULT_PANEL_ID        0x3CD1A1

#define MIPI_DSI_RX_FIFO_WORDS        16

typedef struct IPodTouchMIPIDSIState
{
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[MIPI_DSI_NUM_REGS];
    uint32_t payload_words;
    uint32_t rx_fifo[MIPI_DSI_RX_FIFO_WORDS];
    uint32_t rx_head;
    uint32_t rx_count;
    uint32_t panel_id;
} IPodTouchMIPIDSIState;

#endif
