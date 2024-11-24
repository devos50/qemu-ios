#include "hw/arm/atv4_anc.h"

static uint64_t atv4_anc_read(void *opaque, hwaddr addr, unsigned size)
{
    ATV4ANCState *s = (ATV4ANCState *) opaque;

    switch(addr)
    {
        default:
            return 0x0;
    }
}

static void atv4_anc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    ATV4ANCState *s = (ATV4ANCState *) opaque;

    switch(addr)
    {
        default:
            return;
    }
}

static const MemoryRegionOps atv4_anc_ops = {
    .read = atv4_anc_read,
    .write = atv4_anc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void atv4_anc_init(Object *obj)
{
    ATV4ANCState *s = ATV4_ANC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->ppnnpl_iomem, obj, &atv4_anc_ops, s, TYPE_ATV4_ANC, 0x100); // TODO hard-coded anc_ops call, should change
    sysbus_init_mmio(sbd, &s->ppnnpl_iomem);

    memory_region_init_io(&s->anc_iomem, obj, &atv4_anc_ops, s, TYPE_ATV4_ANC, 0x3000);
    sysbus_init_mmio(sbd, &s->anc_iomem);
}

static void atv4_anc_class_init(ObjectClass *klass, void *data)
{
    
}

static const TypeInfo atv4_anc_type_info = {
    .name = TYPE_ATV4_ANC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ATV4ANCState),
    .instance_init = atv4_anc_init,
    .class_init = atv4_anc_class_init,
};

static void atv4_anc_register_types(void)
{
    type_register_static(&atv4_anc_type_info);
}

type_init(atv4_anc_register_types)