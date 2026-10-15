#ifndef HW_ARM_S5L8900_NAND_H
#define HW_ARM_S5L8900_NAND_H

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"

/*
 * The S5L8900's NAND flash controller (FMC) with the NAND chips behind it, and its ECC engine. The registers are the
 * ones iBoot 1.1.5's FIL (Whimory 2.1, H1flash.c and H1ECC.c) uses.
 */
#define TYPE_S5L8900_NAND "s5l8900.nand"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900NANDState, S5L8900_NAND)

#define TYPE_S5L8900_NAND_ECC "s5l8900.nand-ecc"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900NANDECCState, S5L8900_NAND_ECC)

/* The NAND of the 8 GB iPod touch: 8 banks of Hynix NAND, one of the chips iBoot knows */
#define S5L8900_NAND_CHIP_ID            0xa514d3ad
#define S5L8900_NAND_BANKS              8
#define S5L8900_NAND_BLOCKS_PER_BANK    4096
#define S5L8900_NAND_PAGES_PER_BLOCK    128
#define S5L8900_NAND_PAGES_PER_BANK     (S5L8900_NAND_BLOCKS_PER_BANK * S5L8900_NAND_PAGES_PER_BLOCK)
#define S5L8900_NAND_BYTES_PER_PAGE     2048
#define S5L8900_NAND_BYTES_PER_SPARE    64
#define S5L8900_NAND_PAGE_REGISTER      (S5L8900_NAND_BYTES_PER_PAGE + S5L8900_NAND_BYTES_PER_SPARE)

/*
 * The NAND's contents: a disk image presented as a freshly formatted NAND, with an overlay for the pages the guest
 * changes (s5l8900_nand_disk.c). Without one, every page is erased.
 */
typedef struct S5L8900NANDDisk {
    int disk_fd;                            // -1 without a disk image
    uint64_t disk_size;
    unsigned long *data_lbns;               // the logical blocks that hold data, and are presented as written
    GHashTable *special_pages;              // (bank << 32 | page) -> page data + spare of the formatted NAND
    int overlay_data_fd[S5L8900_NAND_BANKS];
    int overlay_spare_fd[S5L8900_NAND_BANKS];
    int overlay_state_fd[S5L8900_NAND_BANKS];
    uint8_t *overlay_state[S5L8900_NAND_BANKS];
} S5L8900NANDDisk;

bool s5l8900_nand_disk_open(S5L8900NANDDisk *disk, const char *path, const char *overlay, Error **errp);
// Reads a page into data and spare. Returns false if the page is erased (data and spare are then all 0xff).
bool s5l8900_nand_disk_read_page(S5L8900NANDDisk *disk, uint32_t bank, uint32_t page, uint8_t *data, uint8_t *spare);

typedef enum {
    NAND_OUT_DATA,      // the page register, from the column
    NAND_OUT_ID,
    NAND_OUT_STATUS,
} S5L8900NANDOutput;

typedef struct S5L8900NANDState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    char *disk_path;
    char *overlay_path;
    S5L8900NANDDisk disk;

    uint32_t fmctrl0;
    uint32_t fmctrl1;
    uint32_t cmd;
    uint32_t fmaddr0;
    uint32_t fmaddr1;
    uint32_t fmanum;
    uint32_t fmdnum;
    uint32_t fmcstat;
    uint32_t rsctrl;

    // the chips: what the selected one outputs, and the page it last loaded into its page register
    uint32_t output;
    uint32_t column;
    uint32_t row;
    uint8_t page_register[S5L8900_NAND_PAGE_REGISTER];

    // what the last data transfer brought into the FIFO
    uint8_t fifo[S5L8900_NAND_PAGE_REGISTER];
    uint32_t fifo_len;
    uint32_t fifo_pos;
} S5L8900NANDState;

typedef struct S5L8900NANDECCState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t data_addr;
    uint32_t ecc_addr;
    uint32_t setup;
    uint32_t status;
} S5L8900NANDECCState;

#endif
