#include "hw/arm/ipod_touch_nand.h"
#include "qemu/bitmap.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qapi/error.h"

/*
 * The layout of a freshly formatted NAND, as the VSVFL production format and FTL_Format of the iPhone OS 2.x kernel
 * write it. Ported from generate_nand.c of generate_nand_it2g. The pipeline's nand.py relies on the position of the
 * data pages (virtual_page()) when it folds the overlay.
 */
#define NUM_BANKS                       8   // the two planes of each CE are exposed as two banks
#define BLOCKS_PER_BANK                 (NAND_BLOCKS_PER_CE / 2)
#define PAGES_PER_SUBLOCK               (NAND_PAGES_PER_BLOCK * NUM_BANKS)
#define META_SIZE                       12

// VSVFL gives 976 out of every 1024 blocks to the user super blocks, the rest is reserved to replace bad blocks
#define VFL_NUM_OF_SUBLKS               ((0x3d0 * BLOCKS_PER_BANK) / 1024)
#define VFL_RESERVED_BLOCKS_PER_BANK    (BLOCKS_PER_BANK - VFL_NUM_OF_SUBLKS)
#define VFL_CXT_BLOCK                   1
#define VFL_INFO_SECTION_SIZE           4
#define VFL_NUM_OF_VFL_CXT_COPIES       8
#define VFL_META_VERSION                2
#define VFL_VENDOR_SPECIFIC_TYPE        0x100014
#define VFL_CTX_SPARE_TYPE              0x80
#define VFL_BAD_MAP_TABLE_AVAILABLE     0xFFF0
#define VFL_BAD_MAP_TABLE_UNUSABLE      0xFFFF
#define WMR_MAX_RESERVED_SIZE           820
#define VFL_INITIAL_CXT_AGE             0xFFFFFF00

#define FTL_CXT_SECTION_SIZE            3
#define FREE_SECTION_SIZE               20
#define LOG_SECTION_SIZE                (FREE_SECTION_SIZE - 3)
#define FTL_DATA_VBN_START              (FTL_CXT_SECTION_SIZE + FREE_SECTION_SIZE)
#define FTL_NUM_OF_LBNS                 (VFL_NUM_OF_SUBLKS - FTL_DATA_VBN_START)
#define FTL_CXT_VBN                     0
#define FTL_MAP_TABLE_FIRST_VPN         5
#define FTL_INITIAL_CXT_AGE             0xFFFFFFFF
#define FTL_SPARE_TYPE_CXT_INDEX        0x43
#define FTL_SPARE_TYPE_DATA             0x41
#define MAX_NUM_OF_MAP_TABLES           18

#define LBN_SIZE                        ((uint64_t)PAGES_PER_SUBLOCK * NAND_BYTES_PER_PAGE)
#define DISK_SIZE                       (FTL_NUM_OF_LBNS * LBN_SIZE)

/*
 * The NAND signature and the bad block table are stored in the last two blocks of each CE: the last reserved blocks of
 * the second and the first plane. They need blocks of their own, because the NAND driver erases every block that holds
 * a signature when it updates it (e.g. the epoch update when iPhone OS 2.2 meets a NAND formatted for 2.1.1).
 */
#define NAND_SIG_PAGE                   ((NAND_BLOCKS_PER_CE - 1) * NAND_PAGES_PER_BLOCK)
#define BBT_PAGE                        ((NAND_BLOCKS_PER_CE - 2) * NAND_PAGES_PER_BLOCK)

// chip blocks that are replaced by a block in the reserved area of the same plane (block 0 and the VFL info blocks)
static const uint32_t remapped_blocks[] = { 0, 1, 2, 3, 4 };

#define PAGE_AND_SPARE                  (NAND_BYTES_PER_PAGE + NAND_BYTES_PER_SPARE)

// the state of a page in the overlay
enum {
    OVERLAY_UNCHANGED = 0,  // the page of the formatted NAND
    OVERLAY_PROGRAMMED = 1, // the page in the overlay's data and spare files
    OVERLAY_ERASED = 2,
};

