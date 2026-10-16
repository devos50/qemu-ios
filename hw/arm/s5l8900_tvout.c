/*
 * The S5L8900's TV out, as the 1.1.5 kernel's AppleH1TVOut drives it: the SDO (0x39300000, interrupt 0x1e), the
 * mixer (0x39200000, interrupt 0x26) and a third block (0x39100000) that takes the source pictures' formats and
 * scaling. There's no TV to show a picture on, so the registers only read back what was written, except for the
 * SDO's frame interrupt.
 *
 * The driver turns the TV out on as soon as a client hands it a buffer (SpringBoard does at start): it clears the
 * SDO's interrupt status (0x280, writing 1s clears them), unmasks it (0x284 = 0) and enables the SDO (0x000 = 1). It
 * takes the next buffer from its queue, and finishes the one on screen, only on the SDO's frame interrupt (bit 0),
 * and SpringBoard waits for that. The model raises it at 60 Hz while the SDO is enabled. The mixer's interrupt
 * (0x4c, underflows) is never raised.
 */
#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/arm/s5l8900_tvout.h"
#include "migration/vmstate.h"

static void s5l8900_tvout_update_irq(S5L8900TVOutState *s)
{
    uint32_t mask = s->regs[S5L8900_TVOUT_SDO][S5L8900_TVOUT_SDO_INT_MASK / 4];
    qemu_set_irq(s->sdo_irq, (s->sdo_status & ~mask) != 0);
}

static void s5l8900_tvout_schedule(S5L8900TVOutState *s)
{
    if (s->regs[S5L8900_TVOUT_SDO][S5L8900_TVOUT_SDO_ENABLE / 4] & 1) {
        if (!timer_pending(s->frame_timer)) {
            timer_mod(s->frame_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + S5L8900_TVOUT_FRAME_NS);
        }
    } else {
        timer_del(s->frame_timer);
    }
}

static void s5l8900_tvout_frame(void *opaque)
{
    S5L8900TVOutState *s = opaque;

    s->sdo_status |= 1;
    s5l8900_tvout_update_irq(s);
    s5l8900_tvout_schedule(s);
}

static uint64_t s5l8900_tvout_read(S5L8900TVOutState *s, int block, hwaddr addr)
{
    if (block == S5L8900_TVOUT_SDO && addr == S5L8900_TVOUT_SDO_INT) {
        return s->sdo_status;
    }
    return s->regs[block][addr / 4];
}

static void s5l8900_tvout_write(S5L8900TVOutState *s, int block, hwaddr addr, uint64_t val)
{
    if (block == S5L8900_TVOUT_SDO && addr == S5L8900_TVOUT_SDO_INT) {
        s->sdo_status &= ~val;
        s5l8900_tvout_update_irq(s);
        return;
    }
    s->regs[block][addr / 4] = val;
    if (block == S5L8900_TVOUT_SDO) {
        s5l8900_tvout_update_irq(s);
        s5l8900_tvout_schedule(s);
    }
}

#define DEFINE_BLOCK_OPS(name, block) \
    static uint64_t s5l8900_tvout_read_##name(void *opaque, hwaddr addr, unsigned size) \
    { \
        return s5l8900_tvout_read(opaque, block, addr); \
    } \
    static void s5l8900_tvout_write_##name(void *opaque, hwaddr addr, uint64_t val, unsigned size) \
    { \
        s5l8900_tvout_write(opaque, block, addr, val); \
    } \
    static const MemoryRegionOps s5l8900_tvout_ops_##name = { \
        .read = s5l8900_tvout_read_##name, \
        .write = s5l8900_tvout_write_##name, \
        .endianness = DEVICE_LITTLE_ENDIAN, \
        .valid = { .min_access_size = 4, .max_access_size = 4 }, \
    };
DEFINE_BLOCK_OPS(sdo, S5L8900_TVOUT_SDO)
DEFINE_BLOCK_OPS(mixer, S5L8900_TVOUT_MIXER)
DEFINE_BLOCK_OPS(vp, S5L8900_TVOUT_VP)

static void s5l8900_tvout_init(Object *obj)
{
    S5L8900TVOutState *s = S5L8900_TVOUT(obj);
    static const struct { const MemoryRegionOps *ops; const char *name; } blocks[S5L8900_TVOUT_BLOCKS] = {
        [S5L8900_TVOUT_SDO] = { &s5l8900_tvout_ops_sdo, "s5l8900.tvout.sdo" },
        [S5L8900_TVOUT_MIXER] = { &s5l8900_tvout_ops_mixer, "s5l8900.tvout.mixer" },
        [S5L8900_TVOUT_VP] = { &s5l8900_tvout_ops_vp, "s5l8900.tvout.vp" },
    };

    for (int i = 0; i < S5L8900_TVOUT_BLOCKS; i++) {
        memory_region_init_io(&s->iomem[i], obj, blocks[i].ops, s, blocks[i].name, S5L8900_TVOUT_REGS);
        sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem[i]);
    }
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->sdo_irq);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->mixer_irq);
    s->frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, s5l8900_tvout_frame, s);
}

static void s5l8900_tvout_reset(DeviceState *dev)
{
    S5L8900TVOutState *s = S5L8900_TVOUT(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->sdo_status = 0;
    timer_del(s->frame_timer);
}

static const VMStateDescription vmstate_s5l8900_tvout = {
    .name = TYPE_S5L8900_TVOUT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_2DARRAY(regs, S5L8900TVOutState, S5L8900_TVOUT_BLOCKS, S5L8900_TVOUT_REGS / 4),
        VMSTATE_UINT32(sdo_status, S5L8900TVOutState),
        VMSTATE_TIMER_PTR(frame_timer, S5L8900TVOutState),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l8900_tvout_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, s5l8900_tvout_reset);
    dc->vmsd = &vmstate_s5l8900_tvout;
}

static const TypeInfo s5l8900_tvout_info = {
    .name          = TYPE_S5L8900_TVOUT,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900TVOutState),
    .instance_init = s5l8900_tvout_init,
    .class_init    = s5l8900_tvout_class_init,
};

static void s5l8900_tvout_register_types(void)
{
    type_register_static(&s5l8900_tvout_info);
}

type_init(s5l8900_tvout_register_types)
