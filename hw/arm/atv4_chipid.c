#include "hw/arm/atv4_chipid.h"

static uint64_t atv4_chipid_read(void *opaque, hwaddr addr, unsigned size)
{
    //fprintf(stderr, "%s: offset = 0x%08x\n", __func__, addr);

    switch (addr) {
        case rCFG_FUSE0:
            return 0;
        case CHIPID_REVISION:
            return 1;
        default:
            hw_error("%s: reading from unknown chip ID register 0x%08x\n", __func__, addr);
    }

    return 0;
}

static const MemoryRegionOps atv4_chipid_ops = {
    .read = atv4_chipid_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void atv4_chipid_init(Object *obj)
{
    ATV4ChipIDState *s = ATV4_CHIPID(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &atv4_chipid_ops, s, TYPE_ATV4_CHIPID, 0x14);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void atv4_chipid_class_init(ObjectClass *klass, void *data)
{
    
}

static const TypeInfo atv4_chipid_type_info = {
    .name = TYPE_ATV4_CHIPID,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ATV4ChipIDState),
    .instance_init = atv4_chipid_init,
    .class_init = atv4_chipid_class_init,
};

static void atv4_chipid_register_types(void)
{
    type_register_static(&atv4_chipid_type_info);
}

type_init(atv4_chipid_register_types)