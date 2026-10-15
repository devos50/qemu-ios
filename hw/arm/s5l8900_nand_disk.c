/*
 * The 1G's NAND as a disk image: a raw disk (what the FTL presents, logical page n at offset n * 2048) shown as a
 * freshly formatted NAND in Whimory 2.1's legacy format, the one iBoot 1.1.5 and the iPhone OS 1.1.x kernel read.
 * Like the 2G's (ipod_touch_nand.c), the disk image is never written; the overlay, which takes precedence, holds the
 * pages the guest changes: per bank a sparse data file, a sparse spare file and one state byte per page.
 *
 * The layout, as iBoot 1.1.5 opens it (FIL "H1", VFL and FTL of Whimory 2.1; found in iBoot.n45ap.1.1.5):
 *
 * - Bank 0, block 0: the FIL's signature, 0x43303033 ("AND" version 3) in the first word of a page of the block.
 * - Every bank: a "DEVICEINFOBBT" page in the last 10% of the blocks with the factory bad block table (a bit per
 *   block, set for good blocks). It is in the last block, which the VFL can't then use: the VFL's context marks it
 *   bad and replaces it by the first block of the reserved area.
 * - Every bank, blocks 1-4: the VFL's info blocks. The context is in groups of 8 copies; the VFL finds the first info
 *   block from the bad block table, takes the copy with the lowest age and reads the last group written.
 *   Blocks 5-200 are the reserved area for replacing bad blocks, blocks 201-4095 the virtual blocks.
 * - The FTL's virtual block n is the VFL's 201 + n; its pages go round the banks (VFL page = block * 1024 +
 *   page in block * 8 + bank). FTL blocks 0-2 are its context blocks, 3-22 the free blocks for logs, and logical
 *   block n is at FTL block 23 + n (3872 logical blocks of 2 MB).
 * - The FTL context: the first page of a context block identifies it (spare type 'C'), then its map table pages; the
 *   FTL meta data is in the last page, which the FTL looks for backwards from the end. The erase, read and log
 *   tables all point to the first page, which is zero.
 *
 * Every page's spare starts with 12 bytes of meta data the VFL and FTL see; the rest is the ECC of the data and the
 * meta data, which the FIL checks with the ECC engine. Here it is zero: the engine (s5l8900.nand-ecc) computes
 * nothing, it only fails sectors whose data and ECC are all 0xff, as for an erased page.
 */
#include "qemu/osdep.h"
#include "qemu/bitmap.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/arm/s5l8900_nand.h"

#define BANKS                   S5L8900_NAND_BANKS
#define BLOCKS_PER_BANK         S5L8900_NAND_BLOCKS_PER_BANK
#define PAGES_PER_BLOCK         S5L8900_NAND_PAGES_PER_BLOCK
#define PAGES_PER_BANK          S5L8900_NAND_PAGES_PER_BANK
#define PAGE_SIZE_              S5L8900_NAND_BYTES_PER_PAGE
#define SPARE_SIZE              S5L8900_NAND_BYTES_PER_SPARE
#define PAGES_PER_SUBLOCK       (PAGES_PER_BLOCK * BANKS)
#define META_SIZE               12

#define FIL_SIGNATURE           0x43303033
#define BBT_BLOCK               (BLOCKS_PER_BANK - 1)

#define VFL_INFO_BLOCK          1
#define VFL_INFO_SECTION_SIZE   4
#define VFL_CXT_COPIES          8
#define VFL_RESERVED_START      (VFL_INFO_BLOCK + VFL_INFO_SECTION_SIZE)
#define VFL_RESERVED_SIZE       196
#define VFL_FTL_START           (VFL_RESERVED_START + VFL_RESERVED_SIZE)     // 201, iBoot's own number
#define VFL_BBT_REPLACEMENT     VFL_RESERVED_START                          // where the BBT block's data goes
#define VFL_CXT_AGE             0xFFFFFF00
#define VFL_CTX_SPARE_TYPE      0x80
#define VFL_CXT_CHECKSUM_OFFSET 0x7F8
#define VFL_CXT_CHECKSUM_SEED   0xAABBCCDD

