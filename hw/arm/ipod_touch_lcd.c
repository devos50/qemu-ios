#include "hw/arm/ipod_touch_lcd.h"
#include "ui/pixel_ops.h"
#include "ui/console.h"
#include "hw/display/framebuffer.h"
#include "migration/vmstate.h"

int lcd_brightness = 255;

static uint64_t ipod_touch_lcd_read(void *opaque, hwaddr addr, unsigned size)
{
    // printf("%s: read from location 0x%08x\n", __func__, addr);

    IPodTouchLCDState *s = (IPodTouchLCDState *)opaque;
    switch(addr)
    {
        case 0x0:
            return 2;
        case 0x4:
            return s->lcd_con;
        case LCD_REG_INT_ENABLE:
            return s->int_enable;
        case LCD_REG_INT_STATUS:
            return s->int_status;
        case 0x20:
            return s->w1_display_depth_info;
        case 0x24:
            return s->w1_framebuffer_base;
        case 0x28:
            return s->w1_hspan;
        case 0x30:
            return s->w1_display_resolution_info;
        case 0x1b10:
            return 2;
	case 0x1b14:
	    return 0x3;
        default:
            //printf("%s: read invalid location 0x%08x.\n", __func__, addr);
            break;
    }
    return 0;
}

static void ipod_touch_lcd_update_irq(IPodTouchLCDState *s)
{
    qemu_set_irq(s->irq, !!(s->int_status & s->int_enable));
}

static void ipod_touch_lcd_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IPodTouchLCDState *s = (IPodTouchLCDState *)opaque;
    // printf("%s: writing 0x%08x to 0x%08x\n", __func__, val, addr);

    switch(addr) {
        case 0x4:
            s->lcd_con = val;
            break;
        case LCD_REG_INT_ENABLE:
            s->int_enable = val;
            ipod_touch_lcd_update_irq(s);
            break;
        case LCD_REG_INT_STATUS:
            s->int_status &= ~val;
            ipod_touch_lcd_update_irq(s);
            break;
        case 0x20:
            s->w1_display_depth_info = val;
            break;
        case 0x24:
            s->w1_framebuffer_base = val;
            break;
        case 0x28:
            s->w1_hspan = val;
            break;
        case 0x30:
            s->w1_display_resolution_info = val;
            break;
    }
}

void lcd_changebrightness(int brightness)
{
    lcd_brightness = brightness & 0xFF;
}

static void lcd_invalidate(void *opaque)
{
    IPodTouchLCDState *s = opaque;
    s->invalidate = 1;
}

static void draw_line32_32(void *opaque, uint8_t *d, const uint8_t *s, int width, int deststep)
{
    // IPodTouchLCDState *lcd = (IPodTouchLCDState *) opaque;
    uint8_t r, g, b;

    do {
        //v = lduw_le_p((void *) s);
        //printf("V: %d\n", *s);
        b = s[0];
        g = s[1];
        r = s[2];
        // printf("R: %d, G: %d, B: %d, A: %d\n", r, g, b, lcd_brightness);
        ((uint32_t *) d)[0] = rgb_to_pixel32(round((float)r * ((float)lcd_brightness / 255.0f)), round((float)g * ((float)lcd_brightness / 255.0f)), round((float)b * ((float)lcd_brightness / 255.0f)));
        s += 4;
        d += 4;
    } while (-- width != 0);
}

static bool lcd_refresh(void *opaque)
{
    // printf("%s: refreshing LCD screen\n", __func__);

    IPodTouchLCDState *lcd = (IPodTouchLCDState *) opaque;
    DisplaySurface *surface = qemu_console_surface(lcd->con);
    drawfn draw_line;
    int src_width, dest_width;
    int height, first, last;
    int width, linesize;

    if (!lcd || !lcd->con || !surface_bits_per_pixel(surface))
        return true;

    dest_width = 4;
    draw_line = draw_line32_32;

    /* Resolution */
    first = last = 0;
    width = 320;
    height = 480;
    lcd->invalidate = 1;

    src_width =  4 * width;
    linesize = surface_stride(surface);

    if(lcd->invalidate) {
        framebuffer_update_memory_section(&lcd->fbsection, lcd->sysmem, lcd->w1_framebuffer_base, height, 4 * width);
    }

    framebuffer_update_display(surface, &lcd->fbsection,
                               width, height,
                               src_width,       /* Length of source line, in bytes.  */
                               linesize,        /* Bytes between adjacent horizontal output pixels.  */
                               dest_width,      /* Bytes between adjacent vertical output pixels.  */
                               lcd->invalidate,
                               draw_line, NULL,
                               &first, &last);
    if (first >= 0) {
        qemu_console_update(lcd->con, 0, first, width, last - first + 1);
    }
    lcd->invalidate = 0;
    return true;
}

