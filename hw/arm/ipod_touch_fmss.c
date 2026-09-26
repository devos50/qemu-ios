#include "hw/arm/ipod_touch_fmss.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "exec/cpu-common.h"

// the sequences of iBoot (iBoot-385.22) are part of its image, the kernel's sequences are in kernel memory
#define IBOOT_BASE 0x0ff00000
#define IBOOT_SIZE 0x100000

// iBoot looks up the bluetooth node under uart3, but on the device tree we use it is a child of uart1
#define IBOOT_BT_NODE_PATH_ADDR 0x0ff2206c
static const char iboot_bt_node_path_orig[] = "arm-io/uart3/bluetooth";
static const char iboot_bt_node_path_patched[] = "arm-io/uart1/bluetooth";

// Release iBoot ignores the boot-args variable: after loading the kernel it formats gBootArgs.commandLine as " " (a space
// and a NUL) and passes it to the kernel. The kernel's parser skips separators including NUL characters, so it also sees
// whatever follows in the buffer. We put our boot arguments there, behind two characters that iBoot overwrites.
#define IBOOT_BOOT_ARGS_ADDR 0x0ff2a584
#define IBOOT_BOOT_ARGS_SIZE 0x100
static const char boot_args[] = "  debug=0x8 cpus=1 rd=disk0s1 serial=1 pmu-debug=0x1 io=0xffff8fff debug-usb=0xffffffff amfi_allow_any_signature=1 -v zalloc_debug";
QEMU_BUILD_BUG_ON(sizeof(boot_args) > IBOOT_BOOT_ARGS_SIZE);

static uint32_t fmss_ldl(hwaddr addr)
{
    uint32_t val;
    cpu_physical_memory_read(addr, &val, sizeof(val));
    return le32_to_cpu(val);
}

static void fmss_stl(hwaddr addr, uint32_t val)
{
    val = cpu_to_le32(val);
    cpu_physical_memory_write(addr, &val, sizeof(val));
}

static uint32_t fmss_reg(IPodTouchFMSSState *s, hwaddr reg)
{
    return s->regs[reg / 4];
}

static char *nand_page_filename(IPodTouchFMSSState *s, uint32_t ce, uint32_t page)
{
    return g_strdup_printf("%s/cs%u/%u.page", s->nand_path, ce, page);
}

// Reads a page into the page and spare buffers. Returns false if the page is erased.
static bool nand_read_page(IPodTouchFMSSState *s, uint32_t ce, uint32_t page)
{
    g_autofree char *filename = nand_page_filename(s, ce, page);
    FILE *f = fopen(filename, "rb");
    if (!f) {
        memset(s->page_buffer, 0xFF, sizeof(s->page_buffer));
        memset(s->spare_buffer, 0xFF, sizeof(s->spare_buffer));
        return false;
    }

    if (fread(s->page_buffer, 1, NAND_BYTES_PER_PAGE, f) != NAND_BYTES_PER_PAGE ||
        fread(s->spare_buffer, 1, NAND_BYTES_PER_SPARE, f) != NAND_BYTES_PER_SPARE) {
        error_report("%s: %s is truncated", __func__, filename);
    }
    fclose(f);
    return true;
}

// Programs a page with the contents of the page and spare buffers.
static bool nand_program_page(IPodTouchFMSSState *s, uint32_t ce, uint32_t page)
{
    g_autofree char *dirname = g_strdup_printf("%s/cs%u", s->nand_path, ce);
    g_autofree char *filename = nand_page_filename(s, ce, page);

    if (g_file_test(filename, G_FILE_TEST_EXISTS)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: programming page %u on CE %u that is not erased\n", __func__, page, ce);
    }

    if (g_mkdir_with_parents(dirname, 0700) != 0) {
        error_report("%s: unable to create %s: %s", __func__, dirname, strerror(errno));
        return false;
    }

    FILE *f = fopen(filename, "wb");
    if (!f) {
        error_report("%s: unable to open %s: %s", __func__, filename, strerror(errno));
        return false;
    }
    bool ok = fwrite(s->page_buffer, 1, NAND_BYTES_PER_PAGE, f) == NAND_BYTES_PER_PAGE &&
              fwrite(s->spare_buffer, 1, NAND_BYTES_PER_SPARE, f) == NAND_BYTES_PER_SPARE;
    if (fclose(f) != 0 || !ok) {
        error_report("%s: unable to write %s", __func__, filename);
        return false;
    }
    return true;
}

