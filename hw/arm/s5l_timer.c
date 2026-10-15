/*
 * S5L8720 timer block.
 *
 * It contains a 64-bit free running counter and eight timers, all clocked from the 24 MHz fixed-frequency clock.
 *  - iBoot runs the counter at 12 MHz (CFG 0x00018010) and uses timer 4 at 12 MHz for its deadlines.
 *  - The kernel reprograms the counter to the 6 MHz timebase-frequency (CFG 0x10018030) and uses it for
 *    mach_absolute_time. Timer 4 is the decrementer: it runs at 6 MHz (CONFIG 0x1150), gets the number of ticks until
 *    the next deadline in COUNT_BUFFER and is restarted with STATE = 3. Its interrupt is routed to the FIQ, which
 *    acknowledges it by writing 0x03000000 to IRQSTAT.
 * Whether a timer stops or reloads when it reaches COUNT_BUFFER is not known; both iBoot and the kernel reprogram it
 * from the interrupt handler, so the model reloads.
 */
#include "hw/arm/s5l_timer.h"
#include "migration/vmstate.h"
#include "qemu/log.h"

static uint64_t s5l_timer_hz(S5LTimerChannel *ch)
{
    switch ((ch->config >> TIMER_CONFIG_CLK_SHIFT) & TIMER_CONFIG_CLK_MASK) {
    case 0: return TIMER_SRC_HZ / 2;
    case 1: return TIMER_SRC_HZ / 4;
    case 2: return TIMER_SRC_HZ / 16;
    case 3: return TIMER_SRC_HZ / 64;
    case 4: return TIMER_SRC_HZ;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unknown clock select in config 0x%08x\n", __func__, ch->config);
        return TIMER_SRC_HZ;
    }
}

static bool s5l_timer_running(S5LTimerChannel *ch)
{
    return ch->state & TIMER_STATE_START;
}

static uint64_t s5l_timer_period(S5LTimerChannel *ch)
{
    return MAX(ch->count_buffer, 1);
}

static uint64_t s5l_timer_count(S5LTimerChannel *ch, int64_t now)
{
    if (!s5l_timer_running(ch)) {
        return ch->base_count;
    }
    uint64_t count = ch->base_count + muldiv64(now - ch->start_ns, s5l_timer_hz(ch), NANOSECONDS_PER_SECOND);
    return MIN(count, s5l_timer_period(ch));
}

// virtual time at which the timer reaches its count buffer
static int64_t s5l_timer_expiry_ns(S5LTimerChannel *ch)
{
    uint64_t period = s5l_timer_period(ch);
    uint64_t remaining = ch->base_count < period ? period - ch->base_count : 0;
    return ch->start_ns + muldiv64(remaining, NANOSECONDS_PER_SECOND, s5l_timer_hz(ch));
}

static void s5l_timer_schedule(S5LTimerChannel *ch)
{
    if (s5l_timer_running(ch)) {
        timer_mod(ch->timer, s5l_timer_expiry_ns(ch));
    } else {
        timer_del(ch->timer);
    }
}

// restart the count at the current value, e.g. before changing the clock
static void s5l_timer_fold(S5LTimerChannel *ch, int64_t now)
{
    ch->base_count = s5l_timer_count(ch, now);
    ch->start_ns = now;
}

static void s5l_timer_update_irq(S5LTimerState *s)
{
    uint32_t pending = 0;
    for (int n = 4; n < NUM_TIMERS; n++) {
        uint32_t enabled = (s->channels[n].config >> TIMER_CONFIG_IE_SHIFT) & TIMER_CONFIG_IE_MASK;
        pending |= (s->irqstat >> TIMER_IRQSTAT_SHIFT(n)) & enabled;
    }
    qemu_set_irq(s->irq, pending != 0);
}

static void s5l_timer_expired(S5LTimerState *s, int n)
{
    S5LTimerChannel *ch = &s->channels[n];
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    // start the next period where this one ended, unless the timer is a whole
    // period behind: catching up one period per callback would then starve
    // the main loop, so drop the missed periods instead
    ch->start_ns = s5l_timer_expiry_ns(ch);
    ch->base_count = 0;
    if (s5l_timer_expiry_ns(ch) <= now) {
        ch->start_ns = now;
    }
    s5l_timer_schedule(ch);

    if (n >= 4) {
        s->irqstat |= TIMER_IRQ_COUNT_REACHED << TIMER_IRQSTAT_SHIFT(n);
        s5l_timer_update_irq(s);
    } else {
        qemu_log_mask(LOG_UNIMP, "%s: interrupt of timer %d is not modelled\n", __func__, n);
    }
}

