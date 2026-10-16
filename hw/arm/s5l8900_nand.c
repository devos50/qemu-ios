/*
 * The S5L8900's NAND flash controller (FMC) and the NAND behind it, and its ECC engine.
 *
 * The guest selects a bank in FMCTRL0, writes a command to CMD, the address cycles to FMADDR0/1 (FMANUM + 1 of them,
 * sent by writing 1 to FMCTRL1), and moves FMDNUM + 1 bytes through the FIFO by writing 2 to FMCTRL1, by reading the
 * FIFO itself or, in DMA mode (FMCTRL0 bit 10), through DMAC0. FMCSTAT reports the steps as they finish: bit 1 when
 * the controller can take a command, bit 2 when the address has gone out, bit 3 when a transfer has, bit 4 + n when
 * bank n is ready again; the last three are cleared by writing them.
 *
 * The chips are 8 banks of Hynix NAND, as in the 8 GB iPod touch. Their contents come from a disk image with an
 * overlay (the "disk" and "overlay" properties, s5l8900_nand_disk.c); without one every page reads as erased. The
 * commands that would change the NAND through these registers aren't modelled (iBoot only reads), and are logged
 * with -d unimp. The kernel doesn't use them either: its driver hands commands to the ADM (s5l8900_adm.c), which
 * moves the pages through the FIFO as a stream (see s5l8900_nand.h) and programs and erases the overlay.
 *
 * The ECC engine checks what the FIL read, 512-byte sectors against their ECC bytes, and interrupts when it is done.
 * It doesn't compute ECC: it reports an uncorrectable error only where the sectors and their ECC bytes are all 0xff,
 * as the real engine does for an erased page, which is how the FIL tells erased pages from written ones.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "hw/core/irq.h"
#include "hw/arm/s5l8900_nand.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "system/dma.h"

#define NAND_FMCTRL0        0x00
#define NAND_FMCTRL0_BANKS(v)   (((v) >> 1) & 0xff)
#define NAND_FMCTRL1        0x04
#define NAND_FMCTRL1_ADDRESS    0x1     // send the address cycles
#define NAND_FMCTRL1_READ       0x2     // move FMDNUM + 1 bytes from the chip into the FIFO
#define NAND_CMD            0x08
#define NAND_FMADDR0        0x0c
#define NAND_FMADDR1        0x10
#define NAND_FMANUM         0x2c
#define NAND_FMDNUM         0x30
#define NAND_FMCSTAT        0x48
#define NAND_FMCSTAT_CMD_IDLE   (1 << 1)
#define NAND_FMCSTAT_ADDRESS    (1 << 2)
#define NAND_FMCSTAT_TRANSFER   (1 << 3)
#define NAND_FMCSTAT_BANK(n)    (1 << (4 + (n)))
#define NAND_FMFIFO         0x80
#define NAND_RSCTRL         0x100

#define NAND_CMD_READ           0x00
#define NAND_CMD_READ_CONFIRM   0x30
#define NAND_CMD_READ_ID        0x90
#define NAND_CMD_READ_STATUS    0x70
#define NAND_CMD_RESET          0xff

#define NAND_STATUS_READY       0xe0    // ready, not write protected

#define ECC_DATA            0x04
#define ECC_ECC             0x08
#define ECC_START           0x0c
#define ECC_STATUS          0x10
#define ECC_STATUS_UNCORRECTABLE    1
#define ECC_SETUP           0x14
#define ECC_CLEARINT        0x40

static int nand_bank(S5L8900NANDState *s)
{
    uint32_t banks = NAND_FMCTRL0_BANKS(s->fmctrl0);

    return banks ? ctz32(banks) : -1;
}

/* Loads a page into the selected chip's page register */
static void nand_load_page(S5L8900NANDState *s, int bank, uint32_t row)
{
    if (row >= S5L8900_NAND_PAGES_PER_BANK) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: page %u of bank %d is beyond the chip\n", __func__, row, bank);
    }
    s5l8900_nand_disk_read_page(&s->disk, bank, row, s->page_register,
                                s->page_register + S5L8900_NAND_BYTES_PER_PAGE);
}