static uint32_t remap_chip_block(uint32_t chip_block)
{
    uint32_t plane_idx[2] = { 0, 0 };
    for (int i = 0; i < ARRAY_SIZE(remapped_blocks); i++) {
        uint32_t plane = remapped_blocks[i] & 1;
        if (remapped_blocks[i] == chip_block) {
            return (VFL_NUM_OF_SUBLKS + plane_idx[plane]) * 2 + plane;
        }
        plane_idx[plane]++;
    }
    return chip_block;
}

// virtual page -> CE and physical page on that CE, like VSVFL
static void vpn_to_ppn(uint32_t vpn, uint32_t *ce, uint32_t *ppn)
{
    uint32_t bank = vpn % NUM_BANKS;
    uint32_t vbn = vpn / PAGES_PER_SUBLOCK;
    uint32_t page_in_block = (vpn % PAGES_PER_SUBLOCK) / NUM_BANKS;
    // two-plane LSB reordering: the banks >= NAND_NUM_CE map to the odd blocks of a CE
    uint32_t chip_block = vbn * 2 + bank / NAND_NUM_CE;
    *ce = bank % NAND_NUM_CE;
    *ppn = remap_chip_block(chip_block) * NAND_PAGES_PER_BLOCK + page_in_block;
}

// the inverse of vpn_to_ppn for the blocks that are not remapped, which include all data blocks
static uint32_t virtual_page(uint32_t ce, uint32_t ppn)
{
    uint32_t chip_block = ppn / NAND_PAGES_PER_BLOCK;
    uint32_t page_in_block = ppn % NAND_PAGES_PER_BLOCK;
    return (chip_block / 2) * PAGES_PER_SUBLOCK + page_in_block * NUM_BANKS + ce + NAND_NUM_CE * (chip_block % 2);
}

static uint64_t page_key(uint32_t ce, uint32_t page)
{
    return ((uint64_t)ce << 32) | page;
}

// Adds a page of the formatted NAND. The parts of the spare area that meta does not cover stay erased.
static void add_special_page(IPodTouchNAND *nand, uint32_t ce, uint32_t page, const uint8_t *data, const uint8_t *meta,
                             size_t meta_len)
{
    uint8_t *buf = g_malloc(PAGE_AND_SPARE);
    memcpy(buf, data, NAND_BYTES_PER_PAGE);
    memset(buf + NAND_BYTES_PER_PAGE, 0xFF, NAND_BYTES_PER_SPARE);
    memcpy(buf + NAND_BYTES_PER_PAGE, meta, meta_len);

    uint64_t *key = g_new(uint64_t, 1);
    *key = page_key(ce, page);
    g_hash_table_insert(nand->special_pages, key, buf);
}

// the spare area of the VFL and FTL context pages: {age, 0, status, type}
static void context_spare(uint8_t *spare, uint32_t age, uint8_t status, uint8_t type)
{
    stl_le_p(spare, age);
    stl_le_p(spare + 4, 0);
    spare[8] = status;
    spare[9] = type;
}