static bool nand_erase_block(IPodTouchFMSSState *s, uint32_t ce, uint32_t block)
{
    bool ok = true;
    for (uint32_t i = 0; i < NAND_PAGES_PER_BLOCK; i++) {
        g_autofree char *filename = nand_page_filename(s, ce, block * NAND_PAGES_PER_BLOCK + i);
        if (unlink(filename) != 0 && errno != ENOENT) {
            error_report("%s: unable to erase %s: %s", __func__, filename, strerror(errno));
            ok = false;
        }
    }
    return ok;
}

// Converts a CE mask with a single bit set to a CE index. Returns -1 if the mask is invalid.
static int fmss_ce_from_mask(uint32_t mask)
{
    if (mask == 0 || (mask & (mask - 1)) || ctz32(mask) >= NAND_NUM_CE) {
        return -1;
    }
    return ctz32(mask);
}

// Determines the NAND operation that a sequence performs, from the NAND commands and parameters that it uses.
static FMSSOperation fmss_classify_sequence(hwaddr addr)
{
    g_autofree uint8_t *seq = g_malloc(FMSS_SEQ_MAX_SIZE);
    bool cmds[256] = { false };
    bool reg_cmd = false;
    uint32_t params = 0; // bit n is set if the sequence reads parameter register FMSS_PARAM_BASE + 4 * n

    cpu_physical_memory_read(addr, seq, FMSS_SEQ_MAX_SIZE);
    for (int i = 0; i < FMSS_SEQ_MAX_SIZE; i += FMSS_SEQ_INSN_SIZE) {
        uint16_t reg = lduw_le_p(seq + i);
        uint8_t op = seq[i + 3];
        uint32_t imm = ldl_le_p(seq + i + 4);

        if (op == FMSS_SEQ_OP_END) {
            break;
        } else if (op == FMSS_SEQ_OP_STORE_IMM && reg == FMSS_FMCMD) {
            cmds[imm & 0xFF] = true;
        } else if (op == FMSS_SEQ_OP_STORE_REG && reg == FMSS_FMCMD) {
            reg_cmd = true; // the program sequence takes its commands from the command list
        } else if (op == FMSS_SEQ_OP_LOAD && reg >= FMSS_PARAM_BASE && reg < FMSS_PARAM_BASE + FMSS_PARAM_SIZE) {
            params |= 1 << ((reg - FMSS_PARAM_BASE) / 4);
        }
    }

#define PARAM(reg) (1 << (((reg) - FMSS_PARAM_BASE) / 4))
    if (cmds[0x60] && cmds[0xD0]) {
        return FMSS_OP_ERASE;
    } else if (cmds[0x90]) {
        return FMSS_OP_READ_ID;
    } else if (cmds[0x00] && cmds[0x30] && (params & PARAM(FMSS_PARAM_NUM_PAGES)) && (params & PARAM(FMSS_PARAM_META_ADDR))) {
        return FMSS_OP_READ;
    } else if (reg_cmd && (params & PARAM(FMSS_PARAM_CE_ADDR)) && (params & PARAM(FMSS_PARAM_META_ADDR))) {
        return FMSS_OP_PROGRAM;
    } else if (cmds[0xFF]) {
        return FMSS_OP_RESET;
    }
#undef PARAM
    return FMSS_OP_UNKNOWN;
}

static bool fmss_valid_chunks(uint32_t chunks)
{
    return chunks != 0 && chunks <= NAND_BYTES_PER_PAGE && NAND_BYTES_PER_PAGE % chunks == 0;
}

static uint32_t fmss_read_id(IPodTouchFMSSState *s)
{
    hwaddr out = fmss_reg(s, FMSS_PARAM_ID_OUT_ADDR);
    for (int ce = 0; ce < NAND_NUM_CE; ce++) {
        fmss_stl(out + ce * 4, NAND_CHIP_ID);
    }
    return 0;
}

