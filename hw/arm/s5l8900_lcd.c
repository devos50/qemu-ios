/*
 * The S5L8900's LCD controller (CLCD, 0x38900000), as iBoot 1.1.5 drives it.
 *
 * iBoot sets up a window and reads its registers back to tell the kernel where the framebuffer is (the video boot
 * arguments: base, width and height, bytes per line, depth), so every register reads back what was written. iBoot
 * waits on nothing here. The screen shows the last window with a framebuffer and a size; which bits enable a window
 * isn't known. Formats 6-7 are 32 bits per pixel (BGRA in memory), 3-5 16 (RGB565); 8-bit and 4-bit windows aren't
 * drawn. No interrupt yet.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "ui/pixel_ops.h"
#include "hw/arm/s5l8900_lcd.h"

static uint32_t s5l8900_lcd_wnd(S5L8900LCDState *s, int n, hwaddr reg)
{
    return s->regs[(S5L8900_LCD_WINDOW(n) + reg) / 4];
}

static int s5l8900_lcd_bpp(uint32_t con)
{
    static const int bpp[8] = { 4, 8, 8, 16, 16, 16, 32, 32 };
    return bpp[(con >> 8) & 7];
}

static uint64_t s5l8900_lcd_read(void *opaque, hwaddr addr, unsigned size)
{
    S5L8900LCDState *s = opaque;
    return s->regs[addr / 4];
}

static void s5l8900_lcd_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    S5L8900LCDState *s = opaque;
    s->regs[addr / 4] = val;
    if (addr >= S5L8900_LCD_WINDOW(0) && addr < S5L8900_LCD_WINDOW(S5L8900_LCD_WINDOWS)) {
        s->invalidate = true;
    }
}

static const MemoryRegionOps s5l8900_lcd_ops = {
    .read = s5l8900_lcd_read,
    .write = s5l8900_lcd_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void s5l8900_lcd_draw_32(void *opaque, uint8_t *d, const uint8_t *src, int width, int deststep)
{
    for (int x = 0; x < width; x++, src += 4, d += 4) {
        *(uint32_t *)d = rgb_to_pixel32(src[2], src[1], src[0]);
    }
}

static void s5l8900_lcd_draw_16(void *opaque, uint8_t *d, const uint8_t *src, int width, int deststep)
{
    for (int x = 0; x < width; x++, src += 2, d += 4) {
        uint16_t v = lduw_le_p(src);
        *(uint32_t *)d = rgb_to_pixel32((v >> 8) & 0xf8, (v >> 3) & 0xfc, (v << 3) & 0xf8);
    }
}

static void s5l8900_lcd_invalidate(void *opaque)
{
    S5L8900LCDState *s = opaque;
    s->invalidate = true;
}

static bool s5l8900_lcd_update(void *opaque)
{
    S5L8900LCDState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    int n, width, height, hspan, bpp, first = 0, last = 0;
    drawfn draw;

    for (n = S5L8900_LCD_WINDOWS - 1; n >= 0; n--) {
        if (s5l8900_lcd_wnd(s, n, S5L8900_LCD_WND_BASE) && s5l8900_lcd_wnd(s, n, S5L8900_LCD_WND_SIZE)) {
            break;
        }
    }
    if (n < 0 || !surface_bits_per_pixel(surface)) {
        return true;
    }
    width = MIN(s5l8900_lcd_wnd(s, n, S5L8900_LCD_WND_SIZE) >> 16, S5L8900_LCD_WIDTH);
    height = MIN(s5l8900_lcd_wnd(s, n, S5L8900_LCD_WND_SIZE) & 0xffff, S5L8900_LCD_HEIGHT);
    hspan = s5l8900_lcd_wnd(s, n, S5L8900_LCD_WND_HSPAN);
    bpp = s5l8900_lcd_bpp(s5l8900_lcd_wnd(s, n, S5L8900_LCD_WND_CON));
    if (bpp == 32) {
        draw = s5l8900_lcd_draw_32;
    } else if (bpp == 16) {
        draw = s5l8900_lcd_draw_16;
    } else {
        if (s->invalidate) {
            qemu_log_mask(LOG_UNIMP, "%s: window %d has %d bits per pixel\n", __func__, n, bpp);
            s->invalidate = false;
        }
        return true;
    }
    if (!width || !height || hspan < width * bpp / 8) {
        return true;
    }

    if (s->invalidate || n != s->shown) {
        framebuffer_update_memory_section(&s->fbsection, s->sysmem, s5l8900_lcd_wnd(s, n, S5L8900_LCD_WND_BASE),
                                          height, hspan);
        s->shown = n;
    }
    framebuffer_update_display(surface, &s->fbsection, width, height, hspan, surface_stride(surface), 4,
                               s->invalidate, draw, s, &first, &last);
    if (first >= 0) {
        qemu_console_update(s->con, 0, first, width, last - first + 1);
    }
    s->invalidate = false;
    return true;
}

static const GraphicHwOps s5l8900_lcd_gfx_ops = {
    .invalidate = s5l8900_lcd_invalidate,
    .gfx_update = s5l8900_lcd_update,
};

static void s5l8900_lcd_reset(DeviceState *dev)
{
    S5L8900LCDState *s = S5L8900_LCD(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->shown = -1;
    s->invalidate = true;
}

static void s5l8900_lcd_init(Object *obj)
{
    S5L8900LCDState *s = S5L8900_LCD(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8900_lcd_ops, s, TYPE_S5L8900_LCD, S5L8900_LCD_REGS);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void s5l8900_lcd_realize(DeviceState *dev, Error **errp)
{
    S5L8900LCDState *s = S5L8900_LCD(dev);

    s->sysmem = get_system_memory();
    s->con = qemu_graphic_console_create(dev, 0, &s5l8900_lcd_gfx_ops, s);
    qemu_console_resize(s->con, S5L8900_LCD_WIDTH, S5L8900_LCD_HEIGHT);
}

static int s5l8900_lcd_post_load(void *opaque, int version_id)
{
    S5L8900LCDState *s = opaque;
    s->shown = -1;
    s->invalidate = true;
    return 0;
}

static const VMStateDescription vmstate_s5l8900_lcd = {
    .name = TYPE_S5L8900_LCD,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = s5l8900_lcd_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, S5L8900LCDState, S5L8900_LCD_REGS / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l8900_lcd_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = s5l8900_lcd_realize;
    device_class_set_legacy_reset(dc, s5l8900_lcd_reset);
    dc->vmsd = &vmstate_s5l8900_lcd;
}

static const TypeInfo s5l8900_lcd_info = {
    .name          = TYPE_S5L8900_LCD,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900LCDState),
    .instance_init = s5l8900_lcd_init,
    .class_init    = s5l8900_lcd_class_init,
};

static void s5l8900_lcd_register_types(void)
{
    type_register_static(&s5l8900_lcd_info);
}

type_init(s5l8900_lcd_register_types)