#define FTL_CXT_SECTION_SIZE    3
#define FREE_SECTION_SIZE       20
#define LOG_SECTION_SIZE        (FREE_SECTION_SIZE - 3)
#define FTL_DATA_VBN_START      (FTL_CXT_SECTION_SIZE + FREE_SECTION_SIZE)
#define FTL_NUM_OF_LBNS         3872                                        // USER_SUBLKS_TOTAL
#define FTL_CXT_VBN             0
#define FTL_MAP_TABLE_FIRST_VPN 1
#define FTL_MAP_TABLE_PAGES     DIV_ROUND_UP(FTL_NUM_OF_LBNS * 2, PAGE_SIZE_)
#define FTL_CXT_AGE             0xFFFFFFFF
#define FTL_SPARE_TYPE_CXT      0x43
#define FTL_SPARE_TYPE_DATA     0x40
#define FTL_VERSION             0x46560000

QEMU_BUILD_BUG_ON(VFL_FTL_START + FTL_DATA_VBN_START + FTL_NUM_OF_LBNS != BLOCKS_PER_BANK);

#define LBN_SIZE                ((uint64_t)PAGES_PER_SUBLOCK * PAGE_SIZE_)
#define DISK_SIZE               (FTL_NUM_OF_LBNS * LBN_SIZE)
#define PAGE_AND_SPARE          (PAGE_SIZE_ + SPARE_SIZE)

// the state of a page in the overlay
enum {
    OVERLAY_UNCHANGED = 0,  // the page of the formatted NAND
    OVERLAY_PROGRAMMED = 1, // the page in the overlay's data and spare files
    OVERLAY_ERASED = 2,
};

// the chip block of a VFL virtual block
static uint32_t vfl_physical_block(uint32_t vbn)
{
    return vbn == BBT_BLOCK ? VFL_BBT_REPLACEMENT : vbn;
}

// FTL virtual page -> bank and page on that bank
static void ftl_page_to_ppn(uint32_t vpn, uint32_t *bank, uint32_t *ppn)
{
    uint32_t vfl_vpn = vpn + VFL_FTL_START * PAGES_PER_SUBLOCK;
    *bank = vfl_vpn % BANKS;
    *ppn = vfl_physical_block(vfl_vpn / PAGES_PER_SUBLOCK) * PAGES_PER_BLOCK + (vfl_vpn / BANKS) % PAGES_PER_BLOCK;
}

// the inverse for the pages of the VFL's virtual blocks; false for the others
static bool ppn_to_ftl_page(uint32_t bank, uint32_t ppn, uint32_t *vpn)
{
    uint32_t block = ppn / PAGES_PER_BLOCK;
    if (block == VFL_BBT_REPLACEMENT) {
        block = BBT_BLOCK;
    } else if (block < VFL_FTL_START || block == BBT_BLOCK) {
        return false;
    }
    *vpn = (block - VFL_FTL_START) * PAGES_PER_SUBLOCK + (ppn % PAGES_PER_BLOCK) * BANKS + bank;
    return true;
}

static uint64_t page_key(uint32_t bank, uint32_t page)
{
    return ((uint64_t)bank << 32) | page;
}

// Adds a page of the formatted NAND: the data, the meta data (the rest of it erased) and a zero ECC.
static void add_special_page(S5L8900NANDDisk *disk, uint32_t bank, uint32_t page, const uint8_t *data,
                             const uint8_t *meta, size_t meta_len)
{
    uint8_t *buf = g_malloc(PAGE_AND_SPARE);
    memcpy(buf, data, PAGE_SIZE_);
    memset(buf + PAGE_SIZE_, 0xFF, META_SIZE);
    memcpy(buf + PAGE_SIZE_, meta, meta_len);
    memset(buf + PAGE_SIZE_ + META_SIZE, 0, SPARE_SIZE - META_SIZE);

    uint64_t *key = g_new(uint64_t, 1);
    *key = page_key(bank, page);
    g_hash_table_insert(disk->special_pages, key, buf);
}

// the meta data of the VFL and FTL context pages: {age, 0, status, type}
static void context_meta(uint8_t *meta, uint32_t age, uint8_t status, uint8_t type)
{
    stl_le_p(meta, age);
    stl_le_p(meta + 4, 0);
    meta[8] = status;
    meta[9] = type;
}

static void format_signature_and_bbt(S5L8900NANDDisk *disk)
{
    g_autofree uint8_t *page = g_malloc0(PAGE_SIZE_);
    uint8_t meta[META_SIZE] = { 0 };

    stl_le_p(page, FIL_SIGNATURE);
    add_special_page(disk, 0, 0, page, meta, sizeof(meta));

    // the factory bad block table: all blocks good
    memset(page, 0, PAGE_SIZE_);
    memcpy(page, "DEVICEINFOBBT", 13);
    stl_le_p(page + 0x34, BLOCKS_PER_BANK / 8);
    memset(page + 0x38, 0xFF, BLOCKS_PER_BANK / 8);
    for (uint32_t bank = 0; bank < BANKS; bank++) {
        add_special_page(disk, bank, BBT_BLOCK * PAGES_PER_BLOCK, page, meta, sizeof(meta));
    }
}

