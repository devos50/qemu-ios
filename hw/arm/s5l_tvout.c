#include "hw/arm/s5l_tvout.h"
#include "qapi/error.h"
#include "migration/vmstate.h"

static void s5l_tvout_update_irq(S5LTVOutState *s)
{
    // a set bit in the mask register masks the interrupt
    qemu_set_irq(s->irq, (s->sdo_irq & ~s->sdo_irq_mask) != 0);
}

/*
 * While the mixer runs, the SDO signals every frame, which completes the swaps of the TV out framebuffer. The handler
 * reprograms the mixer, so signalling right when the mixer is (re)enabled would storm.
 */
static void s5l_tvout_vsync(void *opaque)
{
    S5LTVOutState *s = opaque;

    if (s->mixer_enabled) {
        s->sdo_irq |= SDO_IRQ_VSYNC;
        s5l_tvout_update_irq(s);
    }
    timer_mod(s->vsync_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / TVOUT_REFRESH_RATE);
}

static uint64_t s5l_tvout_sdo_read(void *opaque, hwaddr offset, unsigned size)
{
    S5LTVOutState *s = (S5LTVOutState *)opaque;

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

static void s5l_tvout_sdo_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    S5LTVOutState *s = (S5LTVOutState *)opaque;

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
            s5l_tvout_update_irq(s);
            return;
        case SDO_IRQMASK:
            s->sdo_irq_mask = value;
            s5l_tvout_update_irq(s);
            return;
    }
}

static uint64_t s5l_tvout_mixer1_read(void *opaque, hwaddr offset, unsigned size)
{
    S5LTVOutState *s = (S5LTVOutState *)opaque;

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

static void s5l_tvout_mixer1_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    S5LTVOutState *s = (S5LTVOutState *)opaque;

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

static uint64_t s5l_tvout_mixer2_read(void *opaque, hwaddr offset, unsigned size)
{
    S5LTVOutState *s = (S5LTVOutState *)opaque;

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

static void s5l_tvout_mixer2_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    S5LTVOutState *s = (S5LTVOutState *)opaque;

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

static const MemoryRegionOps s5l_tvout_sdo_ops = {
    .read = s5l_tvout_sdo_read,
    .write = s5l_tvout_sdo_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const MemoryRegionOps s5l_tvout_mixer1_ops = {
    .read = s5l_tvout_mixer1_read,
    .write = s5l_tvout_mixer1_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const MemoryRegionOps s5l_tvout_mixer2_ops = {
    .read = s5l_tvout_mixer2_read,
    .write = s5l_tvout_mixer2_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void s5l_tvout_init(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    S5LTVOutState *s = S5L_TVOUT(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    // mixer 1
    memory_region_init_io(&s->mixer1_iomem, obj, &s5l_tvout_mixer1_ops, s, "tvout_mixer1", 4096);
    sysbus_init_mmio(sbd, &s->mixer1_iomem);

    // mixer 2
    memory_region_init_io(&s->mixer2_iomem, obj, &s5l_tvout_mixer2_ops, s, "tvout_mixer2", 4096);
    sysbus_init_mmio(sbd, &s->mixer2_iomem);

    // SDO
    memory_region_init_io(&s->sdo_iomem, obj, &s5l_tvout_sdo_ops, s, "tvout_sdo", 4096);
    sysbus_init_mmio(sbd, &s->sdo_iomem);

    sysbus_init_irq(sbd, &s->irq);

    s->vsync_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, s5l_tvout_vsync, s);
    timer_mod(s->vsync_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / TVOUT_REFRESH_RATE);
}

static const VMStateDescription vmstate_s5l_tvout = {
    .name = TYPE_S5L_TVOUT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(mixer1_status, S5LTVOutState),
        VMSTATE_UINT32(mixer1_cfg, S5LTVOutState),
        VMSTATE_UINT32(mixer2_status, S5LTVOutState),
        VMSTATE_UINT32(mixer2_cfg, S5LTVOutState),
        VMSTATE_UINT32(sdo_clkcon, S5LTVOutState),
        VMSTATE_UINT32(sdo_config, S5LTVOutState),
        VMSTATE_UINT32(sdo_irq, S5LTVOutState),
        VMSTATE_UINT32(sdo_irq_mask, S5LTVOutState),
        VMSTATE_BOOL(mixer_enabled, S5LTVOutState),
        VMSTATE_TIMER_PTR(vsync_timer, S5LTVOutState),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l_tvout_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_s5l_tvout;
}

static const TypeInfo s5l_tvout_type_info = {
    .name = TYPE_S5L_TVOUT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5LTVOutState),
    .instance_init = s5l_tvout_init,
    .class_init = s5l_tvout_class_init,
};

static void s5l_tvout_register_types(void)
{
    type_register_static(&s5l_tvout_type_info);
}

type_init(s5l_tvout_register_types)