static void nand_command(S5L8900NANDState *s, uint32_t cmd)
{
    int bank = nand_bank(s);

    switch (cmd) {
    case NAND_CMD_READ:
        // a read, or back to the page data after the status
        s->output = NAND_OUT_DATA;
        break;
    case NAND_CMD_READ_CONFIRM:
        if (bank >= 0) {
            nand_load_page(s, bank, s->row);
            s->fmcstat |= NAND_FMCSTAT_BANK(bank);
        }
        s->output = NAND_OUT_DATA;
        break;
    case NAND_CMD_READ_ID:
        s->output = NAND_OUT_ID;
        break;
    case NAND_CMD_READ_STATUS:
        s->output = NAND_OUT_STATUS;
        break;
    case NAND_CMD_RESET:
        if (bank >= 0) {
            s->fmcstat |= NAND_FMCSTAT_BANK(bank);
        }
        s->output = NAND_OUT_DATA;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: command 0x%02x\n", __func__, cmd);
        break;
    }
}

static void nand_address(S5L8900NANDState *s)
{
    // the column in two cycles, then the page
    s->column = s->fmaddr0 & 0xffff;
    s->row = (s->fmaddr0 >> 16) | ((s->fmaddr1 & 0xff) << 16);
    s->fmcstat |= NAND_FMCSTAT_ADDRESS;
}

static void nand_transfer(S5L8900NANDState *s)
{
    static const uint8_t id[] = {
        S5L8900_NAND_CHIP_ID & 0xff, (S5L8900_NAND_CHIP_ID >> 8) & 0xff, (S5L8900_NAND_CHIP_ID >> 16) & 0xff,
        S5L8900_NAND_CHIP_ID >> 24,
    };
    uint32_t len = MIN(s->fmdnum + 1, sizeof(s->fifo));

    for (uint32_t i = 0; i < len; i++) {
        switch (s->output) {
        case NAND_OUT_ID:
            s->fifo[i] = i < sizeof(id) ? id[i] : 0;
            break;
        case NAND_OUT_STATUS:
            s->fifo[i] = NAND_STATUS_READY;
            break;
        default:
            s->fifo[i] = s->column < sizeof(s->page_register) ? s->page_register[s->column] : 0xff;
            s->column++;
            break;
        }
    }
    s->fifo_len = len;
    s->fifo_pos = 0;
    s->fmcstat |= NAND_FMCSTAT_TRANSFER;
}

void s5l8900_nand_stream_read(S5L8900NANDState *s, uint32_t count, const uint8_t *banks, const uint32_t *pages)
{
    assert(count <= S5L8900_NAND_STREAM_PAGES);
    s->stream = NAND_STREAM_READ;
    s->stream_count = count;
    s->stream_next = 0;
    memcpy(s->stream_banks, banks, count);
    memcpy(s->stream_pages, pages, count * sizeof(*pages));
    s->fifo_len = s->fifo_pos = 0;
}

void s5l8900_nand_stream_write(S5L8900NANDState *s, uint32_t count, const uint8_t *banks, const uint32_t *pages,
                               uint32_t stride, const uint8_t *meta, uint32_t meta_len)
{
    assert(count <= S5L8900_NAND_STREAM_PAGES && meta_len <= S5L8900_NAND_STREAM_META);
    assert(stride >= S5L8900_NAND_BYTES_PER_PAGE && stride <= sizeof(s->fifo));
    s->stream = NAND_STREAM_WRITE;
    s->stream_count = count;
    s->stream_next = 0;
    s->stream_stride = stride;
    s->stream_meta_len = meta_len;
    memcpy(s->stream_banks, banks, count);
    memcpy(s->stream_pages, pages, count * sizeof(*pages));
    memcpy(s->stream_meta, meta, count * meta_len);
    s->fifo_len = s->fifo_pos = 0;
    if (!count) {
        s->stream = NAND_STREAM_NONE;
        qemu_irq_pulse(s->stream_done);
    }
}

