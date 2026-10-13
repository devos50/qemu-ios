#include "hw/arm/ipod_touch_tvout.h"
#include "qapi/error.h"
#include "migration/vmstate.h"

static void ipod_touch_tvout_update_irq(IPodTouchTVOutState *s)
{
    // a set bit in the mask register masks the interrupt
    qemu_set_irq(s->irq, (s->sdo_irq & ~s->sdo_irq_mask) != 0);
}

/*
 * While the mixer runs, the SDO signals every frame, which completes the swaps of the TV out framebuffer. The handler
 * reprograms the mixer, so signalling right when the mixer is (re)enabled would storm.
 */
static void ipod_touch_tvout_vsync(void *opaque)
{
    IPodTouchTVOutState *s = opaque;

    if (s->mixer_enabled) {
        s->sdo_irq |= SDO_IRQ_VSYNC;
        ipod_touch_tvout_update_irq(s);
    }
    timer_mod(s->vsync_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / TVOUT_REFRESH_RATE);
}

static uint64_t ipod_touch_tvout_sdo_read(void *opaque, hwaddr offset, unsigned size)
{
    IPodTouchTVOutState *s = (IPodTouchTVOutState *)opaque;

    //printf("%s: offset = 0x%08x\n", __func__, offset);

    switch(offset) {
        case SDO_CLKCON:
            return s->sdo_clkcon;
        case SDO_CONFIG:
            return s->sdo_config;
        case SDO_IRQ:
            return s->sdo_irq;
        case SDO_IRQMASK:
            return s->sdo_irq_mask;
        default:
            break;
    }

    return 0;
}

static void ipod_touch_tvout_sdo_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    IPodTouchTVOutState *s = (IPodTouchTVOutState *)opaque;

    //printf("%s: writing 0x%08x to 0x%08x\n", __func__, value, offset);

    switch(offset) {
        case SDO_CLKCON:
            s->sdo_clkcon = value;
            return;
        case SDO_CONFIG:
            s->sdo_config = value;
            return;
        case SDO_IRQ:
            s->sdo_irq &= ~value;
            ipod_touch_tvout_update_irq(s);
            return;
        case SDO_IRQMASK:
            s->sdo_irq_mask = value;
            ipod_touch_tvout_update_irq(s);
            return;
    }
}

static uint64_t ipod_touch_tvout_mixer1_read(void *opaque, hwaddr offset, unsigned size)
{
    IPodTouchTVOutState *s = (IPodTouchTVOutState *)opaque;

    //printf("%s: offset = 0x%08x\n", __func__, offset);

    switch(offset) {
        case MXR_STATUS:
            return 0x4;
        case MXR_CFG:
            return s->mixer1_cfg;
        default:
            break;
    }

    return 0;
}

static void ipod_touch_tvout_mixer1_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    IPodTouchTVOutState *s = (IPodTouchTVOutState *)opaque;

    //printf("%s: writing 0x%08x to 0x%08x\n", __func__, value, offset);

    switch(offset) {
        case MXR_STATUS:
            s->mixer_enabled = value & MXR_STATUS_ENABLE;
            break;
        case MXR_CFG:
            s->mixer1_cfg = value;
            break;
    }
}

static uint64_t ipod_touch_tvout_mixer2_read(void *opaque, hwaddr offset, unsigned size)
{
    IPodTouchTVOutState *s = (IPodTouchTVOutState *)opaque;

    //printf("%s: offset = 0x%08x\n", __func__, offset);

    switch(offset) {
        case MXR_STATUS:
            return s->mixer2_status;
        case MXR_CFG:
            return s->mixer2_cfg;
        default:
            break;
    }

    return 0;
}

static void ipod_touch_tvout_mixer2_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    IPodTouchTVOutState *s = (IPodTouchTVOutState *)opaque;

    //printf("%s: writing 0x%08x to 0x%08x\n", __func__, value, offset);

    switch(offset) {
        case MXR_STATUS:
            s->mixer2_status = value;
            break;
        case MXR_CFG:
            s->mixer2_cfg = value;
            break;
    }
}

static const MemoryRegionOps ipod_touch_tvout_sdo_ops = {
    .read = ipod_touch_tvout_sdo_read,
    .write = ipod_touch_tvout_sdo_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const MemoryRegionOps ipod_touch_tvout_mixer1_ops = {
    .read = ipod_touch_tvout_mixer1_read,
    .write = ipod_touch_tvout_mixer1_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const MemoryRegionOps ipod_touch_tvout_mixer2_ops = {
    .read = ipod_touch_tvout_mixer2_read,
    .write = ipod_touch_tvout_mixer2_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipod_touch_tvout_init(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    IPodTouchTVOutState *s = IPOD_TOUCH_TVOUT(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    // mixer 1
    memory_region_init_io(&s->mixer1_iomem, obj, &ipod_touch_tvout_mixer1_ops, s, "tvout_mixer1", 4096);
    sysbus_init_mmio(sbd, &s->mixer1_iomem);

    // mixer 2
    memory_region_init_io(&s->mixer2_iomem, obj, &ipod_touch_tvout_mixer2_ops, s, "tvout_mixer2", 4096);
    sysbus_init_mmio(sbd, &s->mixer2_iomem);

    // SDO
    memory_region_init_io(&s->sdo_iomem, obj, &ipod_touch_tvout_sdo_ops, s, "tvout_sdo", 4096);
    sysbus_init_mmio(sbd, &s->sdo_iomem);

    sysbus_init_irq(sbd, &s->irq);

    s->vsync_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ipod_touch_tvout_vsync, s);
    timer_mod(s->vsync_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / TVOUT_REFRESH_RATE);
}

static const VMStateDescription vmstate_ipod_touch_tvout = {
    .name = TYPE_IPOD_TOUCH_TVOUT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(mixer1_status, IPodTouchTVOutState),
        VMSTATE_UINT32(mixer1_cfg, IPodTouchTVOutState),
        VMSTATE_UINT32(mixer2_status, IPodTouchTVOutState),
        VMSTATE_UINT32(mixer2_cfg, IPodTouchTVOutState),
        VMSTATE_UINT32(sdo_clkcon, IPodTouchTVOutState),
        VMSTATE_UINT32(sdo_config, IPodTouchTVOutState),
        VMSTATE_UINT32(sdo_irq, IPodTouchTVOutState),
        VMSTATE_UINT32(sdo_irq_mask, IPodTouchTVOutState),
        VMSTATE_BOOL(mixer_enabled, IPodTouchTVOutState),
        VMSTATE_TIMER_PTR(vsync_timer, IPodTouchTVOutState),
        VMSTATE_END_OF_LIST()
    }
};

static void ipod_touch_tvout_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_ipod_touch_tvout;
}

static const TypeInfo ipod_touch_tvout_type_info = {
    .name = TYPE_IPOD_TOUCH_TVOUT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchTVOutState),
    .instance_init = ipod_touch_tvout_init,
    .class_init = ipod_touch_tvout_class_init,
};

static void ipod_touch_tvout_register_types(void)
{
    type_register_static(&ipod_touch_tvout_type_info);
}

type_init(ipod_touch_tvout_register_types)