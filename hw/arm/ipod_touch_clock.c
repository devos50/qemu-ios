#include "hw/arm/ipod_touch_clock.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"

static bool s5l8900_clock_known_register(IPodTouchClockState *s, hwaddr addr)
{
    if (s->bus_block) {
        return true; // written on every performance state change, so don't flood the log
    }

    switch (addr) {
    case CLOCK_CONFIG0 ... CLOCK_CONFIG5:
    case CLOCK_PLL0CON ... CLOCK_PLLMODE:
    case CLOCK_PWRCON0:
    case CLOCK_PWRCON1:
    case CLOCK_PWRCON2:
    case CLOCK_PWRCON3:
    case CLOCK_PWRCON4:
        return true;
    default:
        return false;
    }
}

static void s5l8900_clock_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IPodTouchClockState *s = IPOD_TOUCH_CLOCK(opaque);

    if (!s->bus_block && addr == CLOCK_PLLLOCK) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write of 0x%08" PRIx64 " to the read-only PLLLOCK register\n",
                      __func__, val);
        return;
    }
    if (!s5l8900_clock_known_register(s, addr)) {
        qemu_log_mask(LOG_UNIMP, "%s: write of 0x%08" PRIx64 " to unknown register 0x%03" HWADDR_PRIx "\n",
                      __func__, val, addr);
    }
    s->regs[addr / 4] = val;
}

static uint64_t s5l8900_clock_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodTouchClockState *s = IPOD_TOUCH_CLOCK(opaque);

    if (!s->bus_block && addr == CLOCK_PLLLOCK) {
        return (1 | 2 | 4 | 8); // all PLLs are locked
    }
    if (!s5l8900_clock_known_register(s, addr)) {
        qemu_log_mask(LOG_UNIMP, "%s: read from unknown register 0x%03" HWADDR_PRIx "\n", __func__, addr);
    }
    return s->regs[addr / 4];
}

static const MemoryRegionOps clock_ops = {
    .read = s5l8900_clock_read,
    .write = s5l8900_clock_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void s5l8900_clock_reset(DeviceState *dev)
{
    IPodTouchClockState *s = IPOD_TOUCH_CLOCK(dev);
    memset(s->regs, 0, sizeof(s->regs));
}

static void s5l8900_clock_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    IPodTouchClockState *s = IPOD_TOUCH_CLOCK(obj);

    memory_region_init_io(&s->iomem, obj, &clock_ops, s, "clock", CLOCK_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static Property s5l8900_clock_properties[] = {
    DEFINE_PROP_BOOL("bus-block", IPodTouchClockState, bus_block, false),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_s5l8900_clock = {
    .name = TYPE_IPOD_TOUCH_CLOCK,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IPodTouchClockState, CLOCK_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l8900_clock_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = s5l8900_clock_reset;
    dc->vmsd = &vmstate_s5l8900_clock;
    device_class_set_props(dc, s5l8900_clock_properties);
}

static const TypeInfo ipod_touch_clock_info = {
    .name          = TYPE_IPOD_TOUCH_CLOCK,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchClockState),
    .instance_init = s5l8900_clock_init,
    .class_init    = s5l8900_clock_class_init,
};

static void ipod_touch_machine_types(void)
{
    type_register_static(&ipod_touch_clock_info);
}

type_init(ipod_touch_machine_types)