// The sum and the XOR of the words before them, each combined with a seed. iBoot 1.1.5 checks them, if loosely.
static void vfl_context_checksum(uint8_t *ctx)
{
    uint32_t sum = 0, xor = 0;

    for (int i = 0; i < VFL_CXT_CHECKSUM_OFFSET; i += 4) {
        sum += ldl_le_p(ctx + i);
        xor ^= ldl_le_p(ctx + i);
    }
    stl_le_p(ctx + VFL_CXT_CHECKSUM_OFFSET, sum + VFL_CXT_CHECKSUM_SEED);
    stl_le_p(ctx + VFL_CXT_CHECKSUM_OFFSET + 4, xor ^ VFL_CXT_CHECKSUM_SEED);
}

static void format_vfl_context(S5L8900NANDDisk *disk)
{
    g_autofree uint8_t *page = g_malloc0(PAGE_SIZE_);
    uint8_t meta[META_SIZE];

    for (uint32_t bank = 0; bank < BANKS; bank++) {
        memset(page, 0, PAGE_SIZE_);
        stl_le_p(page, bank);                                   // dwGlobalCxtAge
        for (int i = 0; i < FTL_CXT_SECTION_SIZE; i++) {
            stw_le_p(page + 4 + i * 2, FTL_CXT_VBN + i);        // aFTLCxtVbn
        }
        stl_le_p(page + 12, VFL_CXT_AGE);                       // dwCxtAge
        stw_le_p(page + 16, 0);                                 // wCxtLocation, an index in awInfoBlk
        stw_le_p(page + 18, VFL_CXT_COPIES);                    // wNextCxtPOffset
        stw_le_p(page + 20, 1);                                 // wNumOfInitBadBlk
        // the BBT block, replaced by the first reserved block
        stw_le_p(page + 26, 1);                                 // wBadMapTableMaxIdx
        stw_le_p(page + 28, VFL_RESERVED_START);                // wReservedSecStart
        stw_le_p(page + 30, VFL_RESERVED_SIZE);                 // wReservedSecSize
        stw_le_p(page + 32, BBT_BLOCK);                         // aBadMapTable[0]
        // aBadMark: a bit per 8 blocks, most significant first, clear where a block of the 8 is replaced
        memset(page + 0x688, 0xFF, BLOCKS_PER_BANK / 64);
        page[0x688 + BBT_BLOCK / 64] &= ~(0x80 >> ((BBT_BLOCK / 8) % 8));
        for (int i = 0; i < VFL_INFO_SECTION_SIZE; i++) {
            stw_le_p(page + 0x7a2 + i * 2, VFL_INFO_BLOCK + i); // awInfoBlk
        }
        vfl_context_checksum(page);

        context_meta(meta, VFL_CXT_AGE, 0, VFL_CTX_SPARE_TYPE);
        for (int i = 0; i < VFL_CXT_COPIES; i++) {
            add_special_page(disk, bank, VFL_INFO_BLOCK * PAGES_PER_BLOCK + i, page, meta, sizeof(meta));
        }
    }
}

static void add_ftl_page(S5L8900NANDDisk *disk, const uint8_t *data, uint32_t vpn)
{
    uint8_t meta[META_SIZE];
    uint32_t bank, ppn;

    context_meta(meta, FTL_CXT_AGE, 0xFF, FTL_SPARE_TYPE_CXT);
    ftl_page_to_ppn(vpn, &bank, &ppn);
    add_special_page(disk, bank, ppn, data, meta, sizeof(meta));
}