#define DEFINE_TIMER_CB(n) \
    static void s5l_timer_expired_##n(void *opaque) { s5l_timer_expired(opaque, n); }
DEFINE_TIMER_CB(0) DEFINE_TIMER_CB(1) DEFINE_TIMER_CB(2) DEFINE_TIMER_CB(3)
DEFINE_TIMER_CB(4) DEFINE_TIMER_CB(5) DEFINE_TIMER_CB(6) DEFINE_TIMER_CB(7)

static QEMUTimerCB *const s5l_timer_callbacks[NUM_TIMERS] = {
    s5l_timer_expired_0, s5l_timer_expired_1, s5l_timer_expired_2, s5l_timer_expired_3,
    s5l_timer_expired_4, s5l_timer_expired_5, s5l_timer_expired_6, s5l_timer_expired_7,
};

static uint64_t s5l_timer_ticks_hz(S5LTimerState *s)
{
    return TIMER_SRC_HZ / (((s->ticks_cfg >> TIMER_TICKS_CFG_DIV_SHIFT) & TIMER_TICKS_CFG_DIV_MASK) + 1);
}

static uint64_t s5l_timer_ticks(S5LTimerState *s, int64_t now)
{
    if (s->ticks_cfg & TIMER_TICKS_CFG_STOP_MASK) {
        return s->ticks_base;
    }
    return s->ticks_base + muldiv64(now - s->ticks_base_ns, s5l_timer_ticks_hz(s), NANOSECONDS_PER_SECOND);
}

// returns the timer that owns the register at addr, and the offset of the register within it
static int s5l_timer_decode(hwaddr addr, hwaddr *reg)
{
    for (int n = 0; n < NUM_TIMERS; n++) {
        if (addr >= TIMER_BASE(n) && addr < TIMER_BASE(n) + TIMER_REG_SIZE) {
            *reg = addr - TIMER_BASE(n);
            return n;
        }
    }
    return -1;
}

static void s5l_timer_write_channel(S5LTimerState *s, int n, hwaddr reg, uint32_t value)
{
    S5LTimerChannel *ch = &s->channels[n];
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    switch (reg) {
    case TIMER_CONFIG:
        s5l_timer_fold(ch, now);
        ch->config = value;
        s5l_timer_update_irq(s);
        break;
    case TIMER_STATE:
        s5l_timer_fold(ch, now);
        if (value & TIMER_STATE_MANUALUPDATE) {
            ch->base_count = 0;
        }
        ch->state = value;
        break;
    case TIMER_COUNT_BUFFER:
        ch->count_buffer = value;
        break;
    case TIMER_COUNT_BUFFER2:
        ch->count_buffer2 = value;
        return;
    case TIMER_PRESCALER:
        ch->prescaler = value;
        return;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: write of 0x%08x to register 0x%02" HWADDR_PRIx " of timer %d\n",
                      __func__, value, reg, n);
        return;
    }
    s5l_timer_schedule(ch);
}

static void s5l_timer_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    S5LTimerState *s = S5L_TIMER(opaque);
    hwaddr reg;
    int n;

    switch (addr) {
    case TIMER_IRQSTAT:
        s->irqstat &= ~value;
        s5l_timer_update_irq(s);
        return;
    case TIMER_TICKS_CFG: {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        s->ticks_base = s5l_timer_ticks(s, now);
        s->ticks_base_ns = now;
        s->ticks_cfg = value;
        return;
    }
    case TIMER_TICKS_CFG + 4 ... TIMER_TICKS_CFG + 0x10:
        s->ticks_regs[(addr - TIMER_TICKS_CFG - 4) / 4] = value;
        return;
    }

    n = s5l_timer_decode(addr, &reg);
    if (n < 0) {
        qemu_log_mask(LOG_UNIMP, "%s: unknown register 0x%03" HWADDR_PRIx " (value 0x%08" PRIx64 ")\n",
                      __func__, addr, value);
        return;
    }
    s5l_timer_write_channel(s, n, reg, value);
}