// the next page of a read stream into the FIFO
static void nand_stream_refill(S5L8900NANDState *s)
{
    uint8_t spare[S5L8900_NAND_BYTES_PER_SPARE];
    uint32_t n = s->stream_next++;

    s5l8900_nand_disk_read_page(&s->disk, s->stream_banks[n], s->stream_pages[n], s->fifo, spare);
    s->fifo_len = S5L8900_NAND_BYTES_PER_PAGE;
    s->fifo_pos = 0;
    if (s->stream_next == s->stream_count) {
        s->stream = NAND_STREAM_NONE;
    }
}

// a word into a write stream; the page goes to the NAND, with the meta data in its spare, once it is complete
static void nand_stream_push(S5L8900NANDState *s, uint32_t value, unsigned size)
{
    uint8_t spare[S5L8900_NAND_BYTES_PER_SPARE];
    uint32_t n = s->stream_next;

    for (unsigned i = 0; i < size && s->fifo_len < s->stream_stride; i++) {
        s->fifo[s->fifo_len++] = value >> (8 * i);
    }
    if (s->fifo_len < s->stream_stride) {
        return;
    }
    // as the formatter lays out a spare: 12 bytes of meta data (unused ones erased), then a zero ECC
    memset(spare, 0xff, 12);
    memcpy(spare, s->stream_meta + n * s->stream_meta_len, s->stream_meta_len);
    memset(spare + 12, 0, sizeof(spare) - 12);
    s5l8900_nand_disk_program_page(&s->disk, s->stream_banks[n], s->stream_pages[n], s->fifo, spare);
    s->fifo_len = 0;
    if (++s->stream_next == s->stream_count) {
        s->stream = NAND_STREAM_NONE;
        qemu_irq_pulse(s->stream_done);
    }
}

static uint32_t nand_fifo_pop(S5L8900NANDState *s)
{
    uint8_t word[4];

    if (s->fifo_pos >= s->fifo_len && s->stream == NAND_STREAM_READ) {
        nand_stream_refill(s);
    }

    for (int i = 0; i < sizeof(word); i++) {
        word[i] = s->fifo_pos < s->fifo_len ? s->fifo[s->fifo_pos++] : 0xff;
    }
    return ldl_le_p(word);
}

static uint64_t nand_read(void *opaque, hwaddr addr, unsigned size)
{
    S5L8900NANDState *s = S5L8900_NAND(opaque);

    switch (addr) {
    case NAND_FMCTRL0:
        return s->fmctrl0;
    case NAND_FMCTRL1:
        return s->fmctrl1;
    case NAND_CMD:
        return s->cmd;
    case NAND_FMADDR0:
        return s->fmaddr0;
    case NAND_FMADDR1:
        return s->fmaddr1;
    case NAND_FMANUM:
        return s->fmanum;
    case NAND_FMDNUM:
        return s->fmdnum;
    case NAND_FMCSTAT:
        return s->fmcstat | NAND_FMCSTAT_CMD_IDLE;
    case NAND_FMFIFO:
        return nand_fifo_pop(s);
    case NAND_RSCTRL:
        return s->rsctrl;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: 0x%" HWADDR_PRIx "\n", __func__, addr);
        return 0;
    }
}

static void nand_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    S5L8900NANDState *s = S5L8900_NAND(opaque);

    switch (addr) {
    case NAND_FMCTRL0:
        s->fmctrl0 = value;
        break;
    case NAND_FMCTRL1:
        s->fmctrl1 = value;
        if (value & NAND_FMCTRL1_ADDRESS) {
            nand_address(s);
        }
        if (value & NAND_FMCTRL1_READ) {
            nand_transfer(s);
        }
        break;
    case NAND_CMD:
        s->cmd = value;
        nand_command(s, value & 0xff);
        break;
    case NAND_FMADDR0:
        s->fmaddr0 = value;
        break;
    case NAND_FMADDR1:
        s->fmaddr1 = value;
        break;
    case NAND_FMANUM:
        s->fmanum = value;
        break;
    case NAND_FMDNUM:
        s->fmdnum = value;
        break;
    case NAND_FMCSTAT:
        s->fmcstat &= ~value;
        break;
    case NAND_FMFIFO:
        if (s->stream == NAND_STREAM_WRITE) {
            nand_stream_push(s, value, size);
        } else {
            qemu_log_mask(LOG_UNIMP, "%s: writing to the NAND\n", __func__);
        }
        break;
    case NAND_RSCTRL:
        s->rsctrl = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: 0x%" HWADDR_PRIx " = 0x%" PRIx64 "\n", __func__, addr, value);
        break;
    }
}

