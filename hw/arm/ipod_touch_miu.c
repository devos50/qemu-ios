/*
 * S5L8720 memory interface unit (MIU): the mobile DDR SDRAM controller and its PHY.
 *
 * Only LLB's miu_init touches it: it locks the PHY DLL, issues the DRAM power-up sequence (precharge all, two auto
 * refreshes, mode register and extended mode register set), enables auto refresh and then the controller. It is not in
 * the device tree, so iOS never accesses it. Guest RAM is plain memory, so we only have to keep LLB's polling loops
 * happy: the DLL reports locked and direct commands complete immediately.
 */
#include "hw/arm/ipod_touch_miu.h"

static uint64_t ipod_touch_miu_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodTouchMIUState *s = IPOD_TOUCH_MIU(opaque);

    switch (addr) {
        case MIU_DIRECT_CMD:
        case MIU_STATUS0:
        case MIU_STATUS1:
            return 0;
        case MIU_PHY_STATUS:
            return MIU_PHY_STATUS_LOCKED;
        default:
            return s->regs[addr / 4];
    }
}

static void ipod_touch_miu_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IPodTouchMIUState *s = IPOD_TOUCH_MIU(opaque);
    s->regs[addr / 4] = val;
}

static const MemoryRegionOps ipod_touch_miu_ops = {
    .read = ipod_touch_miu_read,
    .write = ipod_touch_miu_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void ipod_touch_miu_init(Object *obj)
{
    IPodTouchMIUState *s = IPOD_TOUCH_MIU(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &ipod_touch_miu_ops, s, TYPE_IPOD_TOUCH_MIU, MIU_REG_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const TypeInfo ipod_touch_miu_type_info = {
    .name = TYPE_IPOD_TOUCH_MIU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchMIUState),
    .instance_init = ipod_touch_miu_init,
};

static void ipod_touch_miu_register_types(void)
{
    type_register_static(&ipod_touch_miu_type_info);
}

type_init(ipod_touch_miu_register_types)