static void format_vfl_context(IPodTouchNAND *nand)
{
    g_autofree uint8_t *page = g_malloc0(NAND_BYTES_PER_PAGE);
    uint8_t spare[10];

    for (uint32_t ce = 0; ce < NAND_NUM_CE; ce++) {
        memset(page, 0, NAND_BYTES_PER_PAGE);
        stl_le_p(page, ce);                                 // dwGlobalCxtAge
        stl_le_p(page + 4, VFL_INITIAL_CXT_AGE);            // dwCxtAge
        stl_le_p(page + 8, 0);                              // dwFTLType
        // the context is written in groups of VFL_NUM_OF_VFL_CXT_COPIES pages to info block wCxtLocation (an index
        // in awInfoBlk); the next group goes after the first one
        stw_le_p(page + 12, 0);
        stw_le_p(page + 14, VFL_NUM_OF_VFL_CXT_COPIES);

        // replace the blocks that overlap with the VFL info area by blocks from the reserved area of the same plane
        uint16_t bad_map[WMR_MAX_RESERVED_SIZE];
        uint32_t plane_idx[2] = { 0, 0 };
        for (int i = 0; i < WMR_MAX_RESERVED_SIZE; i++) {
            bad_map[i] = VFL_BAD_MAP_TABLE_AVAILABLE;
        }
        for (int i = 0; i < ARRAY_SIZE(remapped_blocks); i++) {
            uint32_t plane = remapped_blocks[i] & 1;
            bad_map[plane * VFL_RESERVED_BLOCKS_PER_BANK + plane_idx[plane]] = remapped_blocks[i];
            plane_idx[plane]++;
        }
        // the last reserved blocks of the two planes hold the BBT and the NAND signature
        bad_map[VFL_RESERVED_BLOCKS_PER_BANK - 1] = VFL_BAD_MAP_TABLE_UNUSABLE;
        bad_map[2 * VFL_RESERVED_BLOCKS_PER_BANK - 1] = VFL_BAD_MAP_TABLE_UNUSABLE;

        stw_le_p(page + 16, ARRAY_SIZE(remapped_blocks));  // wNumOfInitBadBlk
        stw_le_p(page + 22, plane_idx[0]);                  // awReplacementIdx
        stw_le_p(page + 24, plane_idx[1]);
        for (int i = 0; i < WMR_MAX_RESERVED_SIZE; i++) {
            stw_le_p(page + 38 + i * 2, bad_map[i]);
        }
        for (int i = 0; i < VFL_INFO_SECTION_SIZE; i++) {
            stw_le_p(page + 1678 + i * 2, VFL_CXT_BLOCK + i);
        }
        stw_le_p(page + 1686, VFL_NUM_OF_SUBLKS);           // FTL and VFL super blocks
        stw_le_p(page + 1688, VFL_NUM_OF_SUBLKS);
        for (int i = 0; i < FTL_CXT_SECTION_SIZE; i++) {
            stw_le_p(page + 1690 + i * 2, FTL_CXT_VBN + i);
        }
        stl_le_p(page + 1754, VFL_VENDOR_SPECIFIC_TYPE);
        stl_le_p(page + 2036, VFL_META_VERSION);

        context_spare(spare, VFL_INITIAL_CXT_AGE, 0, VFL_CTX_SPARE_TYPE);
        for (int i = 0; i < VFL_NUM_OF_VFL_CXT_COPIES; i++) {
            add_special_page(nand, ce, VFL_CXT_BLOCK * NAND_PAGES_PER_BLOCK + i, page, spare, sizeof(spare));
        }
    }
}

static void add_ftl_page(IPodTouchNAND *nand, const uint8_t *data, uint32_t vpn)
{
    uint8_t spare[10];
    uint32_t ce, ppn;

    context_spare(spare, FTL_INITIAL_CXT_AGE, 0xFF, FTL_SPARE_TYPE_CXT_INDEX);
    vpn_to_ppn(vpn, &ce, &ppn);
    add_special_page(nand, ce, ppn, data, spare, sizeof(spare));
}

static void format_ftl_context(IPodTouchNAND *nand)
{
    g_autofree uint8_t *meta = g_malloc0(NAND_BYTES_PER_PAGE);
    g_autofree uint8_t *table = g_malloc0(NAND_BYTES_PER_PAGE);
    uint32_t ctx_vpn = FTL_CXT_VBN * PAGES_PER_SUBLOCK;

    // the first page of a FTL context block identifies the block, and is the (empty) EC/RC/log table page
    add_ftl_page(nand, table, ctx_vpn);

    stl_le_p(meta, FTL_INITIAL_CXT_AGE - 1);                // dwAge
    stl_le_p(meta + 4, 1);                                  // dwWriteAge
    stw_le_p(meta + 8, FREE_SECTION_SIZE);                  // wNumOfFreeVb
    for (int i = 0; i < FREE_SECTION_SIZE; i++) {
        stw_le_p(meta + 14 + i * 2, FTL_CXT_SECTION_SIZE + i);
    }
    for (int i = 0; i < LOG_SECTION_SIZE + 1; i++) {
        stw_le_p(meta + 420 + i * 20 + 4, 0xFFFF);          // aLOGCxtTable[i].wVbn
    }
    for (int i = 0; i < FTL_CXT_SECTION_SIZE; i++) {
        stw_le_p(meta + 786 + i * 2, FTL_CXT_VBN + i);      // awMapCxtVbn
    }

    // the logical block -> virtual block mapping tables
    uint32_t items_per_map = NAND_BYTES_PER_PAGE / 2;
    for (int i = 0; i < MAX_NUM_OF_MAP_TABLES; i++) {
        uint32_t vpn = ctx_vpn + FTL_MAP_TABLE_FIRST_VPN + i;
        stl_le_p(meta + 56 + i * 4, vpn);
        for (uint32_t j = 0; j < items_per_map; j++) {
            uint32_t lbn = i * items_per_map + j;
            stw_le_p(table + j * 2, lbn < FTL_NUM_OF_LBNS ? lbn + FTL_DATA_VBN_START : 0xFFFF);
        }
        add_ftl_page(nand, table, vpn);
    }

    // the erase counter, log context and read counter tables are all zero, so they all use the first page (ctx_vpn,
    // which is 0: the pointers at 128, 272 and 800 stay zero)
    QEMU_BUILD_BUG_ON(FTL_CXT_VBN != 0);
    stl_le_p(meta + 976, 0xFFFFFFFF);                       // adwStatPtrs
    stl_le_p(meta + 980, 0xFFFFFFFF);

    // the FTL meta data goes into the last page of the context block, the FTL searches it backwards from there
    uint32_t meta_vpn = ctx_vpn + PAGES_PER_SUBLOCK - 1;
    stl_le_p(meta + 792, meta_vpn);                         // dwCurrMapCxtPage
    stl_le_p(meta + 796, 1);                                // boolFlashCxtIsValid
    stl_le_p(meta + 2040, 0x46560000);                      // dwVersion
    stl_le_p(meta + 2044, ~0x46560000u);                    // dwVersionNot
    add_ftl_page(nand, meta, meta_vpn);
}