static const MemoryRegionOps nand_ops = {
    .read = nand_read,
    .write = nand_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void nand_init(Object *obj)
{
    S5L8900NANDState *s = S5L8900_NAND(obj);

    memory_region_init_io(&s->iomem, obj, &nand_ops, s, TYPE_S5L8900_NAND, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    qdev_init_gpio_out_named(DEVICE(obj), &s->stream_done, "stream-done", 1);
    s->disk.disk_fd = -1;
}

static void nand_realize(DeviceState *dev, Error **errp)
{
    S5L8900NANDState *s = S5L8900_NAND(dev);

    if (!s->disk_path != !s->overlay_path) {
        error_setg(errp, "the NAND's disk image and overlay go together");
        return;
    }
    if (s->disk_path) {
        s5l8900_nand_disk_open(&s->disk, s->disk_path, s->overlay_path, errp);
    }
}

static const Property nand_properties[] = {
    DEFINE_PROP_STRING("disk", S5L8900NANDState, disk_path),
    DEFINE_PROP_STRING("overlay", S5L8900NANDState, overlay_path),
};

static void nand_reset(DeviceState *dev)
{
    S5L8900NANDState *s = S5L8900_NAND(dev);

    s->fmctrl0 = s->fmctrl1 = s->cmd = 0;
    s->fmaddr0 = s->fmaddr1 = s->fmanum = s->fmdnum = 0;
    s->fmcstat = 0;
    s->rsctrl = 0;
    s->output = NAND_OUT_DATA;
    s->column = s->row = 0;
    memset(s->page_register, 0xff, sizeof(s->page_register));
    s->fifo_len = s->fifo_pos = 0;
    s->stream = NAND_STREAM_NONE;
    s->stream_count = s->stream_next = 0;
}

static const VMStateDescription vmstate_s5l8900_nand = {
    .name = TYPE_S5L8900_NAND,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(fmctrl0, S5L8900NANDState),
        VMSTATE_UINT32(fmctrl1, S5L8900NANDState),
        VMSTATE_UINT32(cmd, S5L8900NANDState),
        VMSTATE_UINT32(fmaddr0, S5L8900NANDState),
        VMSTATE_UINT32(fmaddr1, S5L8900NANDState),
        VMSTATE_UINT32(fmanum, S5L8900NANDState),
        VMSTATE_UINT32(fmdnum, S5L8900NANDState),
        VMSTATE_UINT32(fmcstat, S5L8900NANDState),
        VMSTATE_UINT32(rsctrl, S5L8900NANDState),
        VMSTATE_UINT32(output, S5L8900NANDState),
        VMSTATE_UINT32(column, S5L8900NANDState),
        VMSTATE_UINT32(row, S5L8900NANDState),
        VMSTATE_UINT8_ARRAY(page_register, S5L8900NANDState, S5L8900_NAND_PAGE_REGISTER),
        VMSTATE_UINT8_ARRAY(fifo, S5L8900NANDState, S5L8900_NAND_PAGE_REGISTER),
        VMSTATE_UINT32(fifo_len, S5L8900NANDState),
        VMSTATE_UINT32(fifo_pos, S5L8900NANDState),
        VMSTATE_UINT32(stream, S5L8900NANDState),
        VMSTATE_UINT32(stream_count, S5L8900NANDState),
        VMSTATE_UINT32(stream_next, S5L8900NANDState),
        VMSTATE_UINT32(stream_stride, S5L8900NANDState),
        VMSTATE_UINT32(stream_meta_len, S5L8900NANDState),
        VMSTATE_UINT8_ARRAY(stream_banks, S5L8900NANDState, S5L8900_NAND_STREAM_PAGES),
        VMSTATE_UINT32_ARRAY(stream_pages, S5L8900NANDState, S5L8900_NAND_STREAM_PAGES),
        VMSTATE_UINT8_ARRAY(stream_meta, S5L8900NANDState, S5L8900_NAND_STREAM_PAGES * S5L8900_NAND_STREAM_META),
        VMSTATE_END_OF_LIST()
    }
};

static void nand_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_s5l8900_nand;
    dc->realize = nand_realize;
    device_class_set_props(dc, nand_properties);
    device_class_set_legacy_reset(dc, nand_reset);
}