static uint64_t s5l_timer_read(void *opaque, hwaddr addr, unsigned size)
{
    S5LTimerState *s = S5L_TIMER(opaque);
    S5LTimerChannel *ch;
    hwaddr reg;
    int n;

    switch (addr) {
    case TIMER_TICKSHIGH: {
        // both iBoot and the kernel read the high word first
        uint64_t ticks = s5l_timer_ticks(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        s->ticks_low = ticks;
        return ticks >> 32;
    }
    case TIMER_TICKSLOW:
        return s->ticks_low;
    case TIMER_TICKS_CFG:
        return s->ticks_cfg;
    case TIMER_TICKS_CFG + 4 ... TIMER_TICKS_CFG + 0x10:
        return s->ticks_regs[(addr - TIMER_TICKS_CFG - 4) / 4];
    case TIMER_IRQSTAT:
        return s->irqstat;
    }

    n = s5l_timer_decode(addr, &reg);
    if (n < 0) {
        qemu_log_mask(LOG_UNIMP, "%s: unknown register 0x%03" HWADDR_PRIx "\n", __func__, addr);
        return 0;
    }

    ch = &s->channels[n];
    switch (reg) {
    case TIMER_CONFIG:
        return ch->config;
    case TIMER_STATE:
        return ch->state;
    case TIMER_COUNT_BUFFER:
        return ch->count_buffer;
    case TIMER_COUNT_BUFFER2:
        return ch->count_buffer2;
    case TIMER_PRESCALER:
        return ch->prescaler;
    case TIMER_COUNT:
        return s5l_timer_count(ch, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
    return 0;
}

static const MemoryRegionOps timer1_ops = {
    .read = s5l_timer_read,
    .write = s5l_timer_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void s5l_timer_reset(DeviceState *dev)
{
    S5LTimerState *s = S5L_TIMER(dev);

    for (int n = 0; n < NUM_TIMERS; n++) {
        S5LTimerChannel *ch = &s->channels[n];
        timer_del(ch->timer);
        ch->config = ch->state = ch->count_buffer = ch->count_buffer2 = ch->prescaler = 0;
        ch->base_count = 0;
        ch->start_ns = 0;
    }
    s->irqstat = 0;
    s->ticks_cfg = 0;
    memset(s->ticks_regs, 0, sizeof(s->ticks_regs));
    s->ticks_base = 0;
    s->ticks_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->ticks_low = 0;
    qemu_irq_lower(s->irq);
}

static void s5l_timer_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    S5LTimerState *s = S5L_TIMER(obj);

    memory_region_init_io(&s->iomem, obj, &timer1_ops, s, "timer1", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    for (int n = 0; n < NUM_TIMERS; n++) {
        s->channels[n].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, s5l_timer_callbacks[n], s);
    }
}

static void s5l_timer_finalize(Object *obj)
{
    S5LTimerState *s = S5L_TIMER(obj);

    for (int n = 0; n < NUM_TIMERS; n++) {
        timer_free(s->channels[n].timer);
    }
}

static const VMStateDescription vmstate_s5l_timer_channel = {
    .name = TYPE_S5L_TIMER "/channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(config, S5LTimerChannel),
        VMSTATE_UINT32(state, S5LTimerChannel),
        VMSTATE_UINT32(count_buffer, S5LTimerChannel),
        VMSTATE_UINT32(count_buffer2, S5LTimerChannel),
        VMSTATE_UINT32(prescaler, S5LTimerChannel),
        VMSTATE_UINT64(base_count, S5LTimerChannel),
        VMSTATE_INT64(start_ns, S5LTimerChannel),
        VMSTATE_TIMER_PTR(timer, S5LTimerChannel),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_s5l_timer = {
    .name = TYPE_S5L_TIMER,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(channels, S5LTimerState, NUM_TIMERS, 1,
                             vmstate_s5l_timer_channel, S5LTimerChannel),
        VMSTATE_UINT32(irqstat, S5LTimerState),
        VMSTATE_UINT32(ticks_cfg, S5LTimerState),
        VMSTATE_UINT32_ARRAY(ticks_regs, S5LTimerState, 4),
        VMSTATE_UINT64(ticks_base, S5LTimerState),
        VMSTATE_INT64(ticks_base_ns, S5LTimerState),
        VMSTATE_UINT32(ticks_low, S5LTimerState),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l_timer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, s5l_timer_reset);
    dc->vmsd = &vmstate_s5l_timer;
}

static const TypeInfo s5l_timer_info = {
    .name          = TYPE_S5L_TIMER,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5LTimerState),
    .instance_init = s5l_timer_init,
    .instance_finalize = s5l_timer_finalize,
    .class_init    = s5l_timer_class_init,
};

static void s5l_timer_register_types(void)
{
    type_register_static(&s5l_timer_info);
}

type_init(s5l_timer_register_types)