// a page in the format the VFL uses for its special blocks: a signature, the data length and the data
static void add_vfl_special_page(IPodTouchNAND *nand, const char *magic, const uint8_t *data, uint32_t len,
                                 uint32_t page_index)
{
    g_autofree uint8_t *page = g_malloc0(NAND_BYTES_PER_PAGE);
    memcpy(page, magic, strlen(magic));
    stl_le_p(page + 0x34, len);
    memcpy(page + 0x38, data, len);
    for (uint32_t ce = 0; ce < NAND_NUM_CE; ce++) {
        add_special_page(nand, ce, page_index, page, NULL, 0);
    }
}

static void low_level_format(IPodTouchNAND *nand, uint32_t epoch)
{
    uint8_t sig[4];
    uint8_t bbt[NAND_BLOCKS_PER_CE / 8];

    format_vfl_context(nand);
    format_ftl_context(nand);
    // "C11" and the security epoch of the NAND driver that formatted the NAND (as a digit). A driver of another epoch
    // starts an epoch update, which iBoot can't do, so the NAND has to match the firmware.
    stl_le_p(sig, 0x43313100 | ('0' + epoch));
    add_vfl_special_page(nand, "NANDDRIVERSIGN", sig, sizeof(sig), NAND_SIG_PAGE);
    // all blocks are good; the VFL rebuilds its bad block map from awBadMapTable when it opens the NAND
    memset(bbt, 0xFF, sizeof(bbt));
    add_vfl_special_page(nand, "DEVICEINFOBBT", bbt, sizeof(bbt), BBT_PAGE);
}

// Determines which logical blocks of the disk image hold data. Only those are presented as written blocks, the others
// are erased, so an empty image is an empty, formatted NAND. Without hole information, all blocks up to the end of the
// image.
static void find_data_lbns(IPodTouchNAND *nand)
{
    nand->data_lbns = bitmap_new(FTL_NUM_OF_LBNS);
#ifdef SEEK_DATA
    off_t pos = 0;
    while (pos < nand->disk_size) {
        off_t start = lseek(nand->disk_fd, pos, SEEK_DATA);
        if (start < 0) {
            break;
        }
        off_t end = lseek(nand->disk_fd, start, SEEK_HOLE);
        if (end < 0 || end > nand->disk_size) {
            end = nand->disk_size;
        }
        if (end > start) {
            bitmap_set(nand->data_lbns, start / LBN_SIZE, (end - 1) / LBN_SIZE - start / LBN_SIZE + 1);
        }
        pos = end;
    }
#else
    bitmap_set(nand->data_lbns, 0, DIV_ROUND_UP(nand->disk_size, LBN_SIZE));
#endif
}