static void format_ftl_context(S5L8900NANDDisk *disk)
{
    g_autofree uint8_t *meta = g_malloc0(PAGE_SIZE_);
    g_autofree uint8_t *table = g_malloc0(PAGE_SIZE_);
    uint32_t ctx_vpn = FTL_CXT_VBN * PAGES_PER_SUBLOCK;

    // the first page identifies the block, and is the (empty) erase counter, read counter and log table page
    add_ftl_page(disk, table, ctx_vpn);

    stl_le_p(meta, FTL_CXT_AGE - 1);                            // dwAge
    stl_le_p(meta + 4, 1);                                      // dwWriteAge
    stw_le_p(meta + 8, FREE_SECTION_SIZE);                      // wNumOfFreeVb
    for (int i = 0; i < FREE_SECTION_SIZE; i++) {
        stw_le_p(meta + 14 + i * 2, FTL_CXT_SECTION_SIZE + i);  // awFreeVbList
    }
    for (int i = 0; i < LOG_SECTION_SIZE + 1; i++) {
        stw_le_p(meta + 420 + i * 20 + 4, 0xFFFF);              // aLOGCxtTable[i].wVbn
    }
    for (int i = 0; i < FTL_CXT_SECTION_SIZE; i++) {
        stw_le_p(meta + 786 + i * 2, FTL_CXT_VBN + i);          // awMapCxtVbn
    }

    // the logical block -> virtual block map
    uint32_t items_per_page = PAGE_SIZE_ / 2;
    for (int i = 0; i < FTL_MAP_TABLE_PAGES; i++) {
        uint32_t vpn = ctx_vpn + FTL_MAP_TABLE_FIRST_VPN + i;
        stl_le_p(meta + 56 + i * 4, vpn);                       // adwMapTablePtrs
        for (uint32_t j = 0; j < items_per_page; j++) {
            uint32_t lbn = i * items_per_page + j;
            stw_le_p(table + j * 2, lbn < FTL_NUM_OF_LBNS ? FTL_DATA_VBN_START + lbn : 0xFFFF);
        }
        add_ftl_page(disk, table, vpn);
    }

    // adwECTablePtrs (128), adwLOGCxtMapPtrs (272) and adwRCTablePtrs (800) stay zero: the first page
    QEMU_BUILD_BUG_ON(FTL_CXT_VBN != 0);
    stl_le_p(meta + 976, 0xFFFFFFFF);                           // adwStatPtrs: none
    stl_le_p(meta + 980, 0xFFFFFFFF);

    // the FTL meta data, in the last page of the block
    uint32_t meta_vpn = ctx_vpn + PAGES_PER_SUBLOCK - 1;
    stl_le_p(meta + 792, meta_vpn);                             // dwCurrMapCxtPage
    stl_le_p(meta + 796, 1);                                    // boolFlashCxtIsValid
    stl_le_p(meta + 2040, FTL_VERSION);                         // dwVersion
    stl_le_p(meta + 2044, ~FTL_VERSION);                        // dwVersionNot
    add_ftl_page(disk, meta, meta_vpn);
}

// Determines which logical blocks of the disk image hold data. Only those are presented as written blocks, the others
// are erased, so an empty image is an empty, formatted NAND. Without hole information, all blocks up to the end of the
// image.
static void find_data_lbns(S5L8900NANDDisk *disk)
{
    disk->data_lbns = bitmap_new(FTL_NUM_OF_LBNS);
#ifdef SEEK_DATA
    off_t pos = 0;
    while (pos < disk->disk_size) {
        off_t start = lseek(disk->disk_fd, pos, SEEK_DATA);
        if (start < 0) {
            break;
        }
        off_t end = lseek(disk->disk_fd, start, SEEK_HOLE);
        if (end < 0 || end > disk->disk_size) {
            end = disk->disk_size;
        }
        if (end > start) {
            bitmap_set(disk->data_lbns, start / LBN_SIZE, (end - 1) / LBN_SIZE - start / LBN_SIZE + 1);
        }
        pos = end;
    }
#else
    bitmap_set(disk->data_lbns, 0, DIV_ROUND_UP(disk->disk_size, LBN_SIZE));
#endif
}

// Reads a page of the formatted NAND with the disk image's data blocks in place. Returns false if it is erased.
static bool read_formatted_page(S5L8900NANDDisk *disk, uint32_t bank, uint32_t page, uint8_t *data, uint8_t *spare)
{
    uint64_t key = page_key(bank, page);
    uint8_t *special = g_hash_table_lookup(disk->special_pages, &key);
    if (special) {
        memcpy(data, special, PAGE_SIZE_);
        memcpy(spare, special + PAGE_SIZE_, SPARE_SIZE);
        return true;
    }

    uint32_t vpn;
    memset(data, 0xFF, PAGE_SIZE_);
    memset(spare, 0xFF, SPARE_SIZE);
    if (!ppn_to_ftl_page(bank, page, &vpn)) {
        return false;
    }
    uint32_t vbn = vpn / PAGES_PER_SUBLOCK;
    if (vbn < FTL_DATA_VBN_START || !test_bit(vbn - FTL_DATA_VBN_START, disk->data_lbns)) {
        return false;
    }

    // a data page with the meta data FTL_Read expects: the logical page and the ECC mark (byte 10) 0xff
    uint32_t lpn = (vbn - FTL_DATA_VBN_START) * PAGES_PER_SUBLOCK + vpn % PAGES_PER_SUBLOCK;
    uint64_t offset = (uint64_t)lpn * PAGE_SIZE_;
    ssize_t n = 0;
    if (offset < disk->disk_size) {
        n = pread(disk->disk_fd, data, PAGE_SIZE_, offset);
        if (n < 0) {
            error_report("%s: unable to read logical page %u of the disk image: %s", __func__, lpn, strerror(errno));
            n = 0;
        }
    }
    memset(data + n, 0, PAGE_SIZE_ - n);
    stl_le_p(spare, lpn);
    stl_le_p(spare + 4, 0);
    spare[9] = FTL_SPARE_TYPE_DATA;
    memset(spare + META_SIZE, 0, SPARE_SIZE - META_SIZE);
    return true;
}