static uint32_t fmss_read(IPodTouchFMSSState *s)
{
    uint32_t num_pages = fmss_reg(s, FMSS_PARAM_NUM_PAGES);
    uint32_t chunks = fmss_reg(s, FMSS_PARAM_CHUNKS);
    bool all_clean = true;

    if (num_pages == 0 || num_pages > FMSS_MAX_PAGES_PER_OP || !fmss_valid_chunks(chunks)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid read of %u pages in %u chunks\n", __func__, num_pages, chunks);
        return FMSS_STATUS_ERROR;
    }

    uint32_t chunk_size = NAND_BYTES_PER_PAGE / chunks;
    for (uint32_t i = 0; i < num_pages; i++) {
        uint32_t page = fmss_ldl(fmss_reg(s, FMSS_PARAM_PAGES_ADDR) + i * 4);
        uint32_t ce_mask = fmss_ldl(fmss_reg(s, FMSS_PARAM_CE_ADDR) + i * 4);
        int ce = fmss_ce_from_mask(ce_mask);
        if (ce < 0) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid CE mask 0x%x for page %u\n", __func__, ce_mask, page);
            return FMSS_STATUS_ERROR;
        }

        if (nand_read_page(s, ce, page)) {
            all_clean = false;
        }

        for (uint32_t c = 0; c < chunks; c++) {
            hwaddr dst = fmss_ldl(fmss_reg(s, FMSS_PARAM_DMA_ADDR) + (i * chunks + c) * 4);
            cpu_physical_memory_write(dst, s->page_buffer + c * chunk_size, chunk_size);
        }
        cpu_physical_memory_write(fmss_reg(s, FMSS_PARAM_META_ADDR) + i * FMSS_META_SIZE, s->spare_buffer, FMSS_META_SIZE);
    }

    return all_clean ? FMSS_STATUS_CLEAN : 0;
}

static uint32_t fmss_program(IPodTouchFMSSState *s)
{
    uint32_t chunks = fmss_reg(s, FMSS_PARAM_CHUNKS);
    if (!fmss_valid_chunks(chunks)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid number of chunks %u\n", __func__, chunks);
        return FMSS_STATUS_ERROR;
    }

    // the command list holds a {command, page} pair for every page and ends with a zero command
    uint32_t chunk_size = NAND_BYTES_PER_PAGE / chunks;
    for (uint32_t i = 0; ; i++) {
        hwaddr entry = fmss_reg(s, FMSS_PARAM_CE_ADDR) + i * 8;
        uint32_t cmd = fmss_ldl(entry);
        if (cmd == 0) {
            break;
        }

        uint32_t page = fmss_ldl(entry + 4);
        int ce = fmss_ce_from_mask(cmd & 0xFF);
        if (i >= FMSS_MAX_PAGES_PER_OP || ce < 0) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid command 0x%08x for page %u (entry %u)\n", __func__, cmd, page, i);
            return FMSS_STATUS_ERROR;
        }

        for (uint32_t c = 0; c < chunks; c++) {
            hwaddr src = fmss_ldl(fmss_reg(s, FMSS_PARAM_DMA_ADDR) + (i * chunks + c) * 4);
            cpu_physical_memory_read(src, s->page_buffer + c * chunk_size, chunk_size);
        }
        memset(s->spare_buffer, 0xFF, sizeof(s->spare_buffer));
        cpu_physical_memory_read(fmss_reg(s, FMSS_PARAM_META_ADDR) + i * FMSS_META_SIZE, s->spare_buffer, FMSS_META_SIZE);

        if (!nand_program_page(s, ce, page)) {
            return FMSS_STATUS_ERROR;
        }
    }
    return 0;
}

static uint32_t fmss_erase(IPodTouchFMSSState *s)
{
    uint32_t num_blocks = fmss_reg(s, FMSS_PARAM_NUM_PAGES);
    if (num_blocks == 0 || num_blocks > FMSS_MAX_PAGES_PER_OP) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid erase of %u blocks\n", __func__, num_blocks);
        return FMSS_STATUS_ERROR;
    }

    for (uint32_t i = 0; i < num_blocks; i++) {
        uint32_t page = fmss_ldl(fmss_reg(s, FMSS_PARAM_PAGES_ADDR) + i * 4);
        uint32_t ce = fmss_ldl(fmss_reg(s, FMSS_PARAM_CE_ADDR) + i * 4); // the erase sequence takes CE indices, not masks
        if (ce >= NAND_NUM_CE) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid CE %u for page %u\n", __func__, ce, page);
            return FMSS_STATUS_ERROR;
        }
        if (!nand_erase_block(s, ce, page / NAND_PAGES_PER_BLOCK)) {
            return FMSS_STATUS_ERROR;
        }
    }
    return 0;
}