static const MemoryRegionOps lcd_ops = {
    .read = ipod_touch_lcd_read,
    .write = ipod_touch_lcd_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const GraphicHwOps gfx_ops = {
    .invalidate  = lcd_invalidate,
    .gfx_update  = lcd_refresh,
};

static void ipod_touch_lcd_mouse_event(void *opaque, int x, int y, int z, int buttons_state)
{
    // printf("x %d y %d z %d state %d\n", x, y, z, buttons_state);

    // convert x and y to fractional numbers. The multitouch Y axis points up.
    float fx = (float)x / INPUT_EVENT_ABS_MAX;
    float fy = 1 - (float)y / INPUT_EVENT_ABS_MAX;

    IPodTouchLCDState *lcd = (IPodTouchLCDState *) opaque;
    lcd->mt->touch_x = fx;
    lcd->mt->touch_y = fy;

    if(buttons_state && !lcd->mt->touch_down) {
        ipod_touch_multitouch_on_touch(lcd->mt);
    }
    else if(!buttons_state && lcd->mt->touch_down) {
        ipod_touch_multitouch_on_release(lcd->mt);
    }
}

static void ipod_touch_lcd_touch_input_event(DeviceState *dev, QemuConsole *src,
                                             QemuInputEvent *evt)
{
    IPodTouchLCDState *s = IPOD_TOUCH_LCD(dev);

    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS:
        if (evt->abs.axis < INPUT_AXIS__MAX) {
            s->touch_axis[evt->abs.axis] = evt->abs.value;
        }
        break;
    case INPUT_EVENT_KIND_BTN:
        if (evt->btn.button == INPUT_BUTTON_LEFT) {
            s->touch_button = evt->btn.down;
        }
        break;
    default:
        break;
    }
}

static void ipod_touch_lcd_touch_input_sync(DeviceState *dev)
{
    IPodTouchLCDState *s = IPOD_TOUCH_LCD(dev);

    ipod_touch_lcd_mouse_event(s, s->touch_axis[INPUT_AXIS_X], s->touch_axis[INPUT_AXIS_Y], 0, s->touch_button);
}

static const QemuInputHandler ipod_touch_lcd_touch_handler = {
    .name  = "iPod Touch Touchscreen",
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = ipod_touch_lcd_touch_input_event,
    .sync  = ipod_touch_lcd_touch_input_sync,
};

static void refresh_timer_tick(void *opaque)
{
    IPodTouchLCDState *s = (IPodTouchLCDState *)opaque;

    // The kernel only enables the vsync interrupt while it has a swap or vsync callbacks pending
    s->int_status |= LCD_INT_VSYNC;
    ipod_touch_lcd_update_irq(s);

    timer_mod(s->refresh_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / 60);//LCD_REFRESH_RATE_FREQUENCY);
}

static void ipod_touch_lcd_realize(DeviceState *dev, Error **errp)
{
    IPodTouchLCDState *s = IPOD_TOUCH_LCD(dev);
    s->con = qemu_graphic_console_create(dev, 0, &gfx_ops, s);
    qemu_console_resize(s->con, 320, 480);

    // the touchscreen takes absolute pointer events
    s->touch_handler = qemu_input_handler_register(dev, &ipod_touch_lcd_touch_handler);
    qemu_input_handler_activate(s->touch_handler);

    // initialize the refresh timer
    s->refresh_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, refresh_timer_tick, s);
    timer_mod(s->refresh_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / LCD_REFRESH_RATE_FREQUENCY);
}

static void ipod_touch_lcd_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(sbd);
    IPodTouchLCDState *s = IPOD_TOUCH_LCD(dev);

    memory_region_init_io(&s->iomem, obj, &lcd_ops, s, "lcd", 0x10000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static int ipod_touch_lcd_post_load(void *opaque, int version_id)
{
    lcd_invalidate(opaque);
    return 0;
}

// The brightness comes from the PMU. The touch axes and button are the host pointer's.
static const VMStateDescription vmstate_ipod_touch_lcd = {
    .name = TYPE_IPOD_TOUCH_LCD,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = ipod_touch_lcd_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(lcd_con, IPodTouchLCDState),
        VMSTATE_UINT32(w1_display_resolution_info, IPodTouchLCDState),
        VMSTATE_UINT32(w1_framebuffer_base, IPodTouchLCDState),
        VMSTATE_UINT32(w1_hspan, IPodTouchLCDState),
        VMSTATE_UINT32(w1_display_depth_info, IPodTouchLCDState),
        VMSTATE_UINT32(int_enable, IPodTouchLCDState),
        VMSTATE_UINT32(int_status, IPodTouchLCDState),
        VMSTATE_TIMER_PTR(refresh_timer, IPodTouchLCDState),
        VMSTATE_END_OF_LIST()
    }
};

static void ipod_touch_lcd_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ipod_touch_lcd_realize;
    dc->vmsd = &vmstate_ipod_touch_lcd;
}

static const TypeInfo ipod_touch_lcd_info = {
    .name          = TYPE_IPOD_TOUCH_LCD,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchLCDState),
    .instance_init = ipod_touch_lcd_init,
    .class_init    = ipod_touch_lcd_class_init,
};

static void ipod_touch_machine_types(void)
{
    type_register_static(&ipod_touch_lcd_info);
}

type_init(ipod_touch_machine_types)