// Reads a page of the formatted NAND with the disk image's data blocks in place. Returns false if it is erased.
static bool read_formatted_page(IPodTouchNAND *nand, uint32_t ce, uint32_t page, uint8_t *data, uint8_t *spare)
{
    uint64_t key = page_key(ce, page);
    uint8_t *special = g_hash_table_lookup(nand->special_pages, &key);
    if (special) {
        memcpy(data, special, NAND_BYTES_PER_PAGE);
        memcpy(spare, special + NAND_BYTES_PER_PAGE, NAND_BYTES_PER_SPARE);
        return true;
    }

    uint32_t vpn = virtual_page(ce, page);
    uint32_t vbn = vpn / PAGES_PER_SUBLOCK;
    if (vbn < FTL_DATA_VBN_START || vbn >= FTL_DATA_VBN_START + FTL_NUM_OF_LBNS ||
        !test_bit(vbn - FTL_DATA_VBN_START, nand->data_lbns)) {
        memset(data, 0xFF, NAND_BYTES_PER_PAGE);
        memset(spare, 0xFF, NAND_BYTES_PER_SPARE);
        return false;
    }

    // a data page with the meta data that FTL_Read and _FTLRestore expect: the logical page, a write age older than
    // anything the FTL writes later and the data spare type. Bytes 8 and 10 stay 0xFF, they are the ECC mark.
    uint32_t lpn = (vbn - FTL_DATA_VBN_START) * PAGES_PER_SUBLOCK + vpn % PAGES_PER_SUBLOCK;
    uint64_t offset = (uint64_t)lpn * NAND_BYTES_PER_PAGE;
    ssize_t n = 0;
    if (offset < nand->disk_size) {
        n = pread(nand->disk_fd, data, NAND_BYTES_PER_PAGE, offset);
        if (n < 0) {
            error_report("%s: unable to read logical page %u of the disk image: %s", __func__, lpn, strerror(errno));
            n = 0;
        }
    }
    memset(data + n, 0, NAND_BYTES_PER_PAGE - n);
    memset(spare, 0xFF, NAND_BYTES_PER_SPARE);
    stl_le_p(spare, lpn);
    stl_le_p(spare + 4, 0);
    spare[9] = FTL_SPARE_TYPE_DATA;
    return true;
}

static int open_overlay_file(const char *overlay, uint32_t ce, const char *ext, Error **errp)
{
    g_autofree char *path = g_strdup_printf("%s/cs%u.%s", overlay, ce, ext);
    return qemu_create(path, O_RDWR | O_BINARY, 0600, errp);
}

bool ipod_touch_nand_open_disk(IPodTouchNAND *nand, const char *disk, const char *overlay, uint32_t epoch,
                               Error **errp)
{
    nand->disk_fd = qemu_open(disk, O_RDONLY | O_BINARY, errp);
    if (nand->disk_fd < 0) {
        return false;
    }
    off_t size = lseek(nand->disk_fd, 0, SEEK_END);
    if (size < 0 || size > DISK_SIZE) {
        error_setg(errp, "the NAND disk image %s must be at most %" PRIu64 " bytes", disk, (uint64_t)DISK_SIZE);
        return false;
    }
    nand->disk_size = size;
    find_data_lbns(nand);

    nand->special_pages = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
    low_level_format(nand, epoch);

    if (g_mkdir_with_parents(overlay, 0700) != 0) {
        error_setg_errno(errp, errno, "unable to create the NAND overlay %s", overlay);
        return false;
    }
    for (uint32_t ce = 0; ce < NAND_NUM_CE; ce++) {
        nand->overlay_data_fd[ce] = open_overlay_file(overlay, ce, "data", errp);
        if (nand->overlay_data_fd[ce] < 0) {
            return false;
        }
        nand->overlay_spare_fd[ce] = open_overlay_file(overlay, ce, "spare", errp);
        if (nand->overlay_spare_fd[ce] < 0) {
            return false;
        }
        nand->overlay_state_fd[ce] = open_overlay_file(overlay, ce, "state", errp);
        if (nand->overlay_state_fd[ce] < 0) {
            return false;
        }

        // a missing or short state file means that the pages are unchanged
        nand->overlay_state[ce] = g_malloc0(NAND_PAGES_PER_CE);
        ssize_t n = pread(nand->overlay_state_fd[ce], nand->overlay_state[ce], NAND_PAGES_PER_CE, 0);
        if (n < 0) {
            error_setg_errno(errp, errno, "unable to read the NAND overlay state of CE %u", ce);
            return false;
        }
    }
    return true;
}