// Applies our changes to iBoot when it runs its first sequence, once it has been loaded (and decrypted) and cleared its bss.
static void fmss_patch_iboot(IPodTouchFMSSState *s)
{
    char path[sizeof(iboot_bt_node_path_orig)];

    cpu_physical_memory_read(IBOOT_BT_NODE_PATH_ADDR, path, sizeof(path));
    if (memcmp(path, iboot_bt_node_path_orig, sizeof(path)) == 0) {
        cpu_physical_memory_write(IBOOT_BT_NODE_PATH_ADDR, iboot_bt_node_path_patched, sizeof(iboot_bt_node_path_patched));
        cpu_physical_memory_write(IBOOT_BOOT_ARGS_ADDR, boot_args, sizeof(boot_args));
        s->iboot_patched = true;
    }
}

static void fmss_run_sequence(IPodTouchFMSSState *s, bool irq_enabled)
{
    hwaddr seq = fmss_reg(s, FMSS_CS_BASEADDR);

    if (seq >= IBOOT_BASE && seq < IBOOT_BASE + IBOOT_SIZE && !s->iboot_patched) {
        fmss_patch_iboot(s);
    }

    switch (fmss_classify_sequence(seq)) {
        case FMSS_OP_RESET:
            s->status = 0;
            break;
        case FMSS_OP_READ_ID:
            s->status = fmss_read_id(s);
            break;
        case FMSS_OP_READ:
            s->status = fmss_read(s);
            break;
        case FMSS_OP_PROGRAM:
            s->status = fmss_program(s);
            break;
        case FMSS_OP_ERASE:
            s->status = fmss_erase(s);
            break;
        default:
            qemu_log_mask(LOG_UNIMP, "%s: unsupported sequence at 0x%08" HWADDR_PRIx "\n", __func__, seq);
            s->status = FMSS_STATUS_ERROR;
            break;
    }

    // the operation completes immediately
    s->cs_irq |= FMSS_CS_IRQ_DONE;
    if (irq_enabled) {
        qemu_set_irq(s->irq, 1);
    }
}

static uint64_t ipod_touch_fmss_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodTouchFMSSState *s = (IPodTouchFMSSState *)opaque;

    switch (addr) {
        case FMSS_FMCTRL1:
            return FMSS_FMCTRL1_IDLE;
        case FMSS_CS_STATE:
            return 0;
        case FMSS_CS_IRQ:
            return s->cs_irq;
        case FMSS_CS_STATUS:
            return s->status;
        case FMSS_CS_BUF_RST_OK:
            return 1;
        default:
            return s->regs[addr / 4];
    }
}

static void ipod_touch_fmss_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IPodTouchFMSSState *s = (IPodTouchFMSSState *)opaque;

    switch (addr) {
        case FMSS_CS_CTRL:
            if ((val & FMSS_CS_CTRL_START_MASK) == FMSS_CS_CTRL_START) {
                fmss_run_sequence(s, val & FMSS_CS_CTRL_IRQ_EN);
            }
            break;
        case FMSS_CS_IRQ:
            s->cs_irq = 0;
            qemu_set_irq(s->irq, 0);
            break;
        default:
            s->regs[addr / 4] = val;
            break;
    }
}

static const MemoryRegionOps fmss_ops = {
    .read = ipod_touch_fmss_read,
    .write = ipod_touch_fmss_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipod_touch_fmss_reset(DeviceState *dev)
{
    IPodTouchFMSSState *s = IPOD_TOUCH_FMSS(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[FMSS_CS_IRQMASK / 4] = 1;
    s->regs[FMSS_PARAM_UNKNOWN_D00 / 4] = 42; // unknown, kept from the earlier implementation
    s->cs_irq = 0;
    s->status = 0;
    s->iboot_patched = false;
    qemu_set_irq(s->irq, 0);
}

static void ipod_touch_fmss_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    IPodTouchFMSSState *s = IPOD_TOUCH_FMSS(obj);

    memory_region_init_io(&s->iomem, obj, &fmss_ops, s, "fmss", FMSS_REG_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static void ipod_touch_fmss_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = ipod_touch_fmss_reset;
}

static const TypeInfo ipod_touch_fmss_info = {
    .name          = TYPE_IPOD_TOUCH_FMSS,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchFMSSState),
    .instance_init = ipod_touch_fmss_init,
    .class_init    = ipod_touch_fmss_class_init,
};

static void ipod_touch_machine_types(void)
{
    type_register_static(&ipod_touch_fmss_info);
}

type_init(ipod_touch_machine_types)
