/*
 * The S5L8900's ADM: a core next to the NAND controller that runs the firmware the 1.1.x kernel uploads
 * ("CalmADMFMCFirmware-17", in AppleS5L8900XADMFMC) and drives the NAND for it. The model doesn't run that firmware;
 * it carries out the firmware's commands itself, as the kernel's drivers issue them (iPhone OS 1.1.5):
 *
 * - CTRL (0x00): bit 0 runs the core, bit 1 reads as set while it is stopped, bit 2 soft-resets it. The kernel
 *   writes the physical addresses of the firmware's code sections to 0x50 and 0x68-0x80 and of its data sections to
 *   0x54 and 0x84-0x8c, then starts the core and waits for the firmware's POST code, the first byte of data section
 *   2, to reach 0x50 (the byte after it is an abort code, 0 for none).
 * - INT (0x04): reads the interrupts the core raises (bit 4, a command is done; bit 5; bit 6, the firmware has
 *   logged something); writing n acknowledges interrupt bit n, and writing 2 hands the core a command.
 * - A command is a big-endian block at data section 2 + 0x1104: the opcode at 0x24 (1 reset, 2 and 3 read, 4 and 5
 *   write, 6 erase), the number of pages at 0x28, the meta data bytes per page at 0x2c, the number of banks at 0x30
 *   and their chip enables from 0x34, flags at 0x3c (bit 0: no page data), the ECC mode at 0x3e, and for each page
 *   its bank (an index into the chip enables) from 0x44 and its page from 0x244. The interleaved commands (2 and 4)
 *   give a bank and first page per bank, and the pages go round the banks.
 * - The results are in 0x30-0x3c: a status (0 done, 1 hardware error, 2 bad argument, 0xff timed out), a completion
 *   code (0, or 2 with write errors) and, for reads, 0xfe when the pages are erased.
 *
 * The page data goes through the NAND controller's FIFO, which the kernel moves with DMA; the meta data (the first
 * bytes of each page's spare) through data section 3. A read is done at once, a write when the DMA has filled the
 * FIFO with its pages (the NAND's "stream-done").
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/s5l8900_adm.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "system/dma.h"

#define ADM_CTRL                0x00
#define ADM_CTRL_RUN                (1 << 0)
#define ADM_CTRL_STOPPED            (1 << 1)
#define ADM_CTRL_RESET              (1 << 2)
#define ADM_INT                 0x04
#define ADM_INT_COMMAND             2       // written: a command is waiting
#define ADM_INT_DONE                (1 << 4)
#define ADM_INT_MASK                0x70
#define ADM_RESULT              0x30
#define ADM_DATA_SECTION2       0x88
#define ADM_DATA_SECTION3       0x8c

#define ADM_POST_ONLINE         0x50

#define CMD_BLOCK               0x1104      // in data section 2
#define CMD_OPCODE              0x24
#define CMD_COUNT               0x28
#define CMD_META                0x2c
#define CMD_BANKS               0x30
#define CMD_CHIP_ENABLES        0x34
#define CMD_FLAGS               0x3c
#define CMD_FLAGS_NO_DATA           (1 << 0)
#define CMD_PAGE_BANKS          0x44
#define CMD_PAGES               0x244
#define CMD_SIZE                (CMD_PAGES + 4 * S5L8900_NAND_STREAM_PAGES)

#define OP_RESET                1           // and read the chips' IDs into data section 3
#define OP_READ_INTERLEAVED     2
#define OP_READ                 3
#define OP_WRITE_INTERLEAVED    4
#define OP_WRITE                5
#define OP_ERASE                6

#define RESULT_STATUS           0
#define RESULT_COMPLETION       1
#define RESULT_EXTRA            2
#define STATUS_BAD_ARGUMENT     2
#define EXTRA_ERASED            0xfe

#define WRITE_PAD               0x10        // the DMA adds a pad after each page it writes

static void adm_update_irq(S5L8900ADMState *s)
{
    qemu_set_irq(s->irq, (s->status & ADM_INT_MASK) != 0);
}

static void adm_done(S5L8900ADMState *s)
{
    s->status |= ADM_INT_DONE;
    adm_update_irq(s);
}

static bool adm_mem(S5L8900ADMState *s, int section, hwaddr offset, void *buf, hwaddr len, bool write)
{
    hwaddr addr = s->regs[section / 4] + offset;
    MemTxResult r = write ? dma_memory_write(&address_space_memory, addr, buf, len, MEMTXATTRS_UNSPECIFIED)
                          : dma_memory_read(&address_space_memory, addr, buf, len, MEMTXATTRS_UNSPECIFIED);
    if (r != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "s5l8900.adm: can't %s 0x%" HWADDR_PRIx " bytes at 0x%" HWADDR_PRIx "\n",
                      write ? "write" : "read", len, addr);
        return false;
    }
    return true;
}

// the bank and page of each page of a command
static bool adm_pages(const uint8_t *cmd, uint32_t count, bool interleaved, uint8_t *banks, uint32_t *pages)
{
    uint32_t nbanks = lduw_be_p(cmd + CMD_BANKS);

    if (count > S5L8900_NAND_STREAM_PAGES || (interleaved && (nbanks == 0 || nbanks > 8))) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t slot = interleaved ? i % nbanks : i;
        uint8_t index = cmd[CMD_PAGE_BANKS + slot];
        if (index >= 8) {
            return false;
        }
        banks[i] = cmd[CMD_CHIP_ENABLES + index];
        pages[i] = ldl_be_p(cmd + CMD_PAGES + 4 * slot) + (interleaved ? i / nbanks : 0);
        if (banks[i] >= S5L8900_NAND_BANKS) {
            return false;
        }
    }
    return true;
}

static void adm_reset_chips(S5L8900ADMState *s)
{
    uint32_t ids[S5L8900_NAND_BANKS];

    for (int i = 0; i < S5L8900_NAND_BANKS; i++) {
        stl_le_p(&ids[i], S5L8900_NAND_CHIP_ID);
    }
    adm_mem(s, ADM_DATA_SECTION3, 0, ids, sizeof(ids), true);
    adm_done(s);
}

static void adm_read(S5L8900ADMState *s, const uint8_t *cmd, uint32_t count, uint8_t *banks, uint32_t *pages)
{
    uint32_t meta_len = lduw_be_p(cmd + CMD_META);
    g_autofree uint8_t *data = g_malloc(S5L8900_NAND_BYTES_PER_PAGE);
    g_autofree uint8_t *meta = g_malloc0(count * meta_len + 1);
    uint8_t spare[S5L8900_NAND_BYTES_PER_SPARE];
    bool erased = true;

    if (meta_len > sizeof(spare)) {
        s->results[RESULT_STATUS] = STATUS_BAD_ARGUMENT;
        adm_done(s);
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (s5l8900_nand_disk_read_page(&s->nand->disk, banks[i], pages[i], data, spare)) {
            erased = false;
        }
        memcpy(meta + i * meta_len, spare, meta_len);
    }
    if (count * meta_len) {
        adm_mem(s, ADM_DATA_SECTION3, 0, meta, count * meta_len, true);
    }
    if (!(lduw_be_p(cmd + CMD_FLAGS) & CMD_FLAGS_NO_DATA)) {
        s5l8900_nand_stream_read(s->nand, count, banks, pages);
    }
    if (count && erased) {
        s->results[RESULT_EXTRA] = EXTRA_ERASED;
    }
    adm_done(s);
}

static void adm_write(S5L8900ADMState *s, const uint8_t *cmd, uint32_t count, uint8_t *banks, uint32_t *pages)
{
    uint32_t meta_len = lduw_be_p(cmd + CMD_META);
    g_autofree uint8_t *meta = g_malloc0(count * meta_len + 1);

    if (meta_len > S5L8900_NAND_STREAM_META) {
        s->results[RESULT_STATUS] = STATUS_BAD_ARGUMENT;
        adm_done(s);
        return;
    }
    if (count * meta_len) {
        adm_mem(s, ADM_DATA_SECTION3, 0, meta, count * meta_len, false);
    }
    // done when the NAND has the pages (adm_stream_done)
    s5l8900_nand_stream_write(s->nand, count, banks, pages, S5L8900_NAND_BYTES_PER_PAGE + WRITE_PAD, meta,
                              meta_len);
}

static void adm_command(S5L8900ADMState *s)
{
    g_autofree uint8_t *cmd = g_malloc(CMD_SIZE);
    uint8_t banks[S5L8900_NAND_STREAM_PAGES];
    uint32_t pages[S5L8900_NAND_STREAM_PAGES];

    memset(s->results, 0, sizeof(s->results));
    if (!adm_mem(s, ADM_DATA_SECTION2, CMD_BLOCK, cmd, CMD_SIZE, false)) {
        s->results[RESULT_STATUS] = STATUS_BAD_ARGUMENT;
        adm_done(s);
        return;
    }

    uint32_t op = lduw_be_p(cmd + CMD_OPCODE);
    uint32_t count = lduw_be_p(cmd + CMD_COUNT);
    bool interleaved = op == OP_READ_INTERLEAVED || op == OP_WRITE_INTERLEAVED;
    if (op != OP_RESET && !adm_pages(cmd, count, interleaved, banks, pages)) {
        qemu_log_mask(LOG_GUEST_ERROR, "s5l8900.adm: command 0x%x with %u pages is malformed\n", op, count);
        s->results[RESULT_STATUS] = STATUS_BAD_ARGUMENT;
        adm_done(s);
        return;
    }

    switch (op) {
    case OP_RESET:
        adm_reset_chips(s);
        break;
    case OP_READ:
    case OP_READ_INTERLEAVED:
        adm_read(s, cmd, count, banks, pages);
        break;
    case OP_WRITE:
    case OP_WRITE_INTERLEAVED:
        adm_write(s, cmd, count, banks, pages);
        break;
    case OP_ERASE:
        for (uint32_t i = 0; i < count; i++) {
            s5l8900_nand_disk_erase_block(&s->nand->disk, banks[i], pages[i] / S5L8900_NAND_PAGES_PER_BLOCK);
        }
        adm_done(s);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "s5l8900.adm: command 0x%x\n", op);
        s->results[RESULT_STATUS] = STATUS_BAD_ARGUMENT;
        adm_done(s);
        break;
    }
}

static void adm_stream_done(void *opaque, int n, int level)
{
    S5L8900ADMState *s = S5L8900_ADM(opaque);

    if (level) {
        adm_done(s);
    }
}

static void adm_start(S5L8900ADMState *s)
{
    uint8_t post[2] = { ADM_POST_ONLINE, 0 };

    adm_mem(s, ADM_DATA_SECTION2, 0, post, sizeof(post), true);
}

static uint64_t adm_read_reg(void *opaque, hwaddr addr, unsigned size)
{
    S5L8900ADMState *s = S5L8900_ADM(opaque);

    switch (addr) {
    case ADM_CTRL:
        return (s->ctrl & ~(ADM_CTRL_STOPPED | ADM_CTRL_RESET)) | (s->ctrl & ADM_CTRL_RUN ? 0 : ADM_CTRL_STOPPED);
    case ADM_INT:
        return s->status;
    case ADM_RESULT ... ADM_RESULT + 4 * S5L8900_ADM_RESULTS - 1:
        return s->results[(addr - ADM_RESULT) / 4];
    default:
        if (addr < S5L8900_ADM_REGS) {
            return s->regs[addr / 4];
        }
        qemu_log_mask(LOG_UNIMP, "%s: 0x%" HWADDR_PRIx "\n", __func__, addr);
        return 0;
    }
}

static void adm_write_reg(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    S5L8900ADMState *s = S5L8900_ADM(opaque);

    switch (addr) {
    case ADM_CTRL: {
        bool start = (value & ADM_CTRL_RUN) && !(s->ctrl & ADM_CTRL_RUN);
        s->ctrl = value & ~ADM_CTRL_RESET;      // the reset is over at once
        if (start) {
            adm_start(s);
        }
        break;
    }
    case ADM_INT:
        if (value == ADM_INT_COMMAND) {
            adm_command(s);
        } else if (value < 32) {
            s->status &= ~(1u << value);
            adm_update_irq(s);
        }
        break;
    default:
        if (addr < S5L8900_ADM_REGS) {
            s->regs[addr / 4] = value;
        } else {
            qemu_log_mask(LOG_UNIMP, "%s: 0x%" HWADDR_PRIx " = 0x%" PRIx64 "\n", __func__, addr, value);
        }
        break;
    }
}

static const MemoryRegionOps adm_ops = {
    .read = adm_read_reg,
    .write = adm_write_reg,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void adm_init(Object *obj)
{
    S5L8900ADMState *s = S5L8900_ADM(obj);

    memory_region_init_io(&s->iomem, obj, &adm_ops, s, TYPE_S5L8900_ADM, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_in_named(DEVICE(obj), adm_stream_done, "stream-done", 1);
}

static void adm_realize(DeviceState *dev, Error **errp)
{
    S5L8900ADMState *s = S5L8900_ADM(dev);

    if (!s->nand) {
        error_setg(errp, "the ADM needs its NAND (the \"nand\" link)");
    }
}

static void adm_reset(DeviceState *dev)
{
    S5L8900ADMState *s = S5L8900_ADM(dev);

    s->ctrl = 0;
    s->status = 0;
    memset(s->results, 0, sizeof(s->results));
    memset(s->regs, 0, sizeof(s->regs));
}

static const Property adm_properties[] = {
    DEFINE_PROP_LINK("nand", S5L8900ADMState, nand, TYPE_S5L8900_NAND, S5L8900NANDState *),
};

static const VMStateDescription vmstate_s5l8900_adm = {
    .name = TYPE_S5L8900_ADM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, S5L8900ADMState),
        VMSTATE_UINT32(status, S5L8900ADMState),
        VMSTATE_UINT32_ARRAY(results, S5L8900ADMState, S5L8900_ADM_RESULTS),
        VMSTATE_UINT32_ARRAY(regs, S5L8900ADMState, S5L8900_ADM_REGS / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void adm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_s5l8900_adm;
    dc->realize = adm_realize;
    device_class_set_props(dc, adm_properties);
    device_class_set_legacy_reset(dc, adm_reset);
}

static const TypeInfo adm_info = {
    .name = TYPE_S5L8900_ADM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900ADMState),
    .instance_init = adm_init,
    .class_init = adm_class_init,
};

static void adm_register_types(void)
{
    type_register_static(&adm_info);
}

type_init(adm_register_types)
