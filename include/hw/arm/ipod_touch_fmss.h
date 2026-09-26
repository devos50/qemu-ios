#ifndef IPOD_TOUCH_FMSS_H
#define IPOD_TOUCH_FMSS_H

#include <math.h>
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/sysbus.h"
#include "hw/hw.h"
#include "hw/irq.h"

#define TYPE_IPOD_TOUCH_FMSS                "ipodtouch.fmss"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchFMSSState, IPOD_TOUCH_FMSS)

/*
 * The FMSS is a flash controller that runs small microcode programs ("sequences") provided by the driver. The driver
 * writes the physical address of a sequence to FMSS_CS_BASEADDR, puts the parameters of the operation in the
 * FMSS_PARAM_* registers and starts the sequence through FMSS_CS_CTRL. We do not execute the microcode, but determine
 * which NAND operation a sequence performs from the NAND commands it issues and perform that operation directly.
 */

#define FMSS_REG_SIZE             0xF00

#define FMSS_FMCTRL1              0x4
#define FMSS_CS_CTRL              0xC00
#define FMSS_CS_BASEADDR          0xC04
#define FMSS_CS_STATE             0xC08
#define FMSS_CS_IRQ               0xC0C
#define FMSS_CS_IRQMASK           0xC10
#define FMSS_CS_STATUS            0xC30
#define FMSS_CS_BUF_RST_OK        0xC64

// parameters of the sequences (the CSGENR registers)
#define FMSS_PARAM_BASE           0xD00
#define FMSS_PARAM_SIZE           0x80
#define FMSS_PARAM_UNKNOWN_D00    0xD00
#define FMSS_PARAM_ID_OUT_ADDR    0xD08   // read ID: where to store the chip IDs
#define FMSS_PARAM_PAGES_ADDR     0xD0C   // read/erase: list of pages
#define FMSS_PARAM_CE_ADDR        0xD10   // read: list of CE masks, erase: list of CEs, program: list of {command, page}
#define FMSS_PARAM_NUM_PAGES      0xD18   // read/erase: number of pages
#define FMSS_PARAM_META_ADDR      0xD1C   // read/program: meta data buffer (FMSS_META_SIZE bytes per page)
#define FMSS_PARAM_DMA_ADDR       0xD20   // read/program: list of data buffers, one for every chunk of a page
#define FMSS_PARAM_CHUNKS         0xD28   // read/program: number of chunks per page

#define FMSS_CS_CTRL_START_MASK   0xFF0F
#define FMSS_CS_CTRL_START        0xFF05
#define FMSS_CS_CTRL_IRQ_EN       (1 << 6)  // the kernel starts sequences with 0xfff5, iBoot polls and uses 0xffb5

#define FMSS_CS_IRQ_DONE          (1 << 0)

#define FMSS_FMCTRL1_IDLE         (1 << 30)

#define FMSS_STATUS_ERROR         (1 << 28)
#define FMSS_STATUS_CLEAN         (1 << 29) // all pages that were read are erased

// sequence instructions: {uint16_t reg, uint8_t arg, uint8_t opcode, uint32_t imm}
#define FMSS_SEQ_INSN_SIZE        8
#define FMSS_SEQ_MAX_SIZE         0x2000
#define FMSS_SEQ_OP_END           0x00
#define FMSS_SEQ_OP_STORE_IMM     0x01  // reg = imm
#define FMSS_SEQ_OP_STORE_REG     0x02  // reg = r[arg]
#define FMSS_SEQ_OP_LOAD          0x04  // r[arg] = reg & imm
#define FMSS_FMCMD                0x8   // the command register of the NAND bus

// NAND geometry of the Samsung 0xb614d5ad chips on the iPod Touch 2G
#define NAND_CHIP_ID              0xb614d5ad
#define NAND_NUM_CE               4
#define NAND_BYTES_PER_PAGE       4096
#define NAND_BYTES_PER_SPARE      64
#define NAND_PAGES_PER_BLOCK      128
#define FMSS_META_SIZE            12    // the part of the spare area that is visible to the driver
#define FMSS_MAX_PAGES_PER_OP     1024

typedef enum {
    FMSS_OP_UNKNOWN,
    FMSS_OP_RESET,
    FMSS_OP_READ_ID,
    FMSS_OP_READ,
    FMSS_OP_PROGRAM,
    FMSS_OP_ERASE,
} FMSSOperation;

typedef struct IPodTouchFMSSState
{
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t regs[FMSS_REG_SIZE / 4];
    uint32_t cs_irq;
    uint32_t status;
    bool iboot_patched;

    uint8_t page_buffer[NAND_BYTES_PER_PAGE];
    uint8_t spare_buffer[NAND_BYTES_PER_SPARE];
    char *nand_path;
} IPodTouchFMSSState;

#endif
