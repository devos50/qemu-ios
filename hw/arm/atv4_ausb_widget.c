#include "hw/arm/atv4_ausb_widget.h"

static uint64_t atv4_ausb_widget_read(void *opaque, hwaddr addr, unsigned size)
{
    ATV4AUSBWidgetState *s = (ATV4AUSBWidgetState *) opaque;

    switch(addr)
    {
        default:
            return 0x0;
    }
}

static void atv4_ausb_widget_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    ATV4AUSBWidgetState *s = (ATV4AUSBWidgetState *) opaque;

    switch(addr)
    {
        default:
            return;
    }
}

static const MemoryRegionOps atv4_ausb_widget_ops = {
    .read = atv4_ausb_widget_read,
    .write = atv4_ausb_widget_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void atv4_ausb_widget_init(Object *obj)
{
    ATV4AUSBWidgetState *s = ATV4_AUSB_WIDGET(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &atv4_ausb_widget_ops, s, TYPE_ATV4_AUSB_WIDGET, 0x100);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void atv4_ausb_widget_class_init(ObjectClass *klass, void *data)
{
    
}

static const TypeInfo atv4_ausb_widget_type_info = {
    .name = TYPE_ATV4_AUSB_WIDGET,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ATV4AUSBWidgetState),
    .instance_init = atv4_ausb_widget_init,
    .class_init = atv4_ausb_widget_class_init,
};

static void atv4_ausb_widget_register_types(void)
{
    type_register_static(&atv4_ausb_widget_type_info);
}

type_init(atv4_ausb_widget_register_types)