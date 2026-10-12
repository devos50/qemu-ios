#ifndef IPOD_TOUCH_NAND_H
#define IPOD_TOUCH_NAND_H

#include "qemu/osdep.h"

/*
 * The NAND behind the FMSS: 4 CEs of Samsung NAND. Two storage formats:
 *
 * - A directory with one file per programmed page, <dir>/cs<ce>/<page>.page (page data followed by the spare area);
 *   a page without a file is erased.
 * - A disk image plus an overlay. The disk image holds the disk that the FTL presents (logical page n at offset
 *   n * NAND_BYTES_PER_PAGE). It is never written; we present it as a freshly formatted NAND with every data block in
 *   place, laid out like the VSVFL production format and FTL_Format of the iPhone OS 2.x kernel do it. The pages that
 *   the guest programs or erases go to the overlay, which takes precedence: per CE a sparse data file, a sparse spare
 *   file and one state byte per page. The host folds the overlay back into the disk image when the emulator is not
 *   running (nand.py of the Skeuo pipeline).
 */

#define NAND_CHIP_ID              0xb614d5ad
#define NAND_NUM_CE               4
#define NAND_BYTES_PER_PAGE       4096
#define NAND_BYTES_PER_SPARE      64
#define NAND_PAGES_PER_BLOCK      128
#define NAND_BLOCKS_PER_CE        4096
#define NAND_PAGES_PER_CE         (NAND_PAGES_PER_BLOCK * NAND_BLOCKS_PER_CE)

typedef struct IPodTouchNAND {
    char *page_dir;                         // the one-file-per-page format

    int disk_fd;                            // the disk image format, -1 if not used
    uint64_t disk_size;
    unsigned long *data_lbns;               // the logical blocks that hold data, and are presented as written
    GHashTable *special_pages;              // (ce << 32 | page) -> page data + spare of the formatted NAND
    int overlay_data_fd[NAND_NUM_CE];
    int overlay_spare_fd[NAND_NUM_CE];
    int overlay_state_fd[NAND_NUM_CE];
    uint8_t *overlay_state[NAND_NUM_CE];
} IPodTouchNAND;

void ipod_touch_nand_open_pages(IPodTouchNAND *nand, const char *dir);
bool ipod_touch_nand_open_disk(IPodTouchNAND *nand, const char *disk, const char *overlay, Error **errp);

// Reads a page into data and spare. Returns false if the page is erased (data and spare are then all 0xFF).
bool ipod_touch_nand_read_page(IPodTouchNAND *nand, uint32_t ce, uint32_t page, uint8_t *data, uint8_t *spare);
bool ipod_touch_nand_program_page(IPodTouchNAND *nand, uint32_t ce, uint32_t page, const uint8_t *data,
                                  const uint8_t *spare);
bool ipod_touch_nand_erase_block(IPodTouchNAND *nand, uint32_t ce, uint32_t block);

#endif
