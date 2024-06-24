#include "hw/arm/atv4_ausb.h"

static uint64_t atv4_ausb_read(void *opaque, hwaddr addr, unsigned size)
{
    ATV4AUSBState *s = (ATV4AUSBState *) opaque;

    switch(addr)
    {
        default:
            return 0x0;
    }
}

static void atv4_ausb_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    ATV4AUSBState *s = (ATV4AUSBState *) opaque;

    switch(addr)
    {
        default:
            return;
    }
}

static const MemoryRegionOps atv4_ausb_ops = {
    .read = atv4_ausb_read,
    .write = atv4_ausb_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void atv4_ausb_init(Object *obj)
{
    ATV4AUSBState *s = ATV4_AUSB(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &atv4_ausb_ops, s, TYPE_ATV4_AUSB, 0x100);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void atv4_ausb_class_init(ObjectClass *klass, void *data)
{
    
}

static const TypeInfo atv4_ausb_type_info = {
    .name = TYPE_ATV4_AUSB,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ATV4AUSBState),
    .instance_init = atv4_ausb_init,
    .class_init = atv4_ausb_class_init,
};

static void atv4_ausb_register_types(void)
{
    type_register_static(&atv4_ausb_type_info);
}

type_init(atv4_ausb_register_types)