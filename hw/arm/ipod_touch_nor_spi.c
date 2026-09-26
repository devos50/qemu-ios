#include "hw/arm/ipod_touch_nor_spi.h"
#include "qemu/log.h"
#include "qemu/error-report.h"

static const uint8_t nor_jedec_id[] = { 0x1F, 0x45, 0x02 }; // vendor: atmel, device: 0x02 -> AT25DF081A

IPodTouchNORImage *ipod_touch_nor_image_load(const char *path)
{
    IPodTouchNORImage *img = g_new0(IPodTouchNORImage, 1);
    gsize fsize;

    if (path && path[0] && g_file_get_contents(path, (char **)&img->data, &fsize, NULL) && fsize > 0) {
        img->path = g_strdup(path);
        img->size = fsize;
    } else {
        // without an image, writes are kept in memory only
        if (path && path[0]) {
            error_report("%s: unable to read NOR image %s", __func__, path);
        }
        g_free(img->data);
        img->size = NOR_DEFAULT_SIZE;
        img->data = g_malloc(img->size);
        memset(img->data, 0xFF, img->size);
    }
    return img;
}

static void nor_mark_dirty(IPodTouchNORSPIState *s, uint32_t start, uint32_t len)
{
    if (s->dirty_end == s->dirty_start) {
        s->dirty_start = start;
        s->dirty_end = start + len;
    } else {
        s->dirty_start = MIN(s->dirty_start, start);
        s->dirty_end = MAX(s->dirty_end, start + len);
    }
}

// Writes the range modified by the last command back to the image file.
static void nor_flush(IPodTouchNORSPIState *s)
{
    IPodTouchNORImage *img = s->image;
    uint32_t start = s->dirty_start, len = s->dirty_end - s->dirty_start;

    s->dirty_start = s->dirty_end = 0;
    if (len == 0 || !img->path) {
        return;
    }

    int fd = open(img->path, O_WRONLY);
    if (fd < 0) {
        error_report("%s: unable to open %s: %s", __func__, img->path, strerror(errno));
        return;
    }
    if (pwrite(fd, img->data + start, len, start) != len) {
        error_report("%s: unable to write %s: %s", __func__, img->path, strerror(errno));
    }
    close(fd);
}

static void nor_erase(IPodTouchNORSPIState *s, uint32_t block_size)
{
    IPodTouchNORImage *img = s->image;
    uint32_t start = (s->addr % img->size) & ~(block_size - 1);
    uint32_t len = MIN(block_size, img->size - start);

    if (!(s->status & NOR_STATUS_WEL)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: erase at 0x%x without write enable\n", __func__, s->addr);
        return;
    }
    memset(img->data + start, 0xFF, len);
    nor_mark_dirty(s, start, len);
}

static void nor_program(IPodTouchNORSPIState *s, uint8_t value)
{
    IPodTouchNORImage *img = s->image;
    uint32_t off = s->addr % img->size;

    if (!(s->status & NOR_STATUS_WEL)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: program at 0x%x without write enable\n", __func__, s->addr);
        return;
    }
    // programming can only clear bits
    img->data[off] &= value;
    nor_mark_dirty(s, off, 1);
    // the address wraps around within the 256-byte page
    s->addr = (s->addr & ~0xFF) | ((s->addr + 1) & 0xFF);
}

static void nor_start_command(IPodTouchNORSPIState *s, uint8_t cmd)
{
    s->cur_cmd = cmd;
    s->addr = 0;

    switch (cmd) {
    case NOR_ENABLE_WRITE:
        s->status |= NOR_STATUS_WEL;
        break;
    case NOR_DISABLE_WRITE:
        s->status &= ~NOR_STATUS_WEL;
        break;
    case NOR_ERASE_CHIP:
    case NOR_ERASE_CHIP_ALT:
        nor_erase(s, s->image->size);
        break;
    case NOR_GET_STATUS_CMD:
    case NOR_GET_JEDECID:
    case NOR_READ_DATA_CMD:
    case NOR_FAST_READ_CMD:
    case NOR_WRITE_DATA_CMD:
    case NOR_WRITE_TO_STATUS_REG:
    case NOR_ERASE_BLOCK:
    case NOR_ERASE_BLOCK_32K:
    case NOR_ERASE_BLOCK_64K:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unknown command 0x%02x\n", __func__, cmd);
        break;
    }
}