void ipod_touch_nand_open_pages(IPodTouchNAND *nand, const char *dir)
{
    nand->page_dir = g_strdup(dir);
    nand->disk_fd = -1;
}

static char *page_filename(IPodTouchNAND *nand, uint32_t ce, uint32_t page)
{
    return g_strdup_printf("%s/cs%u/%u.page", nand->page_dir, ce, page);
}

static bool read_page_file(IPodTouchNAND *nand, uint32_t ce, uint32_t page, uint8_t *data, uint8_t *spare)
{
    g_autofree char *filename = page_filename(nand, ce, page);
    FILE *f = fopen(filename, "rb");
    if (!f) {
        memset(data, 0xFF, NAND_BYTES_PER_PAGE);
        memset(spare, 0xFF, NAND_BYTES_PER_SPARE);
        return false;
    }

    if (fread(data, 1, NAND_BYTES_PER_PAGE, f) != NAND_BYTES_PER_PAGE ||
        fread(spare, 1, NAND_BYTES_PER_SPARE, f) != NAND_BYTES_PER_SPARE) {
        error_report("%s: %s is truncated", __func__, filename);
    }
    fclose(f);
    return true;
}

static bool program_page_file(IPodTouchNAND *nand, uint32_t ce, uint32_t page, const uint8_t *data,
                              const uint8_t *spare)
{
    g_autofree char *dirname = g_strdup_printf("%s/cs%u", nand->page_dir, ce);
    g_autofree char *filename = page_filename(nand, ce, page);

    if (g_mkdir_with_parents(dirname, 0700) != 0) {
        error_report("%s: unable to create %s: %s", __func__, dirname, strerror(errno));
        return false;
    }

    FILE *f = fopen(filename, "wb");
    if (!f) {
        error_report("%s: unable to open %s: %s", __func__, filename, strerror(errno));
        return false;
    }
    bool ok = fwrite(data, 1, NAND_BYTES_PER_PAGE, f) == NAND_BYTES_PER_PAGE &&
              fwrite(spare, 1, NAND_BYTES_PER_SPARE, f) == NAND_BYTES_PER_SPARE;
    if (fclose(f) != 0 || !ok) {
        error_report("%s: unable to write %s", __func__, filename);
        return false;
    }
    return true;
}

static bool erase_page_files(IPodTouchNAND *nand, uint32_t ce, uint32_t block)
{
    bool ok = true;
    for (uint32_t i = 0; i < NAND_PAGES_PER_BLOCK; i++) {
        g_autofree char *filename = page_filename(nand, ce, block * NAND_PAGES_PER_BLOCK + i);
        if (unlink(filename) != 0 && errno != ENOENT) {
            error_report("%s: unable to erase %s: %s", __func__, filename, strerror(errno));
            ok = false;
        }
    }
    return ok;
}

static bool write_overlay_state(IPodTouchNAND *nand, uint32_t ce, uint32_t page, uint32_t count, uint8_t state)
{
    memset(nand->overlay_state[ce] + page, state, count);
    if (pwrite(nand->overlay_state_fd[ce], nand->overlay_state[ce] + page, count, page) != count) {
        error_report("%s: unable to write the NAND overlay state of CE %u: %s", __func__, ce, strerror(errno));
        return false;
    }
    return true;
}

// Gives the space of an erased block's data back to the host filesystem, where it can.
static void punch_overlay_data(IPodTouchNAND *nand, uint32_t ce, uint32_t block)
{
    off_t offset = (off_t)block * NAND_PAGES_PER_BLOCK * NAND_BYTES_PER_PAGE;
    off_t length = NAND_PAGES_PER_BLOCK * NAND_BYTES_PER_PAGE;
#if defined(F_PUNCHHOLE)
    struct fpunchhole punch = { .fp_offset = offset, .fp_length = length };
    fcntl(nand->overlay_data_fd[ce], F_PUNCHHOLE, &punch);
#elif defined(CONFIG_FALLOCATE_PUNCH_HOLE)
    fallocate(nand->overlay_data_fd[ce], FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, offset, length);
#endif
}

