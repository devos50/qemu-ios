#include "hw/arm/s5l_chipid.h"
#include "hw/core/qdev-properties.h"

static uint64_t s5l_chipid_read(void *opaque, hwaddr addr, unsigned size)
{
    S5LChipIDState *s = opaque;

    if (s->chip == 0x8900) {
        // the S5L8900 only has its revision, at the top of 0x4 (from the ipod_touch_1g branch)
        return addr == CHIPID_UNKNOWN1 ? S5L8900_CHIP_REVISION << 24 : 0;
    }

    switch (addr) {
        case CHIPID_UNKNOWN1:
            // Bit 5 is the "raw production mode" fuse the boot chain reads (bootrom
            // get_raw_production_mode, and the same check in iBSS/iBEC/LLB/iBoot). When set, every
            // loader requires the images it boots to carry a production ticket (PROD=1). iPhone OS
            // 2.x/3.0 images are PROD=1, but the iOS 4.0 images are PROD=0, so reporting production
            // mode rejects them at the first stage. Report non-production, like the exploitable
            // iPod2,1 units, so the genuinely Apple-signed 4.0 images load (their RSA signatures are
            // still verified); this leaves the PROD=1 images unaffected.
            return 0;
        case CHIPID_INFO:
            return (0x8720 << 16) | (1 << 2) | 1; // ind16 = chipid, ind2 = security domain, ind0 = 24 MHz crystal (12 MHz when clear)
        case CHIPID_UNKNOWN2:
            return 0;
        case CHIPID_UNKNOWN3:
            return 0;
        default:
            hw_error("%s: reading from unknown chip ID register 0x%08x\n", __func__, addr);
    }

    return 0;
}

static const MemoryRegionOps s5l_chipid_ops = {
    .read = s5l_chipid_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void s5l_chipid_init(Object *obj)
{
    S5LChipIDState *s = S5L_CHIPID(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &s5l_chipid_ops, s, TYPE_S5L_CHIPID, 0x14);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const Property s5l_chipid_properties[] = {
    DEFINE_PROP_UINT32("chip", S5LChipIDState, chip, 0x8720),
};

static void s5l_chipid_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_props(DEVICE_CLASS(klass), s5l_chipid_properties);
}

static const TypeInfo s5l_chipid_type_info = {
    .name = TYPE_S5L_CHIPID,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5LChipIDState),
    .instance_init = s5l_chipid_init,
    .class_init = s5l_chipid_class_init,
};

static void s5l_chipid_register_types(void)
{
    type_register_static(&s5l_chipid_type_info);
}

type_init(s5l_chipid_register_types)