// Commands are framed by the chip select line, which the guest drives through a GPIO.
static int ipod_touch_nor_spi_set_cs(SSIPeripheral *dev, bool level)
{
    IPodTouchNORSPIState *s = IPOD_TOUCH_NOR_SPI(dev);

    if (level) {
        // deselected: program and erase commands complete and clear the write enable latch
        switch (s->cur_cmd) {
        case NOR_WRITE_DATA_CMD:
        case NOR_WRITE_TO_STATUS_REG:
        case NOR_ERASE_BLOCK:
        case NOR_ERASE_BLOCK_32K:
        case NOR_ERASE_BLOCK_64K:
        case NOR_ERASE_CHIP:
        case NOR_ERASE_CHIP_ALT:
            if (s->cmd_pos > 0) {
                s->status &= ~NOR_STATUS_WEL;
            }
            break;
        }
        nor_flush(s);
    }
    s->cur_cmd = 0;
    s->cmd_pos = 0;
    return 0;
}

static uint32_t ipod_touch_nor_spi_transfer(SSIPeripheral *dev, uint32_t value)
{
    IPodTouchNORSPIState *s = IPOD_TOUCH_NOR_SPI(dev);
    IPodTouchNORImage *img = s->image;
    uint32_t pos = s->cmd_pos++;
    uint8_t ret;

    if (pos == 0) {
        nor_start_command(s, value);
        return 0;
    }

    switch (s->cur_cmd) {
    case NOR_GET_STATUS_CMD:
        // the chip is never busy, and all sectors are unprotected
        return s->status;
    case NOR_GET_JEDECID:
        return pos <= sizeof(nor_jedec_id) ? nor_jedec_id[pos - 1] : 0;
    case NOR_READ_DATA_CMD:
    case NOR_FAST_READ_CMD:
        if (pos <= 3) {
            s->addr = (s->addr << 8) | (value & 0xFF);
            return 0;
        }
        if (s->cur_cmd == NOR_FAST_READ_CMD && pos == 4) {
            return 0; // dummy byte
        }
        ret = img->data[s->addr % img->size];
        s->addr++;
        return ret;
    case NOR_WRITE_DATA_CMD:
        if (pos <= 3) {
            s->addr = (s->addr << 8) | (value & 0xFF);
        } else {
            nor_program(s, value);
        }
        return 0;
    case NOR_ERASE_BLOCK:
    case NOR_ERASE_BLOCK_32K:
    case NOR_ERASE_BLOCK_64K:
        if (pos <= 3) {
            s->addr = (s->addr << 8) | (value & 0xFF);
        }
        if (pos == 3) {
            nor_erase(s, s->cur_cmd == NOR_ERASE_BLOCK ? 4 * 1024 :
                         s->cur_cmd == NOR_ERASE_BLOCK_32K ? 32 * 1024 : 64 * 1024);
        }
        return 0;
    default:
        return 0;
    }
}

static void ipod_touch_nor_spi_realize(SSIPeripheral *d, Error **errp)
{
    IPodTouchNORSPIState *s = IPOD_TOUCH_NOR_SPI(d);

    // deselected until the guest drives the chip select GPIO low
    d->cs = true;
    s->cur_cmd = 0;
    s->cmd_pos = 0;
    s->status = 0;
}

static void ipod_touch_nor_spi_class_init(ObjectClass *klass, void *data)
{
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);
    k->realize = ipod_touch_nor_spi_realize;
    k->transfer = ipod_touch_nor_spi_transfer;
    k->set_cs = ipod_touch_nor_spi_set_cs;
    k->cs_polarity = SSI_CS_LOW;
}

static const TypeInfo ipod_touch_nor_spi_type_info = {
    .name = TYPE_IPOD_TOUCH_NOR_SPI,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(IPodTouchNORSPIState),
    .class_init = ipod_touch_nor_spi_class_init,
};

static void ipod_touch_nor_spi_register_types(void)
{
    type_register_static(&ipod_touch_nor_spi_type_info);
}

type_init(ipod_touch_nor_spi_register_types)