static int open_overlay_file(const char *overlay, uint32_t bank, const char *ext, Error **errp)
{
    g_autofree char *path = g_strdup_printf("%s/bank%u.%s", overlay, bank, ext);
    return qemu_create(path, O_RDWR | O_BINARY, 0600, errp);
}

bool s5l8900_nand_disk_open(S5L8900NANDDisk *disk, const char *path, const char *overlay, Error **errp)
{
    disk->disk_fd = qemu_open(path, O_RDONLY | O_BINARY, errp);
    if (disk->disk_fd < 0) {
        return false;
    }
    off_t size = lseek(disk->disk_fd, 0, SEEK_END);
    if (size < 0 || size > DISK_SIZE) {
        error_setg(errp, "the NAND disk image %s must be at most %" PRIu64 " bytes", path, (uint64_t)DISK_SIZE);
        return false;
    }
    disk->disk_size = size;
    find_data_lbns(disk);

    disk->special_pages = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
    format_signature_and_bbt(disk);
    format_vfl_context(disk);
    format_ftl_context(disk);

    if (g_mkdir_with_parents(overlay, 0700) != 0) {
        error_setg_errno(errp, errno, "unable to create the NAND overlay %s", overlay);
        return false;
    }
    for (uint32_t bank = 0; bank < BANKS; bank++) {
        disk->overlay_data_fd[bank] = open_overlay_file(overlay, bank, "data", errp);
        if (disk->overlay_data_fd[bank] < 0) {
            return false;
        }
        disk->overlay_spare_fd[bank] = open_overlay_file(overlay, bank, "spare", errp);
        if (disk->overlay_spare_fd[bank] < 0) {
            return false;
        }
        disk->overlay_state_fd[bank] = open_overlay_file(overlay, bank, "state", errp);
        if (disk->overlay_state_fd[bank] < 0) {
            return false;
        }

        // a missing or short state file means that the pages are unchanged
        disk->overlay_state[bank] = g_malloc0(PAGES_PER_BANK);
        ssize_t n = pread(disk->overlay_state_fd[bank], disk->overlay_state[bank], PAGES_PER_BANK, 0);
        if (n < 0) {
            error_setg_errno(errp, errno, "unable to read the NAND overlay state of bank %u", bank);
            return false;
        }
    }
    return true;
}

bool s5l8900_nand_disk_read_page(S5L8900NANDDisk *disk, uint32_t bank, uint32_t page, uint8_t *data, uint8_t *spare)
{
    if (disk->disk_fd < 0 || bank >= BANKS || page >= PAGES_PER_BANK) {
        memset(data, 0xFF, PAGE_SIZE_);
        memset(spare, 0xFF, SPARE_SIZE);
        return false;
    }

    switch (disk->overlay_state[bank][page]) {
    case OVERLAY_PROGRAMMED:
        if (pread(disk->overlay_data_fd[bank], data, PAGE_SIZE_, (off_t)page * PAGE_SIZE_) != PAGE_SIZE_ ||
            pread(disk->overlay_spare_fd[bank], spare, SPARE_SIZE, (off_t)page * SPARE_SIZE) != SPARE_SIZE) {
            error_report("%s: unable to read page %u of bank %u from the NAND overlay", __func__, page, bank);
        }
        return true;
    case OVERLAY_ERASED:
        memset(data, 0xFF, PAGE_SIZE_);
        memset(spare, 0xFF, SPARE_SIZE);
        return false;
    default:
        return read_formatted_page(disk, bank, page, data, spare);
    }
}