/* The ECC bytes per 512-byte sector for the engine's three modes (SETUP bits 2-3) */
static uint32_t ecc_bytes_per_sector(uint32_t setup)
{
    switch (setup & 0xc) {
    case 0x4:
        return 15;
    case 0x8:
        return 20;
    default:
        return 10;
    }
}

static bool ecc_all_erased(hwaddr addr, uint32_t len)
{
    g_autofree uint8_t *buf = g_malloc(len);

    if (dma_memory_read(&address_space_memory, addr, buf, len, MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return false;
    }
    for (uint32_t i = 0; i < len; i++) {
        if (buf[i] != 0xff) {
            return false;
        }
    }
    return true;
}

static void ecc_start(S5L8900NANDECCState *s)
{
    uint32_t sectors = (s->setup & 3) + 1;

    if (ecc_all_erased(s->data_addr, sectors * 512) &&
        ecc_all_erased(s->ecc_addr, sectors * ecc_bytes_per_sector(s->setup))) {
        s->status = ECC_STATUS_UNCORRECTABLE;
    } else {
        s->status = 0;
    }
    qemu_irq_raise(s->irq);
}

static uint64_t ecc_read(void *opaque, hwaddr addr, unsigned size)
{
    S5L8900NANDECCState *s = S5L8900_NAND_ECC(opaque);

    switch (addr) {
    case ECC_DATA:
        return s->data_addr;
    case ECC_ECC:
        return s->ecc_addr;
    case ECC_STATUS:
        return s->status;
    case ECC_SETUP:
        return s->setup;
    default:
        return 0;
    }
}

static void ecc_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    S5L8900NANDECCState *s = S5L8900_NAND_ECC(opaque);

    switch (addr) {
    case ECC_DATA:
        s->data_addr = value;
        break;
    case ECC_ECC:
        s->ecc_addr = value;
        break;
    case ECC_START:
        ecc_start(s);
        break;
    case ECC_SETUP:
        s->setup = value;
        break;
    case ECC_CLEARINT:
        qemu_irq_lower(s->irq);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: 0x%" HWADDR_PRIx " = 0x%" PRIx64 "\n", __func__, addr, value);
        break;
    }
}

static const MemoryRegionOps ecc_ops = {
    .read = ecc_read,
    .write = ecc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void ecc_init(Object *obj)
{
    S5L8900NANDECCState *s = S5L8900_NAND_ECC(obj);

    memory_region_init_io(&s->iomem, obj, &ecc_ops, s, TYPE_S5L8900_NAND_ECC, 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void ecc_reset(DeviceState *dev)
{
    S5L8900NANDECCState *s = S5L8900_NAND_ECC(dev);

    s->data_addr = s->ecc_addr = s->setup = s->status = 0;
    qemu_irq_lower(s->irq);
}

static const VMStateDescription vmstate_s5l8900_nand_ecc = {
    .name = TYPE_S5L8900_NAND_ECC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(data_addr, S5L8900NANDECCState),
        VMSTATE_UINT32(ecc_addr, S5L8900NANDECCState),
        VMSTATE_UINT32(setup, S5L8900NANDECCState),
        VMSTATE_UINT32(status, S5L8900NANDECCState),
        VMSTATE_END_OF_LIST()
    }
};

static void ecc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_s5l8900_nand_ecc;
    device_class_set_legacy_reset(dc, ecc_reset);
}

static const TypeInfo s5l8900_nand_types[] = {
    {
        .name          = TYPE_S5L8900_NAND,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(S5L8900NANDState),
        .instance_init = nand_init,
        .class_init    = nand_class_init,
    }, {
        .name          = TYPE_S5L8900_NAND_ECC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(S5L8900NANDECCState),
        .instance_init = ecc_init,
        .class_init    = ecc_class_init,
    },
};

DEFINE_TYPES(s5l8900_nand_types)
