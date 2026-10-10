#include "hw/arm/ipod_touch_mbx.h"

static uint64_t ipod_touch_mbx1_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodTouchMBXState *s = (IPodTouchMBXState *)opaque;
    //if (addr != 0x1020)
    	//printf("%s: read from location 0x%08x\n", __func__, addr);
    switch(addr)
    {
        case 0x12c:
            return 0x40;
        case 0xf00:
            return (2 << 0x10) | (1 << 0x18); // seems to be some kind of identifier
        case MBX_MMU_CTRL:
            // The MMU state follows the enable bit at once, and invalidations finish at once.
            return (s->mmu_ctrl & ~(MBX_MMU_CTRL_ENABLED | MBX_MMU_CTRL_INVALIDATE_BUSY)) |
                   ((s->mmu_ctrl & MBX_MMU_CTRL_ENABLE) ? MBX_MMU_CTRL_ENABLED : 0);
        default:
            break;
    }
    return 0;
}

static void ipod_touch_mbx1_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IPodTouchMBXState *s = (IPodTouchMBXState *)opaque;
    //fprintf(stderr, "%s: writing 0x%08x to 0x%08x\n", __func__, val, addr);

    switch(addr)
    {
        case MBX_MMU_CTRL:
            s->mmu_ctrl = val & ~MBX_MMU_CTRL_INVALIDATE;
            break;
    }
}

static uint64_t ipod_touch_mbx2_read(void *opaque, hwaddr addr, unsigned size)
{
    //printf("%s: read from location 0x%08x\n", __func__, addr);
    switch(addr)
    {
	case 0x4:
	    return 0xFF;
	    break;
        default:
            break;
    }
    return 0;
}

static void ipod_touch_mbx2_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IPodTouchMBXState *s = (IPodTouchMBXState *)opaque;
    //fprintf(stderr, "%s: writing 0x%08x to 0x%08x\n", __func__, val, addr);
}

static const MemoryRegionOps ipod_touch_mbx1_ops = {
    .read = ipod_touch_mbx1_read,
    .write = ipod_touch_mbx1_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const MemoryRegionOps ipod_touch_mbx2_ops = {
    .read = ipod_touch_mbx2_read,
    .write = ipod_touch_mbx2_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipod_touch_mbx_init(Object *obj)
{
    IPodTouchMBXState *s = IPOD_TOUCH_MBX(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem1, obj, &ipod_touch_mbx1_ops, s, TYPE_IPOD_TOUCH_MBX, 0x1000000);
    sysbus_init_mmio(sbd, &s->iomem1);
    memory_region_init_io(&s->iomem2, obj, &ipod_touch_mbx2_ops, s, TYPE_IPOD_TOUCH_MBX, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem2);
}

static void ipod_touch_mbx_class_init(ObjectClass *klass, void *data)
{
    
}

static const TypeInfo ipod_touch_mbx_type_info = {
    .name = TYPE_IPOD_TOUCH_MBX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchMBXState),
    .instance_init = ipod_touch_mbx_init,
    .class_init = ipod_touch_mbx_class_init,
};

static void ipod_touch_mbx_register_types(void)
{
    type_register_static(&ipod_touch_mbx_type_info);
}

type_init(ipod_touch_mbx_register_types)