bool ipod_touch_nand_read_page(IPodTouchNAND *nand, uint32_t ce, uint32_t page, uint8_t *data, uint8_t *spare)
{
    if (ce >= NAND_NUM_CE || page >= NAND_PAGES_PER_CE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid page %u on CE %u\n", __func__, page, ce);
        memset(data, 0xFF, NAND_BYTES_PER_PAGE);
        memset(spare, 0xFF, NAND_BYTES_PER_SPARE);
        return false;
    }

    if (nand->disk_fd < 0) {
        if (!nand->page_dir) {
            memset(data, 0xFF, NAND_BYTES_PER_PAGE);
            memset(spare, 0xFF, NAND_BYTES_PER_SPARE);
            return false;
        }
        return read_page_file(nand, ce, page, data, spare);
    }

    switch (nand->overlay_state[ce][page]) {
        case OVERLAY_PROGRAMMED:
            if (pread(nand->overlay_data_fd[ce], data, NAND_BYTES_PER_PAGE,
                      (off_t)page * NAND_BYTES_PER_PAGE) != NAND_BYTES_PER_PAGE ||
                pread(nand->overlay_spare_fd[ce], spare, NAND_BYTES_PER_SPARE,
                      (off_t)page * NAND_BYTES_PER_SPARE) != NAND_BYTES_PER_SPARE) {
                error_report("%s: unable to read page %u of CE %u from the NAND overlay", __func__, page, ce);
            }
            return true;
        case OVERLAY_ERASED:
            memset(data, 0xFF, NAND_BYTES_PER_PAGE);
            memset(spare, 0xFF, NAND_BYTES_PER_SPARE);
            return false;
        default:
            return read_formatted_page(nand, ce, page, data, spare);
    }
}

bool ipod_touch_nand_program_page(IPodTouchNAND *nand, uint32_t ce, uint32_t page, const uint8_t *data,
                                  const uint8_t *spare)
{
    if (ce >= NAND_NUM_CE || page >= NAND_PAGES_PER_CE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid page %u on CE %u\n", __func__, page, ce);
        return false;
    }

    g_autofree uint8_t *old_data = g_malloc(NAND_BYTES_PER_PAGE);
    uint8_t old_spare[NAND_BYTES_PER_SPARE];
    if (ipod_touch_nand_read_page(nand, ce, page, old_data, old_spare)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: programming page %u on CE %u that is not erased\n", __func__, page, ce);
    }

    if (nand->disk_fd < 0) {
        if (!nand->page_dir) {
            error_report("%s: the machine has no NAND", __func__);
            return false;
        }
        return program_page_file(nand, ce, page, data, spare);
    }

    // the data first: a page only counts as programmed once its state says so
    if (pwrite(nand->overlay_data_fd[ce], data, NAND_BYTES_PER_PAGE,
               (off_t)page * NAND_BYTES_PER_PAGE) != NAND_BYTES_PER_PAGE ||
        pwrite(nand->overlay_spare_fd[ce], spare, NAND_BYTES_PER_SPARE,
               (off_t)page * NAND_BYTES_PER_SPARE) != NAND_BYTES_PER_SPARE) {
        error_report("%s: unable to write page %u of CE %u to the NAND overlay: %s", __func__, page, ce,
                     strerror(errno));
        return false;
    }
    return write_overlay_state(nand, ce, page, 1, OVERLAY_PROGRAMMED);
}

bool ipod_touch_nand_erase_block(IPodTouchNAND *nand, uint32_t ce, uint32_t block)
{
    if (ce >= NAND_NUM_CE || block >= NAND_BLOCKS_PER_CE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid block %u on CE %u\n", __func__, block, ce);
        return false;
    }

    if (nand->disk_fd < 0) {
        return !nand->page_dir || erase_page_files(nand, ce, block);
    }

    // this also erases the pages of the formatted NAND
    if (!write_overlay_state(nand, ce, block * NAND_PAGES_PER_BLOCK, NAND_PAGES_PER_BLOCK, OVERLAY_ERASED)) {
        return false;
    }
    punch_overlay_data(nand, ce, block);
    return true;
}
