#include "qemu/osdep.h"
#include "hw/arm/ipod_touch_firmware.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qapi/qapi-visit-common.h"
#include "hw/arm/boot.h"
#include "exec/address-spaces.h"
#include "hw/misc/unimp.h"
#include "hw/irq.h"
#include "system/system.h"
#include "system/runstate.h"
#include "system/reset.h"
#include "hw/platform-bus.h"
#include "hw/block/flash.h"
#include "hw/qdev-clock.h"
#include "hw/arm/exynos4210.h"
#include "hw/arm/ipod_touch_2g.h"
#include "hw/arm/guest-services/gles.h"
#include "hw/arm/ipod_touch_pcf50633_pmu.h"
#include "target/arm/cpregs.h"
#include "qemu/error-report.h"
#include "qemu/cutils.h"
#include "ui/input.h"
#include "ui/clipboard.h"

// The D1759 PMU raises this GPIO IRQ when a wake button (hold/menu) is pressed.
// It lands in the same GPIO interrupt group the button code already drives.
#define PMU_WAKE_IRQ 0x61

#define VMSTATE_IT2G_CPREG(name) \
        VMSTATE_UINT64(IT2G_CPREG_VAR_NAME(name), IPodTouchMachineState)

#define IT2G_CPREG_DEF(p_name, p_op0, p_op1, p_crn, p_crm, p_op2, p_access, p_reset) \
    {                                                                              \
        .cp = 15,                                              \
        .name = #p_name, .opc0 = p_op0, .crn = p_crn, .crm = p_crm,                \
        .opc1 = p_op1, .opc2 = p_op2, .access = p_access, .resetvalue = p_reset,   \
        .state = ARM_CP_STATE_AA32, .type = ARM_CP_OVERRIDE,                       \
        .fieldoffset = offsetof(IPodTouchMachineState, IT2G_CPREG_VAR_NAME(p_name))           \
                       - offsetof(ARMCPU, env)                                     \
    }

#define IT2G_CPREG_DEF_QEMU_CALL \
    {                            \
        .cp = 15,                \
        .name = "QEMU_CALL",     \
        .opc0 = 0,               \
        .opc1 = 3,               \
        .crn = 15,               \
        .crm = 15,               \
        .opc2 = 0,               \
        .access = PL0_RW,        \
        .resetvalue = 0,         \
        .state = ARM_CP_STATE_AA32, \
        .type = ARM_CP_IO,       \
        .fieldoffset = offsetof(IPodTouchMachineState, IT2G_CPREG_VAR_NAME(QEMU_CALL)) \
                       - offsetof(ARMCPU, env), \
        .readfn = qemu_call_status, \
        .writefn = qemu_call,     \
    }

const int S5L8900_GPIO_IRQS[5] = { S5L8900_GPIO_G0_IRQ, S5L8900_GPIO_G1_IRQ, S5L8900_GPIO_G2_IRQ, S5L8900_GPIO_G3_IRQ, S5L8900_GPIO_G4_IRQ };

static MemoryRegion *allocate_ram(MemoryRegion *top, const char *name,
                                  uint32_t addr, uint32_t size)
{
    MemoryRegion *sec = g_new(MemoryRegion, 1);
    memory_region_init_ram(sec, NULL, name, size, &error_fatal);
    memory_region_add_subregion(top, addr, sec);
    return sec;
}

/*
 * IT_AMC_WATCH -- a probe, not a device. Off unless the variable is set, and
 * when it is set the guest sees identical memory: every access is forwarded to
 * the RAM that would have served it.
 *
 * It exists to answer one question that no amount of dumping can: the AMC's
 * buffer aperture is ordinary RAM, so the guest reading and writing it is
 * invisible. In particular we cannot otherwise see WHICH addresses the driver
 * reads back after a job, and that set of addresses is, by definition, where
 * the engine's output is expected to be.
 *
 * Set it to "r", "w" or "rw" to choose which direction is logged; "1" means
 * reads, which is the interesting one. Every line carries the guest PC, so the
 * reads can be attributed the same way amc_log_caller() attributes registers.
 */
typedef struct {
    MemoryRegion io;
    uint8_t *ram;          /* the shadowed RAM, which still holds the data */
    bool log_reads;
    bool log_writes;
    bool nonzero_only;     /* IT_RAM_WATCH: only report writes that carry data */
    const char *tag;
    hwaddr base;
    int budget;
} ApertureWatch;

static void aperture_watch_log(ApertureWatch *w, const char *dir, hwaddr off,
                               unsigned size, uint64_t val)
{
    uint32_t pc = 0;

    if (w->budget <= 0) {
        return;
    }
    if (w->nonzero_only && val == 0) {
        return;
    }
    w->budget--;
    if (current_cpu) {
        pc = ARM_CPU(current_cpu)->env.regs[15];
    }
    fprintf(stderr, "[%s] %s +%05x size=%u val=%08x pc=%08x t=%" PRId64 "\n",
            w->tag, dir, (unsigned)off, size, (uint32_t)val, pc,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static uint64_t aperture_watch_read(void *opaque, hwaddr off, unsigned size)
{
    ApertureWatch *w = opaque;
    uint64_t val = 0;

    memcpy(&val, w->ram + off, size);
    if (w->log_reads) {
        aperture_watch_log(w, "R", off, size, val);
    }
    return val;
}

static void aperture_watch_write(void *opaque, hwaddr off, uint64_t val,
                                 unsigned size)
{
    ApertureWatch *w = opaque;

    memcpy(w->ram + off, &val, size);
    if (w->log_writes) {
        aperture_watch_log(w, "W", off, size, val);
    }
}

static const MemoryRegionOps aperture_watch_ops = {
    .read = aperture_watch_read,
    .write = aperture_watch_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void install_aperture_watch(MemoryRegion *top, MemoryRegion *backing,
                                   hwaddr base, uint64_t size)
{
    const char *spec = getenv("IT_AMC_WATCH");
    ApertureWatch *w;

    if (!spec) {
        return;
    }
    w = g_new0(ApertureWatch, 1);
    w->ram = memory_region_get_ram_ptr(backing);
    w->tag = "APW";
    w->base = base;
    w->log_reads = !strchr(spec, 'w') || strchr(spec, 'r');
    w->log_writes = strchr(spec, 'w') != NULL;
    w->budget = 400000;
    memory_region_init_io(&w->io, NULL, &aperture_watch_ops, w,
                          "amc-aperture-watch", size);
    /* Higher priority than the RAM, so it intercepts; we then forward. */
    memory_region_add_subregion_overlap(top, base, &w->io, 1);
    warn_report("ipod: AMC aperture watch active (reads=%d writes=%d) -- "
                "this is a probe and slows the guest",
                w->log_reads, w->log_writes);
}

/*
 * IT_RAM_WATCH=<hex base>:<hex size>[:rw][:nz] -- the same shadow trick, but
 * over an arbitrary window of ordinary DRAM.
 *
 * The audio investigation needs it: the PL080 reads the guest's PCM ring out of
 * plain kernel RAM (0x08c99000, 64 KB) and delivers pure silence, and the only
 * way to tell "nobody wrote it" from "somebody wrote zeroes" or "somebody wrote
 * a DIFFERENT buffer" is to watch the physical pages themselves. `nz` logs only
 * writes carrying a non-zero value, which is what separates a producer from an
 * allocator zeroing a page.
 *
 * Off unless set, and when set the guest sees identical memory -- every access
 * is forwarded to the RAM that would have served it. It costs guest time, so
 * keep the window small (see the instrumentation trap in the memory notes: a
 * probe that slows the guest can change the gesture it observes).
 */
static void install_ram_watch(MemoryRegion *top, MemoryRegion *backing,
                              hwaddr backing_base, uint64_t backing_size)
{
    const char *spec = getenv("IT_RAM_WATCH");
    unsigned long long base, size;
    ApertureWatch *w;

    if (!spec) {
        return;
    }
    if (sscanf(spec, "%llx:%llx", &base, &size) != 2 || size == 0) {
        error_report("IT_RAM_WATCH: expected <hexbase>:<hexsize>[:rw][:nz]");
        exit(1);
    }
    if (base < backing_base || base + size > backing_base + backing_size) {
        return;         /* not this region */
    }
    w = g_new0(ApertureWatch, 1);
    w->ram = memory_region_get_ram_ptr(backing) + (base - backing_base);
    w->tag = "RAMW";
    w->base = base;
    w->log_writes = strchr(spec, 'w') != NULL || !strchr(spec, 'r');
    w->log_reads = strchr(spec, 'r') != NULL;
    w->nonzero_only = strstr(spec, ":nz") != NULL;
    w->budget = 200000;
    memory_region_init_io(&w->io, NULL, &aperture_watch_ops, w,
                          "ipod-ram-watch", size);
    memory_region_add_subregion_overlap(top, base, &w->io, 1);
    warn_report("ipod: RAM watch on %08llx+%llx (reads=%d writes=%d nz=%d) -- "
                "a probe; it slows the guest",
                base, size, w->log_reads, w->log_writes, w->nonzero_only);
}

static void ipod_touch_get_time_dilation(Object *obj, Visitor *v, const char *name,
                                          void *opaque, Error **errp)
{
    uint32_t value = IPOD_TOUCH_MACHINE(obj)->time_dilation;
    visit_type_uint32(v, name, &value, errp);
}

static void ipod_touch_set_time_dilation(Object *obj, Visitor *v, const char *name,
                                          void *opaque, Error **errp)
{
    IPodTouchMachineState *s = IPOD_TOUCH_MACHINE(obj);
    if (s->cpu) {
        error_setg(errp, "time-dilation must be set before the machine starts");
        return;
    }
    uint32_t value;
    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value < 1 || value > IT_TIMER_MAX_DILATION) {
        error_setg(errp, "time-dilation must be between 1 and %u", IT_TIMER_MAX_DILATION);
        return;
    }
    s->time_dilation = value;
    s->time_dilation_explicit = true;
}

static bool ipod_touch_time_env_alias(IPodTouchMachineState *s, Error **errp)
{
    const char *text = getenv("IT_TIME_DILATION");
    if (text && !s->time_dilation_explicit) {
        uint64_t value;
        if (qemu_strtou64(text, NULL, 0, &value) || value < 1 || value > IT_TIMER_MAX_DILATION) {
            error_setg(errp, "IT_TIME_DILATION must be between 1 and %u", IT_TIMER_MAX_DILATION);
            return false;
        }
        s->time_dilation = value;
        warn_report_once("IT_TIME_DILATION is deprecated; use -M iPod-Touch,time-dilation=");
    }
    return true;
}

static void ipod_touch_get_boot_args_delay_ms(Object *obj, Visitor *v,
                                             const char *name, void *opaque,
                                             Error **errp)
{
    uint32_t value = IPOD_TOUCH_MACHINE(obj)->boot_args_delay_ms;
    visit_type_uint32(v, name, &value, errp);
}

static void ipod_touch_set_boot_args_delay_ms(Object *obj, Visitor *v,
                                             const char *name, void *opaque,
                                             Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    if (nms->cpu) {
        error_setg(errp, "boot-args-delay-ms must be set before the machine starts");
        return;
    }
    uint32_t value;
    if (!visit_type_uint32(v, name, &value, errp)) return;
    if (value > 3600000) {
        error_setg(errp, "boot-args-delay-ms must be between 0 and 3600000");
        return;
    }
    nms->boot_args_delay_ms = value;
    nms->boot_args_delay_ms_explicit = true;
}

static void ipod_touch_get_boot_args_repeat(Object *obj, Visitor *v,
                                             const char *name, void *opaque,
                                             Error **errp)
{
    uint32_t value = IPOD_TOUCH_MACHINE(obj)->boot_args_repeat;
    visit_type_uint32(v, name, &value, errp);
}

static void ipod_touch_set_boot_args_repeat(Object *obj, Visitor *v,
                                             const char *name, void *opaque,
                                             Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    if (nms->cpu) {
        error_setg(errp, "boot-args-repeat must be set before the machine starts");
        return;
    }
    uint32_t value;
    if (!visit_type_uint32(v, name, &value, errp)) return;
    if (value > 1000000) {
        error_setg(errp, "boot-args-repeat must be between 0 and 1000000");
        return;
    }
    nms->boot_args_repeat = value;
    nms->boot_args_repeat_explicit = true;
}

static void ipod_touch_get_boot_args_interval_ms(Object *obj, Visitor *v,
                                             const char *name, void *opaque,
                                             Error **errp)
{
    uint32_t value = IPOD_TOUCH_MACHINE(obj)->boot_args_interval_ms;
    visit_type_uint32(v, name, &value, errp);
}

static void ipod_touch_set_boot_args_interval_ms(Object *obj, Visitor *v,
                                             const char *name, void *opaque,
                                             Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    if (nms->cpu) {
        error_setg(errp, "boot-args-interval-ms must be set before the machine starts");
        return;
    }
    uint32_t value;
    if (!visit_type_uint32(v, name, &value, errp)) return;
    if (value < 1 || value > 3600000) {
        error_setg(errp, "boot-args-interval-ms must be between 1 and 3600000");
        return;
    }
    nms->boot_args_interval_ms = value;
    nms->boot_args_interval_ms_explicit = true;
}

static bool ipod_touch_boot_args_env_aliases(IPodTouchMachineState *nms,
                                              Error **errp)
{
    const char *delay_ms = getenv("IT_BOOT_ARGS_DELAY_MS");
    if (delay_ms && !nms->boot_args_delay_ms_explicit) {
        uint64_t value;
        if (qemu_strtou64(delay_ms, NULL, 0, &value) ||
            value > 3600000) {
            error_setg(errp, "IT_BOOT_ARGS_DELAY_MS must be between 0 and 3600000");
            return false;
        }
        nms->boot_args_delay_ms = value;
        warn_report_once("IT_BOOT_ARGS_DELAY_MS is deprecated; use -M iPod-Touch,boot-args-delay-ms=");
    }
    const char *repeat = getenv("IT_BOOT_ARGS_REPEAT");
    if (repeat && !nms->boot_args_repeat_explicit) {
        uint64_t value;
        if (qemu_strtou64(repeat, NULL, 0, &value) ||
            value > 1000000) {
            error_setg(errp, "IT_BOOT_ARGS_REPEAT must be between 0 and 1000000");
            return false;
        }
        nms->boot_args_repeat = value;
        warn_report_once("IT_BOOT_ARGS_REPEAT is deprecated; use -M iPod-Touch,boot-args-repeat=");
    }
    const char *interval_ms = getenv("IT_BOOT_ARGS_INTERVAL_MS");
    if (interval_ms && !nms->boot_args_interval_ms_explicit) {
        uint64_t value;
        if (qemu_strtou64(interval_ms, NULL, 0, &value) ||
            value < 1 || value > 3600000) {
            error_setg(errp, "IT_BOOT_ARGS_INTERVAL_MS must be between 1 and 3600000");
            return false;
        }
        nms->boot_args_interval_ms = value;
        warn_report_once("IT_BOOT_ARGS_INTERVAL_MS is deprecated; use -M iPod-Touch,boot-args-interval-ms=");
    }
    return true;
}

static void ipod_touch_get_bt(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    bool value = IPOD_TOUCH_MACHINE(obj)->bt_enabled;
    visit_type_bool(v, name, &value, errp);
}

static void ipod_touch_set_bt(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    if (nms->cpu) {
        error_setg(errp, "bt must be set before the machine starts");
        return;
    }
    bool value;
    if (visit_type_bool(v, name, &value, errp)) {
        nms->bt_enabled = value;
        nms->bt_enabled_explicit = true;
    }
}

static void ipod_touch_get_bt_latency_us(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    uint32_t value = IPOD_TOUCH_MACHINE(obj)->bt_latency_us;
    visit_type_uint32(v, name, &value, errp);
}

static void ipod_touch_set_bt_latency_us(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    if (nms->cpu) {
        error_setg(errp, "bt-latency-us must be set before the machine starts");
        return;
    }
    uint32_t value;
    if (visit_type_uint32(v, name, &value, errp)) {
        nms->bt_latency_us = value;
        nms->bt_latency_us_explicit = true;
    }
}

static bool ipod_touch_bt_env_aliases(IPodTouchMachineState *nms, Error **errp)
{
    const char *enabled = getenv("IT_BT");
    const char *latency = getenv("IT_BT_LATENCY_US");
    if (enabled && !nms->bt_enabled_explicit) {
        nms->bt_enabled = enabled[0] != '0';
        warn_report_once("IT_BT is deprecated; use -M iPod-Touch,bt=on|off");
    }
    if (latency && !nms->bt_latency_us_explicit) {
        uint64_t value;
        if (qemu_strtou64(latency, NULL, 0, &value) || value > UINT32_MAX) {
            error_setg(errp, "IT_BT_LATENCY_US must be an unsigned 32-bit microsecond value");
            return false;
        }
        nms->bt_latency_us = value;
        warn_report_once("IT_BT_LATENCY_US is deprecated; use -M iPod-Touch,bt-latency-us=");
    }
    return true;
}

static const char *const amc_mode_names[] = { "registers", "handshake", "decode" };

static char *ipod_touch_get_amc_mode(Object *obj, Error **errp)
{
    return g_strdup(amc_mode_names[IPOD_TOUCH_MACHINE(obj)->amc_mode]);
}

static void ipod_touch_set_amc_mode(Object *obj, const char *value, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    if (nms->cpu) {
        error_setg(errp, "amc-mode must be set before the machine starts");
        return;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(amc_mode_names); i++) {
        if (!strcmp(value, amc_mode_names[i])) {
            nms->amc_mode = i;
            nms->amc_mode_explicit = true;
            return;
        }
    }
    error_setg(errp, "amc-mode must be registers, handshake or decode");
}

static void ipod_touch_amc_env_alias(IPodTouchMachineState *nms)
{
    if (nms->amc_mode_explicit) {
        return;
    }
    if (getenv("IT_AMC_DECODE") || getenv("IT_AMC_AAC")) {
        nms->amc_mode = AMC_MODE_DECODE;
        warn_report_once("IT_AMC_DECODE/IT_AMC_AAC are deprecated; use -M iPod-Touch,amc-mode=decode");
    } else if (getenv("IT_AMC_STATE")) {
        nms->amc_mode = AMC_MODE_HANDSHAKE;
        warn_report_once("IT_AMC_STATE is deprecated; use -M iPod-Touch,amc-mode=handshake");
    }
}

static void ipod_touch_get_h264_decode(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    bool value = IPOD_TOUCH_MACHINE(obj)->h264_decode;
    visit_type_bool(v, name, &value, errp);
}

static void ipod_touch_set_h264_decode(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    if (nms->cpu) {
        error_setg(errp, "h264-decode must be set before the machine starts");
        return;
    }
    bool value;
    if (visit_type_bool(v, name, &value, errp)) {
        nms->h264_decode = value;
        nms->h264_decode_explicit = true;
    }
}

static void ipod_touch_h264_env_alias(IPodTouchMachineState *nms)
{
    if (!nms->h264_decode_explicit && getenv("IT_H264_DECODE") != NULL) {
        /* The old alias tests presence, even for an empty or "0" value. */
        nms->h264_decode = true;
        warn_report_once("IT_H264_DECODE is deprecated; use -M iPod-Touch,h264-decode=on");
    }
}

static void ipod_touch_get_forge_sigcheck(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    bool value = IPOD_TOUCH_MACHINE(obj)->forge_sigcheck;
    visit_type_bool(v, name, &value, errp);
}

static void ipod_touch_set_forge_sigcheck(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    if (nms->cpu) {
        error_setg(errp, "forge-sigcheck must be set before the machine starts");
        return;
    }
    bool value;
    if (visit_type_bool(v, name, &value, errp)) {
        nms->forge_sigcheck = value;
        nms->forge_sigcheck_explicit = true;
    }
}

static void ipod_touch_forge_sigcheck_env_alias(IPodTouchMachineState *nms)
{
    if (!nms->forge_sigcheck_explicit && getenv("IT_FORGE_SIGCHECK") != NULL) {
        /* The old alias tests presence, even for an empty or "0" value. */
        nms->forge_sigcheck = true;
        warn_report_once("IT_FORGE_SIGCHECK is deprecated; use -M iPod-Touch,forge-sigcheck=on");
    }
}

static void ipod_touch_get_lcd_planes(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    bool value = IPOD_TOUCH_MACHINE(obj)->lcd_planes;
    visit_type_bool(v, name, &value, errp);
}

static void ipod_touch_set_lcd_planes(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    if (nms->cpu) {
        error_setg(errp, "lcd-planes must be set before the machine starts");
        return;
    }
    bool value;
    if (visit_type_bool(v, name, &value, errp)) {
        nms->lcd_planes = value;
        nms->lcd_planes_explicit = true;
    }
}

static void ipod_touch_lcd_planes_env_alias(IPodTouchMachineState *nms)
{
    if (!nms->lcd_planes_explicit && getenv("IT_LCD_PLANES") != NULL) {
        /* The old alias tests presence, even for an empty or "0" value. */
        nms->lcd_planes = true;
        warn_report_once("IT_LCD_PLANES is deprecated; use -M iPod-Touch,lcd-planes=on");
    }
}

static void ipod_touch_get_mpvd_decode(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    bool value = IPOD_TOUCH_MACHINE(obj)->mpvd_decode;
    visit_type_bool(v, name, &value, errp);
}

static void ipod_touch_set_mpvd_decode(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    if (nms->cpu) {
        error_setg(errp, "mpvd-decode must be set before the machine starts");
        return;
    }
    bool value;
    if (visit_type_bool(v, name, &value, errp)) {
        nms->mpvd_decode = value;
        nms->mpvd_decode_explicit = true;
    }
}

static void ipod_touch_mpvd_env_alias(IPodTouchMachineState *nms)
{
    if (!nms->mpvd_decode_explicit && getenv("IT_MPVD_DECODE") != NULL) {
        /* The old alias tests presence, even for an empty or "0" value. */
        nms->mpvd_decode = true;
        warn_report_once("IT_MPVD_DECODE is deprecated; use -M iPod-Touch,mpvd-decode=on");
    }
}

static void ipod_touch_get_scaler_decode(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    bool value = IPOD_TOUCH_MACHINE(obj)->scaler_decode;
    visit_type_bool(v, name, &value, errp);
}

static void ipod_touch_set_scaler_decode(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    if (nms->cpu) {
        error_setg(errp, "scaler-decode must be set before the machine starts");
        return;
    }
    bool value;
    if (visit_type_bool(v, name, &value, errp)) {
        nms->scaler_decode = value;
        nms->scaler_decode_explicit = true;
    }
}

static void ipod_touch_scaler_env_alias(IPodTouchMachineState *nms)
{
    if (!nms->scaler_decode_explicit && getenv("IT_SCALER_DECODE") != NULL) {
        /* The old alias tests presence, even for an empty or "0" value. */
        nms->scaler_decode = true;
        warn_report_once("IT_SCALER_DECODE is deprecated; use -M iPod-Touch,scaler-decode=on");
    }
}

static void ipod_touch_get_wdt_noreset(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    bool value = IPOD_TOUCH_MACHINE(obj)->wdt_noreset;
    visit_type_bool(v, name, &value, errp);
}

static void ipod_touch_set_wdt_noreset(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    if (nms->cpu) {
        error_setg(errp, "wdt-noreset must be set before the machine starts");
        return;
    }
    bool value;
    if (visit_type_bool(v, name, &value, errp)) {
        nms->wdt_noreset = value;
        nms->wdt_noreset_explicit = true;
    }
}

static void ipod_touch_wdt_env_alias(IPodTouchMachineState *nms)
{
    if (!nms->wdt_noreset_explicit && getenv("IT_WDT_NORESET") != NULL) {
        /* The old alias tests presence, even for an empty or "0" value. */
        nms->wdt_noreset = true;
        warn_report_once("IT_WDT_NORESET is deprecated; use -M iPod-Touch,wdt-noreset=on");
    }
}

static void ipod_touch_get_osk(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    bool value = IPOD_TOUCH_MACHINE(obj)->osk_enabled;
    visit_type_bool(v, name, &value, errp);
}

static void ipod_touch_set_osk(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    if (nms->cpu) {
        error_setg(errp, "osk must be set before the machine starts");
        return;
    }
    bool value;
    if (visit_type_bool(v, name, &value, errp)) {
        nms->osk_enabled = value;
        nms->osk_explicit = true;
    }
}

static void ipod_touch_osk_env_alias(IPodTouchMachineState *nms)
{
    if (!nms->osk_explicit && getenv("IT_OSK") != NULL) {
        /* The old alias tests presence, even for an empty or "0" value. */
        nms->osk_enabled = true;
        warn_report_once("IT_OSK is deprecated; use -M iPod-Touch,osk=on");
    }
}

static char *ipod_touch_get_direct_iboot(Object *obj, Error **errp)
{
    return g_strdup(IPOD_TOUCH_MACHINE(obj)->direct_iboot);
}

static void ipod_touch_set_direct_iboot(Object *obj, const char *value, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    if (nms->cpu) {
        error_setg(errp, "direct-iboot must be set before the machine starts");
        return;
    }
    if (strlen(value) >= sizeof(nms->direct_iboot)) {
        error_setg(errp, "direct-iboot path is too long");
        return;
    }
    g_strlcpy(nms->direct_iboot, value, sizeof(nms->direct_iboot));
    nms->direct_iboot_explicit = true;
}

static char *ipod_touch_get_direct_llb(Object *obj, Error **errp)
{
    return g_strdup(IPOD_TOUCH_MACHINE(obj)->direct_llb);
}

static void ipod_touch_set_direct_llb(Object *obj, const char *value, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    if (nms->cpu) {
        error_setg(errp, "direct-llb must be set before the machine starts");
        return;
    }
    if (strlen(value) >= sizeof(nms->direct_llb)) {
        error_setg(errp, "direct-llb path is too long");
        return;
    }
    g_strlcpy(nms->direct_llb, value, sizeof(nms->direct_llb));
    nms->direct_llb_explicit = true;
}

static void ipod_touch_direct_boot_env_aliases(IPodTouchMachineState *nms)
{
    const char *iboot = getenv("IT_DIRECT_IBOOT");
    if (!nms->direct_iboot_explicit && iboot) {
        ipod_touch_set_direct_iboot(OBJECT(nms), iboot, &error_fatal);
        warn_report_once("IT_DIRECT_IBOOT is deprecated; use direct-iboot=PATH");
    }
    const char *llb = getenv("IT_DIRECT_LLB");
    if (!nms->direct_llb_explicit && llb) {
        ipod_touch_set_direct_llb(OBJECT(nms), llb, &error_fatal);
        warn_report_once("IT_DIRECT_LLB is deprecated; use direct-llb=PATH");
    }
    if (nms->direct_llb[0] && !nms->direct_iboot[0]) {
        error_setg(&error_fatal, "direct-llb requires direct-iboot");
    }
}

/*
 * Audio hardware that the machine did not model until now (the CS42L58 codec
 * and the AMC). Both are real parts on this board, but neither was mapped
 * before, so switching them on changes what every guest sees. 2.1.1 works
 * today and must keep working, so they default to on only for the 3.1.3
 * configuration (which is the one that has the audio bugs, and which is
 * selected by direct-iboot).
 * audio-hw=on forces them on (including under 2.1.1); off removes them.
 * Explicit machine options, including auto, override the legacy IT_AUDIO_HW
 * alias. Hardware topology cannot change after initialization.
 */
static bool ipod_touch_audio_hw_enabled(IPodTouchMachineState *nms)
{
    return nms->audio_hw == ON_OFF_AUTO_AUTO ? nms->direct_iboot[0] != 0
        : nms->audio_hw == ON_OFF_AUTO_ON;
}

static void ipod_touch_get_audio_hw(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    OnOffAuto value = IPOD_TOUCH_MACHINE(obj)->audio_hw;
    visit_type_OnOffAuto(v, name, &value, errp);
}

static void ipod_touch_set_audio_hw(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    if (nms->cpu) {
        error_setg(errp, "audio-hw must be set before the machine starts");
        return;
    }
    OnOffAuto value;
    if (visit_type_OnOffAuto(v, name, &value, errp)) {
        nms->audio_hw = value;
        nms->audio_hw_explicit = true;
    }
}

static void ipod_touch_audio_env_alias(IPodTouchMachineState *nms)
{
    const char *env = getenv("IT_AUDIO_HW");
    if (env && !nms->audio_hw_explicit) {
        /* Preserve the old alias's first-byte semantics, including empty=on. */
        nms->audio_hw = env[0] == '0' ? ON_OFF_AUTO_OFF : ON_OFF_AUTO_ON;
        warn_report_once("IT_AUDIO_HW is deprecated; use -M iPod-Touch,audio-hw=on|off|auto");
    }
}

/* Compatibility cp15 clock source for old research kernels. Stock 5F138 and
 * 7E18 now keep their native PMU RTC code; no clock trampoline is injected. */
static uint64_t host_gmt_seconds(CPUARMState *env, const ARMCPRegInfo *ri)
{
    return (uint64_t)(uint32_t)time(NULL);
}
static void host_gmt_write(CPUARMState *env, const ARMCPRegInfo *ri, uint64_t v)
{
    /* read-only clock source; ignore writes */
}

static const ARMCPRegInfo it2g_cp_reginfo_tcg[] = {
    IT2G_CPREG_DEF(REG0, 0, 0, 7, 6, 0, PL1_RW, 0),
    IT2G_CPREG_DEF(REG1, 0, 0, 15, 2, 4, PL1_RW, 0),
    IT2G_CPREG_DEF(REG1, 0, 0, 7, 14, 0, PL1_RW, 0),
    IT2G_CPREG_DEF(REG1, 0, 0, 7, 10, 0, PL1_RW, 0),
    IT2G_CPREG_DEF_QEMU_CALL,
    { .cp = 15, .name = "HOST_GMT_SECONDS",
      .opc0 = 0, .opc1 = 3, .crn = 15, .crm = 15, .opc2 = 1,
      .access = PL0_RW, .state = ARM_CP_STATE_AA32, .type = ARM_CP_IO,
      .readfn = host_gmt_seconds, .writefn = host_gmt_write },
};

static void ipod_touch_cpu_setup(MachineState *machine, MemoryRegion **sysmem, ARMCPU **cpu, AddressSpace **nsas)
{
    Object *cpuobj = object_new(machine->cpu_type);
    *cpu = ARM_CPU(cpuobj);
    CPUState *cs = CPU(*cpu);

    *sysmem = get_system_memory();

    object_property_set_link(cpuobj, "memory", OBJECT(*sysmem), &error_abort);

    object_property_set_bool(cpuobj, "has_el3", false, NULL);

    object_property_set_bool(cpuobj, "has_el2", false, NULL);

    object_property_set_bool(cpuobj, "realized", true, &error_fatal);

    *nsas = cpu_get_address_space(cs, ARMASIdx_NS);

    define_arm_cp_regs(*cpu, it2g_cp_reginfo_tcg);

    object_unref(cpuobj);
}

/*
 * Put the bootrom back at the reset vector.
 *
 * "vrom" is plain RAM at address 0, filled from the bootrom file once during
 * ipod_touch_memory_setup(). Address 0 is also the ARM exception vector page,
 * which the running guest happily writes over. So by the time anything asks for
 * a reset, the bytes at VROM_MEM_BASE are whatever iOS left there, and the CPU
 * restarts into that instead of the bootrom -- the machine never boots again.
 *
 * That is why the guest's post-fsck reboot went nowhere: the watchdog really
 * did call qemu_system_reset_request(), and the CPU really was reset, but it
 * resumed executing junk. A bare "system_reset" from the monitor against a
 * healthy machine failed exactly the same way, which is what showed this is a
 * machine-model problem rather than anything the guest did.
 *
 * Re-staging it on every reset costs one file read per boot and makes the reset
 * vector mean what it says.
 */
/* All boot-image callers share bounds/error handling: never jump into a
 * missing image or spill an oversized image into the next hardware window. */
static size_t ipod_touch_stage_boot_image(IPodTouchMachineState *nms,
                                         const char *path, hwaddr base,
                                         size_t capacity, Error **errp)
{
    g_autofree char *data = NULL;
    g_autoptr(GError) error = NULL;
    gsize size;

    if (!g_file_get_contents(path, &data, &size, &error)) {
        error_setg(errp, "Cannot read boot image '%s': %s", path, error->message);
        return 0;
    }
    if (!size || size > capacity) {
        error_setg(errp, "Boot image '%s' must contain 1..%zu bytes (got %zu)",
                   path, capacity, size);
        return 0;
    }
    if (address_space_write(nms->nsas, base, MEMTXATTRS_UNSPECIFIED,
                            data, size) != MEMTX_OK) {
        error_setg(errp, "Cannot stage boot image '%s' at 0x%" HWADDR_PRIx,
                   path, base);
        return 0;
    }
    return size;
}

static void ipod_touch_load_bootrom(IPodTouchMachineState *nms)
{
    ipod_touch_stage_boot_image(nms, nms->bootrom_path, VROM_MEM_BASE,
                                0x20000, &error_fatal);
}

/*
 * IT_DIRECT_IBOOT / IT_DIRECT_LLB: boot-chain substitution (explicitly
 * authorised for the 3.1.3 bring-up).
 *
 * iOS 3.0+ personalises the signed boot chain per-device, and the S5L8720
 * bootrom rejects the 7E18 LLB no matter how we forge the PKE check -- it
 * recomputes the image hash itself and drops to the DFU wait loop. So instead
 * of satisfying the bootrom we skip it, exactly as devos50 (iPod touch 1G, no
 * bootrom dump) and DJHartley's iEmu (-option-rom unencrypted iBoot) did: load
 * a *decrypted* iBoot straight into its own RAM region and enter it.
 *
 * The decrypted images are raw (they begin with the ARM vector table). Their
 * intended load address is the absolute value baked into the vector table at
 * offset 0x20: 7E18 iBoot -> 0x0ff00000 (== IBOOT_MEM_BASE), 7E18 LLB ->
 * 0x22000000 (== LLB region). We honour those.
 *
 * IT_DIRECT_LLB, if set, is staged first (it is what normally initialises DRAM
 * on real hardware); on QEMU DRAM is always-present RAM so iBoot alone is
 * usually enough, but this lets us reproduce the full LLB->iBoot handoff if
 * iBoot turns out to depend on state LLB leaves behind.
 */
#define LLB_LOAD_BASE 0x22000000

/*
 * IT_INJECT_DT: 3.1.3 device-tree bring-up (Option B3).
 *
 * The 7E18 iBoot loads and decrypts the kernelcache from HFS, but then fails to
 * load the device tree: its load_and_set_device_tree() (VA 0x0ff0f498) calls
 * image_load() (VA 0x0ff1998c) on the NOR 'dtre' image, which is rejected at
 * signature validation before any GID decrypt is attempted -- and a global
 * security-state change (forge/demote) to permit it breaks the kernelcache.
 *
 * Instead we hand iBoot an already-decrypted device tree. load_and_set_device_tree
 * (VA 0x0ff0f498) sets its DT-address global (g_dt_addr @ 0x0ff27560) to
 * 0x0BF00000 and its DT-size global (g_dt_size @ 0x0ff27564) to the enumerated
 * image size *before* the image_load() call; on image_load() success it returns
 * those to the caller, which then dt_deserialize()s the blob at g_dt_addr into
 * iBoot's node list (the list UpdateDeviceTree/AllocateMemoryRange walk).
 *
 * iBoot zeroes DRAM (both the insecure 0x08000000 and secure 0x0B000000 banks)
 * during early init, so a device tree dropped at 0x0BF00000 at reset is gone long
 * before the DT load. But the "llb"/SRAM region at 0x22000000 is NOT cleared (it
 * is where the SecureROM/LLB run on real hardware) and iBoot keeps it mapped. So
 * we stage the decrypted serialized device tree at 0x22000000 and rewrite the
 * failing image_load() call site (VA 0x0ff0f4da) into a 16-byte thunk that copies
 * the blob into place right when the DT is loaded (after the kernelcache
 * decompress that would otherwise clobber it):
 *
 *     movs r0,#0xbf ; lsls r0,r0,#20      ; r0 = 0x0BF00000 (dst = g_dt_addr)
 *     movs r1,#0x22 ; lsls r1,r1,#24      ; r1 = 0x22000000 (src = staging)
 *     ldr  r2,[r5]                        ; r2 = g_dt_size  (len, already set)
 *     blx  0x0ff1b474                     ; iBoot memcpy(dst, src, len)
 *     b    0x0ff0f4ea                     ; fall into the success/out-param path
 *
 * memcpy preserves r4/r5/r6/r8, so the function's success tail returns g_dt_addr
 * and g_dt_size to the caller exactly as a real image_load would. This touches
 * ONLY the dtre path; the kernelcache still validates and decrypts normally, and
 * a global security-state change (forge/demote) -- which breaks the kernelcache --
 * is avoided. Gated entirely behind IT_INJECT_DT; 2.1.1 is untouched.
 */
#define DT_STAGING_BASE     0x22000000   /* uncleared SRAM/"llb" region */
#define IBOOT_DT_LOAD_PATCH 0xf4da       /* VA offset of the `bl image_load` */

/*
 * IT_INJECT_LOGO: the boot Apple logo, which iBoot never manages to draw.
 *
 * The screen is black for the whole of iBoot's life on every 3.1.3 boot. It is
 * not a display problem: iBoot brings the panel up (pinot_init is clean), the
 * scanout base is programmed, and the backlight is high. It fails one step
 * earlier, and for exactly the same reason the device tree did.
 *
 * do_boot_ui() is inlined into main at 0x0ff00b4c and matches the published
 * iBoot source line for line:
 *
 *   0x0ff00b52  bl 0x0ff12b14   paint_set_bgcolor(0,0,0)
 *   0x0ff00b58  bl 0x0ff13298   paint_set_picture(0)
 *   0x0ff00b5c  ldr r0,='logo'  (the only 'logo' literal in the image)
 *   0x0ff00b5e  bl 0x0ff13498   paint_set_picture_for_tag(IMAGE_TYPE_LOGO)
 *   0x0ff00b62  bl 0x0ff12cf2   paint_update_image()
 *
 * paint_set_picture_for_tag is just paint_set_picture(image_find(tag)), and
 * image_find succeeds -- the 'logo' img3 is in the NOR image list, which is
 * what the "type logo offset 0x495c0" line in the boot log reports. The load
 * happens inside paint_set_picture:
 *
 *   0x0ff132f8  add r2,sp,#0x24         ; r2 = &address slot
 *   0x0ff132fc  add r3,sp,#0x20         ; r3 = &length slot
 *   0x0ff132fe  bl  0x0ff1998c          ; image_load(handle, 0, &addr, &len)
 *   0x0ff13302  cmp r0,#0
 *   0x0ff13304  bge 0x0ff13308          ; success
 *   0x0ff13306  b   0x0ff13452          ; failure: return, no picture set
 *
 * Measured over the gdbstub: that image_load returns **-1**. It is the same
 * image_load that rejects the dtre, failing personalised-signature validation
 * before any GID decrypt is attempted -- confirmed by the AES engine, which
 * performs exactly one GID operation in a whole boot ("7E18 kernelcache") and
 * never one for the logo. Unlike the device tree, a failed picture load is
 * silent: do_boot_ui simply paints its black background and boots on.
 *
 * So do for the logo what IT_INJECT_DT does for the device tree: hand iBoot the
 * already-decrypted image and skip the validation it cannot pass. The call site
 * is retargeted to a thunk that fills in the two out-parameters and returns 0.
 * On the success path iBoot itself checks the blob, so the staged file must be
 * a real decrypted iBootIm container ("iBootIm\0", 'lzss', 'argb' or 'grey');
 * iBoot decompresses and blits it. imgtools/extract_bootlogo.py produces one.
 *
 * Only this one call site is touched, so the kernelcache still validates and
 * decrypts normally -- unlike IT_FORGE_SIGCHECK, which is global and breaks it
 * ("Kernelcache image not valid" -> recovery mode).
 */
/*
 * Where the blob and the thunk can actually live, both learned the hard way:
 *
 * - iBoot ZEROES its own region above the loaded image during early init, so
 *   anything staged at reset into 0x0FF8xxxx is gone by the time do_boot_ui
 *   runs (measured: the thunk read back as 0x0000 halfwords and the CPU walked
 *   through them). The "llb"/SRAM region is the one place that survives, which
 *   is exactly why IT_INJECT_DT stages there; the device tree is still intact
 *   at 0x22000000 at logo time. Put the image there, clear of the DT.
 *
 * - The thunk cannot go there too: a Thumb BL only reaches +/-16 MB and
 *   0x22000000 is ~318 MB from the call site. It has to live inside the loaded
 *   iBoot image, which is not zeroed. do_recoverymode_ui (the 'recm' UI at
 *   0x0ff00cfe) is dead code in a normal boot -- nothing calls it unless iBoot
 *   enters recovery -- so the thunk goes there. If a future change ever needs
 *   recovery mode's UI with IT_INJECT_LOGO set, move this.
 */
#define LOGO_STAGING_BASE      0x22040000 /* llb/SRAM region, survives; DT is
                                           * ~35 KB at 0x22000000            */
#define LOGO_THUNK_BASE        0x0FF00D00 /* dead do_recoverymode_ui code    */
#define IBOOT_LOGO_LOAD_PATCH  0x132fe    /* VA offset of the `bl image_load` */

/*
 * Top of the "insecure" DRAM bank (0x08000000 + 0x3000000). Measured to be
 * zeroed by iBoot and then left untouched through kernel load, and it is inside
 * the kernel's static map (required, see ipod_touch_stage_ramdisk).
 */
#define IT_RAMDISK_DEFAULT_BASE 0x0A000000

/*
 * IT_RAMDISK: stage a filesystem image into guest DRAM so the 3.1.3 kernel can
 * use it as an md0 memory device (see /chosen/memory-map "RAMDisk" in the
 * injected device tree). XNU consumes the entry as
 *     mdevadd(-1, ml_static_ptovirt(paddr) >> 12, len >> 12, 0)
 * and ml_static_ptovirt() is only valid inside the kernel's static DRAM
 * mapping, so the image MUST live in DRAM -- it cannot be parked in a private
 * region outside it.
 *
 * The catch is that iBoot zeroes DRAM during early init, so anything staged at
 * machine-init time is wiped before the kernel runs. IT_RAMDISK_BASE lets us
 * probe where (if anywhere) a blob survives; the value is also what the DT's
 * RAMDisk entry must point at. Purely diagnostic/bring-up, gated on the env var.
 */
static void ipod_touch_ramdisk_stage_now(void *opaque)
{
    IPodTouchMachineState *nms = (IPodTouchMachineState *)opaque;
    const char *rd_path = getenv("IT_RAMDISK");
    const char *rd_base_s = getenv("IT_RAMDISK_BASE");
    uint8_t *rd_data = NULL;
    gsize rd_size;
    uint32_t rd_base = rd_base_s ? (uint32_t)strtoul(rd_base_s, NULL, 0)
                                 : IT_RAMDISK_DEFAULT_BASE;

    if (!g_file_get_contents(rd_path, (char **)&rd_data, &rd_size, NULL)) {
        fprintf(stderr, "[IT_RAMDISK] could not read '%s'\n", rd_path);
        return;
    }

    address_space_rw(nms->nsas, rd_base, MEMTXATTRS_UNSPECIFIED,
                     rd_data, rd_size, 1);
    g_free(rd_data);

    fprintf(stderr, "[IT_RAMDISK] staged '%s' (%llu bytes) at 0x%08x\n",
            rd_path, (unsigned long long)rd_size, rd_base);
}

static void ipod_touch_stage_ramdisk(IPodTouchMachineState *nms)
{
    const char *rd_delay_s = getenv("IT_RAMDISK_DELAY_MS");
    uint64_t delay_ms = rd_delay_s ? strtoull(rd_delay_s, NULL, 0) : 15000;
    QEMUTimer *t;

    if (!getenv("IT_RAMDISK")) {
        return;
    }

    /*
     * Staging cannot happen at machine-init time: iBoot zeroes DRAM during
     * early init, so the image would be wiped long before the kernel looks at
     * it (measured -- a blob written at reset reads back as zeroes, and the low
     * bank is reused by iBoot for img3 buffers). Defer the copy instead. The
     * usable window is wide: iBoot finishes zeroing in the first second or so,
     * and the kernel does not touch the RAMDisk range until it mounts root tens
     * of seconds later, so a one-shot timer is sufficient and needs no guest
     * patching. IT_RAMDISK_DELAY_MS tunes it.
     */
    t = timer_new_ms(QEMU_CLOCK_VIRTUAL, ipod_touch_ramdisk_stage_now, nms);
    timer_mod(t, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + delay_ms);
    fprintf(stderr, "[IT_RAMDISK] staging scheduled at T+%llu ms\n",
            (unsigned long long)delay_ms);
}

/*
 * IT_BOOT_ARGS: set the XNU kernel command line late in boot.
 *
 * 3.1.3 boots with an empty command line (7E18 iBoot heap-panics on any NOR
 * boot-args, so that path is unusable). Without it the kernel's code-signing
 * enforcement is on and AMFI rejects the ad-hoc/invalidly-signed decrypted
 * (Clutch) App-Store binaries at exec, so injected apps are discovered on the
 * home screen but never launch -- whereas stock, Apple-signed apps launch
 * normally. 2.1.1 does not need this: its own boot chain already carries
 * amfi_allow_any_signature=1, which is exactly what makes the same binaries run
 * there. This hook reproduces that on 3.1.3 without touching the 2.1.1 path.
 *
 * XNU reads the string live from boot_args->CommandLine (PE_boot_args returns
 * PE_state.bootArgs + 0x38 on every PE_parse_boot_argn call), and the AMFI kext
 * latches amfi_allow_any_signature once in its start(), tens of seconds into
 * boot. So writing the string into the boot_args CommandLine buffer on a short
 * timer -- after iBoot has built boot_args and handed off, before AMFI start --
 * lands in a wide window and needs no guest code patching.
 *
 * boot_args is built by iBoot at a fixed DRAM location for a given image; we
 * find it by signature (rev==1, virtBase==0xC0000000, physBase==0x08000000)
 * rather than hardcode the address, and overwrite CommandLine at +0x38.
 * IT_BOOT_ARGS_ADDR overrides the struct address; IT_BOOT_ARGS_DELAY_MS the
 * timer. Gated entirely on IT_BOOT_ARGS; 2.1.1 is untouched.
 */
#define BOOT_ARGS_CMDLINE_OFF   0x38
#define BOOT_ARGS_CMDLINE_LEN   256
#define BOOT_ARGS_STAGING_BASE  0x220fff00

/*
 * IT_AMFI_ALLOW_TASKPORT: let SpringBoard launch ad-hoc/invalidly-signed apps.
 *
 * amfi_allow_any_signature forgives code-page validation at exec, so a decrypted
 * (Clutch) app *runs* -- but it does not confer platform-binary status. When
 * SpringBoard spawns an app it then reaches for the child's task port to wire up
 * the app before resuming it (task_name_for_pid -> mac_proc_check_get_task_name);
 * AMFI's policy hook grants that only for a validly-signed binary, so for a
 * decrypted app SpringBoard logs "Failed to spawn ...: Unable to obtain a task
 * name port right ... (os/kern) failure" and kills it (exit 1). No AMFI
 * enforcement-disable boot-arg relaxes this path (amfi_allow_any_signature /
 * cs_enforcement_disable / amfi_get_out_of_my_way / amfi_unrestrict_task_for_pid
 * were all tried; the failure is identical), and get-task-allow on the target
 * does not help either.
 *
 * The single kernel choke points are the MAC framework's
 *   mac_proc_check_get_task_name (VA 0xc01ab2a0) and
 *   mac_proc_check_get_task      (VA 0xc01ab200)
 * which task_name_for_pid / task_for_pid consult; a zero return means "allowed".
 * We patch each prologue to `movs r0,#0 ; bx lr` so every task-port request is
 * granted, exactly as it is for a platform binary. This is the userspace-signing
 * analog of amfi_allow_any_signature and, like IT_BOOT_ARGS, it lives entirely in
 * the emulator -- no image edits, and it applies to any app however it arrived
 * (offline injection or over-the-wire install). The kernelcache is decrypted in
 * DRAM (VA->phys slide 0xB8000000: VA 0xC0000000 == phys 0x08000000), so the
 * code bytes are patchable from the host once the kernel image is present; we
 * ride the same early repeated timer as the boot-args write and only patch once
 * the expected prologue (push {r4-r7,lr}) is in place. Addresses/slide are env-
 * overridable for other builds. Gated on IT_AMFI_ALLOW_TASKPORT; 2.1.1 untouched.
 */

static bool it_amfi_patch_one(IPodTouchMachineState *nms, uint32_t va, uint32_t slide)
{
    static const uint8_t stub[4] = { 0x00, 0x20, 0x70, 0x47 }; /* movs r0,#0; bx lr */
    uint32_t pa = va - slide;
    uint8_t cur[4];

    address_space_rw(nms->nsas, pa, MEMTXATTRS_UNSPECIFIED, cur, sizeof(cur), 0);
    if (cur[0] == stub[0] && cur[1] == stub[1] &&
        cur[2] == stub[2] && cur[3] == stub[3]) {
        return true; /* already patched */
    }
    /* Expected Thumb prologue "push {r4,r5,r6,r7,lr}" == 0xB5F0. Only patch the
     * real function, never mid-decrypt garbage. */
    if (!(cur[0] == 0xF0 && cur[1] == 0xB5)) {
        return false;
    }
    address_space_rw(nms->nsas, pa, MEMTXATTRS_UNSPECIFIED, (void *)stub,
                     sizeof(stub), 1);
    return true;
}

static void ipod_touch_amfi_patch_now(IPodTouchMachineState *nms)
{
    if (!getenv("IT_AMFI_ALLOW_TASKPORT") || nms->amfi_patched) {
        return;
    }
    const char *slide_s = getenv("IT_AMFI_HOOK_SLIDE");
    const char *gtn_s = getenv("IT_AMFI_GET_TASK_NAME_VA");
    const char *gt_s = getenv("IT_AMFI_GET_TASK_VA");
    const ITFirmwareDesc *fw = it_firmware_loaded();
    /* Other builds require all three explicit research overrides. Never use
     * 7E18 function addresses just because a different kernel has a push. */
    if ((!fw || !fw->amfi_get_task_va) && !(slide_s && gtn_s && gt_s)) {
        return;
    }
    uint32_t slide = slide_s ? strtoul(slide_s, NULL, 0) : fw->amfi_slide;
    uint32_t gtn = gtn_s ? strtoul(gtn_s, NULL, 0) : fw->amfi_get_task_name_va;
    uint32_t gt = gt_s ? strtoul(gt_s, NULL, 0) : fw->amfi_get_task_va;

    if (it_amfi_patch_one(nms, gtn, slide) &&
        it_amfi_patch_one(nms, gt, slide)) {
        nms->amfi_patched = true;
        fprintf(stderr, "[IT_AMFI_ALLOW_TASKPORT] patched mac_proc_check_get_task"
                "{,_name} (0x%08x, 0x%08x) to allow\n", gt, gtn);
    }
}

static const char *ipod_touch_requested_boot_args(IPodTouchMachineState *nms)
{
    /* Explicit machine options win over the legacy environment fallback,
     * for both the early handoff and subsequent AMFI argument refreshes. */
    if (nms->boot_args_explicit && !nms->boot_args[0]) return NULL;
    return nms->boot_args[0] ? nms->boot_args : getenv("IT_BOOT_ARGS");
}

static void ipod_touch_set_boot_args_now(void *opaque)
{
    IPodTouchMachineState *nms = (IPodTouchMachineState *)opaque;
    const char *args = ipod_touch_requested_boot_args(nms);
    const char *addr_s = getenv("IT_BOOT_ARGS_ADDR");
    uint32_t ba = 0;
    uint8_t buf[BOOT_ARGS_CMDLINE_LEN];
    size_t n;

    /* AMFI task-port patch rides this same early repeated timer. */
    ipod_touch_amfi_patch_now(nms);

    if (!args) {
        goto rearm;
    }

    if (addr_s) {
        ba = (uint32_t)strtoul(addr_s, NULL, 0);
    } else if (nms->boot_args_addr) {
        /* Found on an earlier tick; boot_args does not move once the kernel
         * has built it. Re-verify the signature so a reboot (which rebuilds
         * DRAM) falls back to a fresh scan instead of scribbling blindly. */
        uint32_t sig[3] = { 0, 0, 0 };
        address_space_rw(nms->nsas, nms->boot_args_addr, MEMTXATTRS_UNSPECIFIED,
                         (uint8_t *)sig, sizeof(sig), 0);
        if ((sig[0] & 0xFFFF) == 1 && sig[1] == 0xC0000000 &&
            sig[2] == 0x08000000) {
            ba = nms->boot_args_addr;
        } else {
            nms->boot_args_addr = 0;
        }
    }
    if (!addr_s && !ba) {
        /*
         * Scan DRAM for the boot_args signature -- in bulk. This used to be
         * three 4-byte address_space_rw calls per word over 8 MB, ~2 million
         * MMIO-path round trips, 30-45 ms on the main loop with the BQL held,
         * EVERY 250 ms tick of this timer, for the whole repeat window. Those
         * were the recurring whole-process freezes that put pages of silence
         * into the middle of every sound the guest played while the window was
         * open (see hw/arm/ipod_touch_i2s.c). Read 64 KB at a time instead and
         * scan in host memory; the windows overlap by 8 bytes so a signature
         * straddling a boundary is still seen.
         */
        uint8_t window[0x10000];
        uint32_t base;
        for (base = 0x08000000; base < 0x08000000 + 0x00800000 && !ba;
             base += sizeof(window) - 8) {
            uint32_t off;
            address_space_rw(nms->nsas, base, MEMTXATTRS_UNSPECIFIED,
                             window, sizeof(window), 0);
            for (off = 0; off + 12 <= sizeof(window); off += 4) {
                if ((ldl_le_p(window + off) & 0xFFFF) == 1 &&
                    ldl_le_p(window + off + 4) == 0xC0000000 &&
                    ldl_le_p(window + off + 8) == 0x08000000) {
                    ba = base + off;
                    nms->boot_args_addr = ba;
                    break;
                }
            }
        }
        if (!ba) {
            /*
             * Not found YET. This timer can easily fire before the kernel has
             * built boot_args, so a bare return here gives up permanently and
             * the command line is never set -- which is exactly the failure it
             * looks least like: the guest boots, AMFI reads a default-empty
             * amfi_allow_any_signature, and the device wedges later with
             * "verify_code_directory server is dead" from a re-signed binary.
             * Re-arm and look again; the scan is the whole point of the repeat
             * window. Complain once so a genuinely wrong image is still
             * diagnosable.
             */
            if (!nms->boot_args_scan_failed) {
                nms->boot_args_scan_failed = true;
                fprintf(stderr, "[IT_BOOT_ARGS] boot_args not found by "
                        "signature yet; retrying (set IT_BOOT_ARGS_ADDR to "
                        "skip the scan)\n");
            }
            goto rearm;
        }
    }

    memset(buf, 0, sizeof(buf));
    n = strlen(args);
    if (n >= sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    memcpy(buf, args, n);
    address_space_rw(nms->nsas, ba + BOOT_ARGS_CMDLINE_OFF,
                     MEMTXATTRS_UNSPECIFIED, buf, sizeof(buf), 1);

    /*
     * The kernel latches boot-args early: consumers like AMFI read
     * amfi_allow_any_signature once in their init, which runs a few seconds
     * into boot -- and under -cpu max the virtual clock advances fast, so a
     * single late write can miss it. Re-arm across an early window (guided by
     * boot-args-repeat / boot-args-interval-ms) so the string is present
     * before any consumer reads it and stays present afterwards.
     */
    if (nms->boot_args_writes == 0) {
        fprintf(stderr, "[IT_BOOT_ARGS] boot_args @ 0x%08x; CommandLine = [ %s ]\n",
                ba, args);
    }
    nms->boot_args_writes++;

rearm:
    {
        /* Keep re-arming while there is still work: the boot-args string needs
         * to be re-asserted a few times, and the AMFI patch waits for the
         * kernelcache to appear in DRAM. */
        bool amfi_pending = getenv("IT_AMFI_ALLOW_TASKPORT") && !nms->amfi_patched;
        if (nms->boot_args_writes < nms->boot_args_repeat || amfi_pending) {
            timer_mod(nms->boot_args_timer,
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + nms->boot_args_interval_ms);
        }
    }
}

static void ipod_touch_stage_boot_args(IPodTouchMachineState *nms)
{
    uint32_t delay_ms = nms->boot_args_delay_ms;

    if (!ipod_touch_requested_boot_args(nms) && !getenv("IT_AMFI_ALLOW_TASKPORT")) {
        return;
    }

    nms->boot_args_writes = 0;
    nms->amfi_patched = false;
    nms->boot_args_scan_failed = false;
    if (!nms->boot_args_timer) {
        nms->boot_args_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                           ipod_touch_set_boot_args_now, nms);
    }
    timer_mod(nms->boot_args_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + delay_ms);
    fprintf(stderr, "[IT_BOOT_ARGS] first write scheduled at T+%llu ms\n",
            (unsigned long long)delay_ms);
}

static void ipod_touch_inject_device_tree(IPodTouchMachineState *nms)
{
    const char *dt_path = getenv("IT_INJECT_DT");
    uint8_t *dt_data = NULL;
    gsize dt_size;
    /* 16-byte thunk (see comment above): memcpy(0x0BF00000, 0x22000000, g_dt_size)
     * then branch to the success path. */
    static const uint8_t patch[16] = {
        0xbf, 0x20, 0x00, 0x05,   /* movs r0,#0xbf ; lsls r0,r0,#20  */
        0x22, 0x21, 0x09, 0x06,   /* movs r1,#0x22 ; lsls r1,r1,#24  */
        0x2a, 0x68,               /* ldr  r2,[r5]                    */
        0x0b, 0xf0, 0xc6, 0xef,   /* blx  0x0ff1b474 (memcpy)        */
        0xff, 0xe7,               /* b    0x0ff0f4ea                 */
    };

    if (!dt_path) {
        return;
    }

    if (!g_file_get_contents(dt_path, (char **)&dt_data, &dt_size, NULL)) {
        fprintf(stderr, "[IT_INJECT_DT] could not read '%s'\n", dt_path);
        return;
    }

    address_space_rw(nms->nsas, DT_STAGING_BASE, MEMTXATTRS_UNSPECIFIED,
                     dt_data, dt_size, 1);
    g_free(dt_data);

    address_space_rw(nms->nsas, IBOOT_MEM_BASE + IBOOT_DT_LOAD_PATCH,
                     MEMTXATTRS_UNSPECIFIED, (uint8_t *)patch, sizeof(patch), 1);

    fprintf(stderr, "[IT_INJECT_DT] staged device tree '%s' (%llu bytes) at "
            "0x%08x and patched iBoot dtre image_load\n",
            dt_path, (unsigned long long)dt_size, DT_STAGING_BASE);
}

/* Encode a Thumb-2 BL from `src` to `dst` into the two halfwords it occupies. */
static void thumb_bl(uint32_t src, uint32_t dst, uint8_t out[4])
{
    int32_t off = (int32_t)(dst - (src + 4));
    uint32_t imm = ((uint32_t)off) >> 1;
    uint32_t s = (off < 0) ? 1 : 0;
    uint32_t i1 = (imm >> 22) & 1, i2 = (imm >> 21) & 1;
    uint32_t j1 = (~i1 & 1) ^ s, j2 = (~i2 & 1) ^ s;
    uint32_t hw1 = 0xF000 | (s << 10) | ((imm >> 11) & 0x3FF);
    uint32_t hw2 = 0xD000 | (j1 << 13) | (j2 << 11) | (imm & 0x7FF);

    out[0] = hw1 & 0xFF; out[1] = hw1 >> 8;
    out[2] = hw2 & 0xFF; out[3] = hw2 >> 8;
}

static void ipod_touch_inject_boot_logo(IPodTouchMachineState *nms)
{
    const char *logo_path = getenv("IT_INJECT_LOGO");
    uint8_t *logo = NULL;
    gsize logo_size;
    uint8_t bl[4];
    /*
     * The thunk. At the call site r2 = &address slot and r3 = &length slot, so
     * it only has to fill both in and report success:
     *
     *   ldr r0,[pc,#8] ; str r0,[r2]   *addr = LOGO_STAGING_BASE
     *   ldr r0,[pc,#8] ; str r0,[r3]   *len  = <size>
     *   movs r0,#0     ; bx lr         return 0, iBoot takes the success path
     */
    uint8_t thunk[20] = {
        0x02, 0x48,               /* ldr  r0,[pc,#8]  */
        0x10, 0x60,               /* str  r0,[r2]     */
        0x02, 0x48,               /* ldr  r0,[pc,#8]  */
        0x18, 0x60,               /* str  r0,[r3]     */
        0x00, 0x20,               /* movs r0,#0       */
        0x70, 0x47,               /* bx   lr          */
        0x00, 0x00, 0x00, 0x00,   /* .word staging base */
        0x00, 0x00, 0x00, 0x00,   /* .word length       */
    };

    if (!logo_path) {
        return;
    }

    if (!g_file_get_contents(logo_path, (char **)&logo, &logo_size, NULL)) {
        fprintf(stderr, "[IT_INJECT_LOGO] could not read '%s'\n", logo_path);
        return;
    }

    if (logo_size < 0x14 || logo_size > BOOT_ARGS_STAGING_BASE - LOGO_STAGING_BASE ||
        memcmp(logo, "iBootIm\0", 8) != 0) {
        fprintf(stderr, "[IT_INJECT_LOGO] '%s' is not a decrypted iBootIm "
                "container; iBoot would reject it. Use "
                "imgtools/extract_bootlogo.py\n", logo_path);
        g_free(logo);
        return;
    }

    address_space_rw(nms->nsas, LOGO_STAGING_BASE, MEMTXATTRS_UNSPECIFIED,
                     logo, logo_size, 1);

    stl_le_p(thunk + 12, LOGO_STAGING_BASE);
    stl_le_p(thunk + 16, (uint32_t)logo_size);
    address_space_rw(nms->nsas, LOGO_THUNK_BASE, MEMTXATTRS_UNSPECIFIED,
                     thunk, sizeof(thunk), 1);

    thumb_bl(IBOOT_MEM_BASE + IBOOT_LOGO_LOAD_PATCH, LOGO_THUNK_BASE, bl);
    address_space_rw(nms->nsas, IBOOT_MEM_BASE + IBOOT_LOGO_LOAD_PATCH,
                     MEMTXATTRS_UNSPECIFIED, bl, sizeof(bl), 1);

    fprintf(stderr, "[IT_INJECT_LOGO] staged boot logo '%s' (%llu bytes) at "
            "0x%08x and retargeted iBoot's logo image_load to 0x%08x\n",
            logo_path, (unsigned long long)logo_size, LOGO_STAGING_BASE,
            LOGO_THUNK_BASE);
    g_free(logo);
}

static void ipod_touch_inject_boot_args(IPodTouchMachineState *nms)
{
    const char *args = ipod_touch_requested_boot_args(nms);
    /* Release 7E18 iBoot deliberately ignores NVRAM boot-args. Redirect its
     * normal-boot empty-string literal, before it constructs XNU's arguments.
     * The late AMFI timer is too late for PE_init_platform's verbose flag. */
    static const uint8_t expected[] = {
        0x2c,0x4b,0x9b,0x46,0x1b,0x68,0x00,0x2b,0x03,0xd1,0x2a,0x48,
        0x06,0x1c,0x01,0x90,0x02,0xe0,0x29,0x4e,0x28,0x49,0x01,0x91
    };
    uint8_t code[sizeof(expected)], literal[4], command[BOOT_ARGS_CMDLINE_LEN] = {0};
    const hwaddr staging = BOOT_ARGS_STAGING_BASE; /* final 256 bytes of LLB SRAM */

    if (!args) return;
    address_space_read(nms->nsas, IBOOT_MEM_BASE + 0x11a72,
                       MEMTXATTRS_UNSPECIFIED, code, sizeof(code));
    address_space_read(nms->nsas, IBOOT_MEM_BASE + 0x11b28,
                       MEMTXATTRS_UNSPECIFIED, literal, sizeof(literal));
    if (memcmp(code, expected, sizeof(code)) || ldl_le_p(literal) != 0x0ff1dba0) {
        fprintf(stderr, "[IT_BOOT_ARGS] unknown iBoot; early argument injection skipped\n");
        return;
    }
    g_strlcpy((char *)command, args, sizeof(command));
    address_space_write(nms->nsas, staging, MEMTXATTRS_UNSPECIFIED, command, sizeof(command));
    stl_le_p(literal, staging);
    address_space_write(nms->nsas, IBOOT_MEM_BASE + 0x11b28,
                        MEMTXATTRS_UNSPECIFIED, literal, sizeof(literal));
    fprintf(stderr, "[IT_BOOT_ARGS] staged early 7E18 command line\n");
}

static void ipod_touch_load_direct_boot(IPodTouchMachineState *nms)
{
    const char *iboot_path = nms->direct_iboot[0] ? nms->direct_iboot : NULL;
    const char *llb_path = nms->direct_llb[0] ? nms->direct_llb : NULL;
    size_t fsize;

    if (llb_path) {
        fsize = ipod_touch_stage_boot_image(nms, llb_path, LLB_LOAD_BASE,
                                            SRAM1_MEM_BASE - LLB_LOAD_BASE,
                                            &error_fatal);
        fprintf(stderr, "[IT_DIRECT] staged LLB '%s' (%llu bytes) at 0x%08x\n",
                llb_path, (unsigned long long)fsize, LLB_LOAD_BASE);
    }

    if (iboot_path) {
        fsize = ipod_touch_stage_boot_image(nms, iboot_path, IBOOT_MEM_BASE,
                                            0x100000, &error_fatal);
        fprintf(stderr, "[IT_DIRECT] staged iBoot '%s' (%llu bytes) at 0x%08x\n",
                iboot_path, (unsigned long long)fsize, IBOOT_MEM_BASE);
        /*
         * iBoot's miu_init reads SYSIC[0x44] bits[31:24] as the boot security
         * epoch and panics ("Epoch Mismatch") unless it equals the epoch baked
         * into the image (4 for the S5L8720 / iPod touch 2G). On real hardware
         * that top byte is a read-only fused value the SecureROM never writes;
         * the low bits are the POWER_ID power-control scratch. We skip the ROM,
         * so the SYSIC model synthesises the epoch top byte on read whenever
         * direct-iboot is configured -- see ipod_touch_sysic_read(). Nothing to do
         * here.
         */

        /* Hand iBoot a pre-decrypted device tree (3.1.3 bring-up). Must run
         * after the iBoot image is staged so the code patch lands on top of it. */
        ipod_touch_inject_device_tree(nms);
        ipod_touch_inject_boot_logo(nms);
        ipod_touch_inject_boot_args(nms);
        ipod_touch_stage_ramdisk(nms);
        ipod_touch_stage_boot_args(nms);
    }
}

static void ipod_touch_cpu_reset(void *opaque)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE((MachineState *)opaque);
    ARMCPU *cpu = nms->cpu;
    CPUState *cs = CPU(cpu);

    it_firmware_reset();
    ipod_agent_reset(nms->agent);
    gles_host_reset();
    cpu_reset(cs);
    ipod_touch_load_bootrom(nms);

    if (nms->direct_iboot[0]) {
        /* Boot-chain substitution: enter the decrypted iBoot directly, skipping
         * the bootrom + LLB signature/personalisation checks. */
        ipod_touch_load_direct_boot(nms);
        cpu_set_pc(CPU(cpu), nms->direct_llb[0] ? LLB_LOAD_BASE : IBOOT_MEM_BASE);
        return;
    }

    //env->regs[0] = nms->kbootargs_pa;
    //cpu_set_pc(CPU(cpu), 0xc00607ec);
    cpu_set_pc(CPU(cpu), VROM_MEM_BASE);
    //env->regs[0] = 0x9000000;
    //cpu_set_pc(CPU(cpu), LLB_BASE + 0x100);
    //cpu_set_pc(CPU(cpu), VROM_MEM_BASE);
}

static void ipod_touch_memory_setup(MachineState *machine, MemoryRegion *sysmem, AddressSpace *nsas)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(machine);

    MemoryRegion *insecure = allocate_ram(sysmem, "insecure_ram",
                                          INSECURE_RAM_MEM_BASE, 0x3000000);
    MemoryRegion *secure = allocate_ram(sysmem, "secure_ram",
                                        SECURE_RAM_MEM_BASE, 0x4B04000);
    install_ram_watch(sysmem, insecure, INSECURE_RAM_MEM_BASE, 0x3000000);
    install_ram_watch(sysmem, secure, SECURE_RAM_MEM_BASE, 0x4B04000);
    allocate_ram(sysmem, "iboot", IBOOT_MEM_BASE, 0x100000);
    MemoryRegion *llb = allocate_ram(sysmem, "llb", 0x22000000, 0x100000);
    install_aperture_watch(sysmem, llb, 0x22000000, AMC_BUF_SIZE);
    allocate_ram(sysmem, "sram1", SRAM1_MEM_BASE, 0x100000);
    allocate_ram(sysmem, "framebuffer", FRAMEBUFFER_MEM_BASE, 0x400000);
    allocate_ram(sysmem, "edgeic", EDGEIC_MEM_BASE, 0x1000);
    sysbus_create_simple("ipodtouch.swi", SWI_MEM_BASE, NULL);
    if (!nms->h264_decode) {
        MemoryRegion *h264 = allocate_ram(sysmem, "h264", H264_MEM_BASE, 0x4000);
        install_ram_watch(sysmem, h264, H264_MEM_BASE, 0x4000);
    }

    /* The bootrom itself is (re)staged by ipod_touch_load_bootrom(), which also
     * runs on every reset -- see the note there. */
    allocate_ram(sysmem, "vrom", 0x0, 0x20000);
    ipod_touch_load_bootrom(nms);
}

static char *ipod_touch_get_bootrom_path(Object *obj, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    return g_strdup(nms->bootrom_path);
}

static void ipod_touch_set_bootrom_path(Object *obj, const char *value, Error **errp)
{
    gboolean bootrom_exists = g_file_test(value, G_FILE_TEST_EXISTS);
    if(!bootrom_exists) {
        error_report("bootrom at path \"%s\" must exist", value);
        exit(1);
    }
    
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    g_strlcpy(nms->bootrom_path, value, sizeof(nms->bootrom_path));
}

static char *ipod_touch_get_nor_path(Object *obj, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    return g_strdup(nms->nor_path);
}

static void ipod_touch_set_nor_path(Object *obj, const char *value, Error **errp)
{
    gboolean nor_exists = g_file_test(value, G_FILE_TEST_EXISTS);
    if(!nor_exists) {
        error_report("NOR at path \"%s\" must exist", value);
        exit(1);
    }

    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    g_strlcpy(nms->nor_path, value, sizeof(nms->nor_path));
}

static char *ipod_touch_get_nor_rw(Object *obj, Error **errp)
{
    return g_strdup(IPOD_TOUCH_MACHINE(obj)->nor_rw_path);
}

static void ipod_touch_set_nor_rw(Object *obj, const char *value, Error **errp)
{
    IPodTouchMachineState *s = IPOD_TOUCH_MACHINE(obj);
    if (s->cpu) {
        error_setg(errp, "nor-rw must be configured before the machine starts");
    } else if (strlen(value) >= sizeof(s->nor_rw_path)) {
        error_setg(errp, "nor-rw path is too long");
    } else {
        g_strlcpy(s->nor_rw_path, value, sizeof(s->nor_rw_path));
    }
}

static bool ipod_touch_get_wifi(Object *obj, Error **errp)
{
    return IPOD_TOUCH_MACHINE(obj)->wifi;
}

static void ipod_touch_set_wifi(Object *obj, bool value, Error **errp)
{
    IPOD_TOUCH_MACHINE(obj)->wifi = value;
}

static char *ipod_touch_get_boot_args(Object *obj, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    return g_strdup(nms->boot_args);
}

static void ipod_touch_set_boot_args(Object *obj, const char *value, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    if (nms->cpu) {
        error_setg(errp, "boot-args must be set before the machine starts");
        return;
    }
    if (strlen(value) >= BOOT_ARGS_CMDLINE_LEN) {
        error_setg(errp, "boot-args exceeds the kernel's 255-byte command line");
        return;
    }
    g_strlcpy(nms->boot_args, value, sizeof(nms->boot_args));
    nms->boot_args_explicit = true;
}

static char *ipod_touch_get_nand_path(Object *obj, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    return g_strdup(nms->nand_path);
}

static void ipod_touch_set_nand_path(Object *obj, const char *value, Error **errp)
{
    /*
     * Either a directory of .page files or a packed image
     * (imgtools/pack_nand.py). The packed form is one mapping instead of an
     * fopen/fread/fclose per 4 KB the guest reads, which is what makes this
     * usable on a phone.
     */
    if (!g_file_test(value, G_FILE_TEST_IS_DIR) &&
        !g_file_test(value, G_FILE_TEST_IS_REGULAR)) {
        error_report("no NAND at \"%s\": expected a page directory or a "
                     "packed image", value);
        exit(1);
    }
    
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    g_strlcpy(nms->nand_path, value, sizeof(nms->nand_path));
}

static char *ipod_touch_get_nand_overlay(Object *obj, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    return g_strdup(nms->nand_overlay);
}

static void ipod_touch_set_nand_overlay(Object *obj, const char *value, Error **errp)
{
    /* Create the overlay directory (and its cs0..cs3 subdirs) on demand so the
     * user only has to name a path. Writes land here; the base 'nand' image is
     * never modified. */
    if (g_mkdir_with_parents(value, 0755) != 0 &&
        !g_file_test(value, G_FILE_TEST_IS_DIR)) {
        error_report("NAND overlay at path \"%s\" could not be created", value);
        exit(1);
    }

    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    g_strlcpy(nms->nand_overlay, value, sizeof(nms->nand_overlay));
}

static char *ipod_touch_get_usb_tcp_addr(Object *obj, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    return g_strdup(nms->usb_tcp_addr);
}

static void ipod_touch_set_usb_tcp_addr(Object *obj, const char *value, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    g_strlcpy(nms->usb_tcp_addr, value, sizeof(nms->usb_tcp_addr));
}

static bool ipod_touch_get_usb_attached(Object *obj, Error **errp)
{
    return IPOD_TOUCH_MACHINE(obj)->usb_attached;
}

static void ipod_touch_set_usb_attached(Object *obj, bool value, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    nms->usb_attached = value;
    if (nms->pmu_state) {
        pcf50633_set_usb_cable(nms->pmu_state, value);
    }
}

static bool ipod_touch_get_usb_patch_mux_gate(Object *obj, Error **errp)
{
    return IPOD_TOUCH_MACHINE(obj)->usb_patch_mux_gate;
}

static void ipod_touch_set_usb_patch_mux_gate(Object *obj, bool value, Error **errp)
{
    IPOD_TOUCH_MACHINE(obj)->usb_patch_mux_gate = value;
}

static bool ipod_touch_get_mbx_irq(Object *obj, Error **errp)
{
    return IPOD_TOUCH_MACHINE(obj)->mbx_irq;
}

static void ipod_touch_set_mbx_irq(Object *obj, bool value, Error **errp)
{
    IPOD_TOUCH_MACHINE(obj)->mbx_irq = value;
}

/*
 * Accelerometer controls, forwarded to the LIS302DL. These live on the machine
 * (a stable /machine QOM path) so a host can drive rotation/shake over QMP:
 *   qom-set path=/machine property=accel-orientation value=3   (0-6, UIDeviceOrientation)
 *   qom-set path=/machine property=accel-shake value=true
 *   qom-set path=/machine property=accel-x value=64
 * The i2c device itself cannot be given a fixed path (it is parented to the bus).
 */
static void ipod_touch_get_battery_adc(Object *obj, Visitor *v, const char *name,
                                      void *opaque, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    if (nms->pmu_state) pcf50633_update_battery(nms->pmu_state);
    int64_t value = nms->pmu_state ? nms->pmu_state->adc_values[4] : nms->battery_adc;
    visit_type_int(v, name, &value, errp);
}

static void ipod_touch_set_battery_adc(Object *obj, Visitor *v, const char *name,
                                      void *opaque, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    int64_t value;
    if (!visit_type_int(v, name, &value, errp)) {
        return;
    }
    if (value < 0 || value > 1023) {
        error_setg(errp, "battery-adc must be between 0 and 1023");
        return;
    }
    nms->battery_adc = value;
    if (nms->pmu_state) {
        pcf50633_set_battery_adc(nms->pmu_state, value);
    }
}

static void ipod_touch_get_battery_level(Object *obj, Visitor *v, const char *name,
                                        void *opaque, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    if (nms->pmu_state) pcf50633_update_battery(nms->pmu_state);
    unsigned counts = nms->pmu_state ? nms->pmu_state->adc_values[4] : nms->battery_adc;
    int64_t value = pcf50633_level_for_adc(counts);
    visit_type_int(v, name, &value, errp);
}

static void ipod_touch_set_battery_level(Object *obj, Visitor *v, const char *name,
                                        void *opaque, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    int64_t value;
    if (!visit_type_int(v, name, &value, errp)) {
        return;
    }
    if (value < 0 || value > 100) {
        error_setg(errp, "battery-level must be between 0 and 100");
        return;
    }
    nms->battery_adc = pcf50633_adc_for_level(value);
    if (nms->pmu_state) {
        pcf50633_set_battery_level(nms->pmu_state, value);
    }
}

static void ipod_touch_get_battery_drain(Object *obj, Visitor *v, const char *name,
                                        void *opaque, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    double value = nms->pmu_state ? nms->pmu_state->drain_rate : nms->battery_drain;
    visit_type_number(v, name, &value, errp);
}

static void ipod_touch_set_battery_drain(Object *obj, Visitor *v, const char *name,
                                        void *opaque, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    double value;
    if (!visit_type_number(v, name, &value, errp)) return;
    if (!isfinite(value) || value < 0 || value > 100) {
        error_setg(errp, "battery-drain must be between 0 and 100 percent per minute");
        return;
    }
    nms->battery_drain = value;
    if (nms->pmu_state) pcf50633_set_battery_drain(nms->pmu_state, value);
}

static char *ipod_touch_get_battery_charging(Object *obj, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    unsigned mode = nms->pmu_state ? nms->pmu_state->charging_mode : nms->battery_charging;
    return g_strdup(mode == 1 ? "on" : mode == 2 ? "off" : "auto");
}

static void ipod_touch_set_battery_charging(Object *obj, const char *value, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    unsigned mode;
    if (!strcmp(value, "auto")) {
        mode = 0;
    } else if (!strcmp(value, "on")) {
        mode = 1;
    } else if (!strcmp(value, "off")) {
        mode = 2;
    } else {
        error_setg(errp, "battery-charging must be auto, on or off");
        return;
    }
    nms->battery_charging = mode;
    if (nms->pmu_state) {
        pcf50633_set_charging_mode(nms->pmu_state, mode);
    }
}

static double ipod_touch_accel_angle(IPodTouchMachineState *nms, bool pitch)
{
    if (nms->lis302dl_state) {
        return (pitch ? nms->lis302dl_state->pitch_mdeg :
                        nms->lis302dl_state->roll_mdeg) / 1000.0;
    }
    return pitch ? nms->accel_pitch : nms->accel_roll;
}

static bool ipod_touch_accel_flat(IPodTouchMachineState *nms)
{
    return nms->lis302dl_state ? nms->lis302dl_state->flat_pose : nms->accel_flat;
}

static void ipod_touch_get_accel_angle(Object *obj, Visitor *v, const char *name,
                                      void *opaque, Error **errp)
{
    double value = ipod_touch_accel_angle(IPOD_TOUCH_MACHINE(obj), !strcmp(name, "accel-pitch"));
    visit_type_number(v, name, &value, errp);
}

static void ipod_touch_set_accel_angle(Object *obj, Visitor *v, const char *name,
                                      void *opaque, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    double value;
    if (!visit_type_number(v, name, &value, errp)) return;
    if (!isfinite(value) || value < -180 || value > 180) {
        error_setg(errp, "%s must be finite and between -180 and 180 degrees", name);
        return;
    }
    nms->accel_pitch = ipod_touch_accel_angle(nms, true);
    nms->accel_roll = ipod_touch_accel_angle(nms, false);
    nms->accel_flat = ipod_touch_accel_flat(nms);
    if (!strcmp(name, "accel-pitch")) nms->accel_pitch = value;
    else nms->accel_roll = value;
    if (nms->lis302dl_state) lis302dl_apply_attitude(nms->lis302dl_state,
        nms->accel_pitch, nms->accel_roll, nms->accel_flat);
}

static char *ipod_touch_get_accel_pose(Object *obj, Error **errp)
{
    return g_strdup(ipod_touch_accel_flat(IPOD_TOUCH_MACHINE(obj)) ? "flat" : "upright");
}

static void ipod_touch_set_accel_pose(Object *obj, const char *value, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    if (strcmp(value, "flat") && strcmp(value, "upright")) {
        error_setg(errp, "accel-pose must be upright or flat");
        return;
    }
    nms->accel_pitch = ipod_touch_accel_angle(nms, true);
    nms->accel_roll = ipod_touch_accel_angle(nms, false);
    nms->accel_flat = !strcmp(value, "flat");
    if (nms->lis302dl_state) lis302dl_apply_attitude(nms->lis302dl_state,
        nms->accel_pitch, nms->accel_roll, nms->accel_flat);
}

static void ipod_touch_get_accel_rate(Object *obj, Visitor *v, const char *name,
                                     void *opaque, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    int64_t value = nms->lis302dl_state ? nms->lis302dl_state->rate_hz : nms->accel_rate_hz;
    visit_type_int(v, name, &value, errp);
}

static void ipod_touch_set_accel_rate(Object *obj, Visitor *v, const char *name,
                                     void *opaque, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    int64_t value;
    if (!visit_type_int(v, name, &value, errp)) return;
    if (value < 0 || value > 400) {
        error_setg(errp, "accel-rate-hz must be 0 (automatic) or 1..400");
        return;
    }
    nms->accel_rate_hz = value;
    if (nms->lis302dl_state) nms->lis302dl_state->rate_hz = value;
}

static void ipod_touch_get_accel_orientation(Object *obj, Visitor *v, const char *name,
                                             void *opaque, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    int64_t val = nms->lis302dl_state ? nms->lis302dl_state->orientation : 0;
    visit_type_int(v, name, &val, errp);
}

static void ipod_touch_set_accel_orientation(Object *obj, Visitor *v, const char *name,
                                             void *opaque, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    int64_t val;
    if (!visit_type_int(v, name, &val, errp)) {
        return;
    }
    if (nms->lis302dl_state) {
        lis302dl_apply_orientation(nms->lis302dl_state, (uint32_t)val);
    }
}

static void ipod_touch_get_accel_axis(Object *obj, Visitor *v, const char *name,
                                      void *opaque, Error **errp)
{
    LIS302DLState *s = IPOD_TOUCH_MACHINE(obj)->lis302dl_state;
    char axis = name[strlen(name) - 1];
    int64_t value = s ? (axis == 'x' ? s->base_x : axis == 'y' ? s->base_y : s->base_z) : 0;
    visit_type_int(v, name, &value, errp);
}

static void ipod_touch_set_accel_axis(Object *obj, Visitor *v, const char *name,
                                      void *opaque, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    int64_t val;
    if (!visit_type_int(v, name, &val, errp)) {
        return;
    }
    /* name is "accel-x" / "accel-y" / "accel-z" */
    if (nms->lis302dl_state) {
        lis302dl_set_axis_value(nms->lis302dl_state, name[strlen(name) - 1], (int)CLAMP(val, -128, 127));
    }
}

static void ipod_touch_set_accel_shake(Object *obj, Visitor *v, const char *name,
                                       void *opaque, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    bool val;
    if (!visit_type_bool(v, name, &val, errp)) {
        return;
    }
    if (val && nms->lis302dl_state) {
        lis302dl_shake(nms->lis302dl_state);
    }
}

/* Defined with the rest of the pasteboard code, further down. */
static char *ipod_touch_get_pasteboard(Object *obj, Error **errp);
static void ipod_touch_set_pasteboard(Object *obj, const char *value,
                                      Error **errp);
static char *ipod_touch_get_guest_pasteboard(Object *obj, Error **errp);
static char *ipod_touch_get_pb_agent(Object *obj, Error **errp);
static char *ipod_touch_get_pb_status(Object *obj, Error **errp);

static void ipod_touch_set_agent_request(Object *obj, const char *value, Error **errp)
{
    if (!ipod_agent_submit(IPOD_TOUCH_MACHINE(obj)->agent, value)) {
        error_setg(errp, "Invalid, duplicate, or full agent request queue");
    }
}

static void ipod_touch_cancel_agent_request(Object *obj, const char *value, Error **errp)
{
    ipod_agent_cancel(IPOD_TOUCH_MACHINE(obj)->agent, value);
}

static char *ipod_touch_get_agent_result(Object *obj, Error **errp)
{
    return ipod_agent_take_result(IPOD_TOUCH_MACHINE(obj)->agent);
}

static char *ipod_touch_get_agent_status(Object *obj, Error **errp)
{
    return g_strdup(ipod_agent_status(IPOD_TOUCH_MACHINE(obj)->agent,
                                     qemu_clock_get_ms(QEMU_CLOCK_REALTIME)));
}

static void ipod_touch_get_gles_contexts(Object *obj, Visitor *v, const char *name,
                                         void *opaque, Error **errp)
{
    int64_t count = gles_host_context_count();
    visit_type_int(v, name, &count, errp);
}

static void ipod_touch_instance_finalize(Object *obj)
{
    ipod_agent_publish(NULL);
    ipod_agent_free(IPOD_TOUCH_MACHINE(obj)->agent);
}

static void ipod_touch_instance_init(Object *obj)
{
    IPOD_TOUCH_MACHINE(obj)->audio_hw = ON_OFF_AUTO_AUTO;
    IPOD_TOUCH_MACHINE(obj)->bt_enabled = true;
    IPOD_TOUCH_MACHINE(obj)->bt_latency_us = 2000;
    IPOD_TOUCH_MACHINE(obj)->time_dilation = 1;
    IPOD_TOUCH_MACHINE(obj)->boot_args_delay_ms = 2000;
    IPOD_TOUCH_MACHINE(obj)->boot_args_repeat = 24;
    IPOD_TOUCH_MACHINE(obj)->boot_args_interval_ms = 500;
    IPOD_TOUCH_MACHINE(obj)->agent = ipod_agent_new();
    ipod_agent_publish(IPOD_TOUCH_MACHINE(obj)->agent);
    object_property_add_str(obj, "agent-request", NULL, ipod_touch_set_agent_request);
    object_property_add_str(obj, "agent-cancel", NULL, ipod_touch_cancel_agent_request);
    object_property_add_str(obj, "agent-result", ipod_touch_get_agent_result, NULL);
    object_property_add_str(obj, "agent-status", ipod_touch_get_agent_status, NULL);
    object_property_add(obj, "gles-contexts", "int", ipod_touch_get_gles_contexts, NULL, NULL, NULL);

    object_property_add_str(obj, "bootrom", ipod_touch_get_bootrom_path, ipod_touch_set_bootrom_path);
    object_property_set_description(obj, "bootrom", "Path to the S5L8720 bootrom binary");

	object_property_add_str(obj, "nor", ipod_touch_get_nor_path, ipod_touch_set_nor_path);
    object_property_set_description(obj, "nor", "Path to the S5L8720 NOR image");
    object_property_add_str(obj, "nor-rw", ipod_touch_get_nor_rw, ipod_touch_set_nor_rw);
    object_property_set_description(obj, "nor-rw", "Existing private writable NOR copy; empty keeps writes in memory");

    object_property_add_str(obj, "nand", ipod_touch_get_nand_path, ipod_touch_set_nand_path);
    object_property_set_description(obj, "nand", "Path to the NAND files");

    object_property_add_str(obj, "nandrw", ipod_touch_get_nand_overlay, ipod_touch_set_nand_overlay);
    object_property_set_description(obj, "nandrw", "Path to a writable NAND overlay directory (copy-on-write); NAND writes are stored here and read back on top of the read-only 'nand' image");

    object_property_add_bool(obj, "wifi", ipod_touch_get_wifi, ipod_touch_set_wifi);
    object_property_set_description(obj, "wifi",
        "Present a BCM4325 on the SDIO bus. Off by default: the dongle "
        "emulation is incomplete, so the driver attaches and then gets stuck");

    /* No default override: unconfigured boots retain firmware defaults. */
    object_property_add_str(obj, "boot-args", ipod_touch_get_boot_args, ipod_touch_set_boot_args);
    object_property_set_description(obj, "boot-args",
        "Startup kernel command line (at most 255 bytes); empty disables injection");

    object_property_add(obj, "boot-args-delay-ms", "uint32",
                        ipod_touch_get_boot_args_delay_ms,
                        ipod_touch_set_boot_args_delay_ms, NULL, NULL);
    object_property_set_description(obj, "boot-args-delay-ms",
        "Initial command-line write delay in virtual milliseconds (0..3600000; default 2000)");
    object_property_add(obj, "boot-args-repeat", "uint32",
                        ipod_touch_get_boot_args_repeat,
                        ipod_touch_set_boot_args_repeat, NULL, NULL);
    object_property_set_description(obj, "boot-args-repeat",
        "Command-line write count (0..1000000; default 24; 0 still performs the initial write)");
    object_property_add(obj, "boot-args-interval-ms", "uint32",
                        ipod_touch_get_boot_args_interval_ms,
                        ipod_touch_set_boot_args_interval_ms, NULL, NULL);
    object_property_set_description(obj, "boot-args-interval-ms",
        "Command-line retry interval in virtual milliseconds (1..3600000; default 500)");

    object_property_add_str(obj, "usb-tcp-addr", ipod_touch_get_usb_tcp_addr, ipod_touch_set_usb_tcp_addr);
    object_property_set_description(obj, "usb-tcp-addr",
        "host:port of a USB-over-TCP host bridge (e.g. usbmuxd's QEMU backend). "
        "Empty disables the link");

    /* On by default: the emulated device is effectively tethered to the host. */
    IPOD_TOUCH_MACHINE(obj)->usb_attached = true;
    IPOD_TOUCH_MACHINE(obj)->battery_adc = 850;
    object_property_add(obj, "battery-adc", "int", ipod_touch_get_battery_adc,
                        ipod_touch_set_battery_adc, NULL, NULL);
    object_property_add(obj, "battery-level", "int", ipod_touch_get_battery_level,
                        ipod_touch_set_battery_level, NULL, NULL);
    object_property_set_description(obj, "battery-level",
        "Battery voltage target (0-100 percent); guest sampling and filtering apply");
    object_property_add(obj, "battery-drain", "number", ipod_touch_get_battery_drain,
                        ipod_touch_set_battery_drain, NULL, NULL);
    object_property_set_description(obj, "battery-drain",
        "Percent per virtual minute while unplugged or charging is forced off; zero disables drain");
    object_property_add_str(obj, "battery-charging", ipod_touch_get_battery_charging,
                            ipod_touch_set_battery_charging);
    /* On by default: this gates the verified MBX MMU request/ack mirror
     * (ipod_touch_mbx.c), which stops the ~21M-read disable-loop spin that
     * froze OpenGL ES apps. The 2D boot path does not touch the MMU handshake,
     * so defaulting it on is safe there. */
    IPOD_TOUCH_MACHINE(obj)->mbx_irq = true;
    object_property_add_bool(obj, "mbx-irq", ipod_touch_get_mbx_irq, ipod_touch_set_mbx_irq);
    object_property_set_description(obj, "mbx-irq",
        "Raise a completion interrupt for the unemulated MBX GPU so an app that "
        "submits work does not wait for it forever. Cannot make anything render");

    object_property_add_bool(obj, "usb-attached", ipod_touch_get_usb_attached, ipod_touch_set_usb_attached);
    object_property_set_description(obj, "usb-attached",
        "Report a USB cable as present to the PMU. iOS leaves the whole USB device "
        "stack parked until it sees this");

    object_property_add_bool(obj, "usb-patch-mux-gate", ipod_touch_get_usb_patch_mux_gate,
                             ipod_touch_set_usb_patch_mux_gate);
    object_property_set_description(obj, "usb-patch-mux-gate",
        "Patch the kernel so the USB stack goes on bus even though the PTP interface "
        "function never registers a driver. Firmware-build-specific (2.1.1 / 5F138)");

    /* Accelerometer (LIS302DL) host controls; see the getters/setters above. */
    object_property_add(obj, "accel-rate-hz", "int", ipod_touch_get_accel_rate, ipod_touch_set_accel_rate, NULL, NULL);
    object_property_set_description(obj, "accel-rate-hz", "Sample rate override, 1..400 Hz; 0 follows sensor DR (100/400 Hz)");
    object_property_add_str(obj, "accel-pose", ipod_touch_get_accel_pose, ipod_touch_set_accel_pose);
    object_property_add(obj, "accel-pitch", "number", ipod_touch_get_accel_angle, ipod_touch_set_accel_angle, NULL, NULL);
    object_property_add(obj, "accel-roll", "number", ipod_touch_get_accel_angle, ipod_touch_set_accel_angle, NULL, NULL);
    object_property_add(obj, "accel-orientation", "int",
                        ipod_touch_get_accel_orientation,
                        ipod_touch_set_accel_orientation, NULL, NULL);
    object_property_set_description(obj, "accel-orientation",
        "Set the reported device orientation: 1=portrait, 2=portrait-upside-down, "
        "3=landscape-home-right, 4=landscape-home-left, 5=face-up, 6=face-down");
    object_property_add(obj, "accel-x", "int", ipod_touch_get_accel_axis, ipod_touch_set_accel_axis, NULL, NULL);
    object_property_add(obj, "accel-y", "int", ipod_touch_get_accel_axis, ipod_touch_set_accel_axis, NULL, NULL);
    object_property_add(obj, "accel-z", "int", ipod_touch_get_accel_axis, ipod_touch_set_accel_axis, NULL, NULL);
    object_property_add(obj, "accel-shake", "bool", NULL, ipod_touch_set_accel_shake, NULL, NULL);

    /*
     * Pasteboard. Setting this from QMP is the headless equivalent of the
     * Cocoa "Paste Text to Guest" menu item, and the only one available when
     * there is no display and so no clipboard peer at all:
     *
     *   qom-set  path=/machine property=pasteboard value="Hello. World #1"
     *   qom-get  path=/machine property=pasteboard-status
     *
     * pasteboard-status, NOT guest-pasteboard, is how you check that the text
     * arrived. guest-pasteboard is the other direction -- the last text copied
     * INSIDE the guest -- and the agent suppresses the echo of anything the
     * host sent, so host text can never show up there no matter how well this
     * works. Polling it for the string you just set is a guaranteed false
     * negative, and has already been read once as "host->guest is broken" on a
     * channel that was delivering correctly.
     */
    object_property_add_str(obj, "pasteboard", ipod_touch_get_pasteboard,
                            ipod_touch_set_pasteboard);
    object_property_set_description(obj, "pasteboard",
        "Text to hand to the guest's UIPasteboard. Collected by the guest "
        "pasteboard agent (contrib/it-pasteboard/it_pbd.c), after which the "
        "user pastes it wherever they like -- unlike the on-screen-keyboard "
        "typist, punctuation and symbols survive. Reads back only what is "
        "still WAITING to be collected, so it empties as soon as the guest "
        "takes it -- read pasteboard-status to see whether it arrived");

    object_property_add_str(obj, "guest-pasteboard",
                            ipod_touch_get_guest_pasteboard, NULL);
    object_property_set_description(obj, "guest-pasteboard",
        "GUEST -> HOST only: the last text copied inside the guest, as "
        "reported by the pasteboard agent. NOT a readback of what the host "
        "sent -- the agent suppresses that echo deliberately, so text set "
        "through the 'pasteboard' property never appears here. Also pushed to "
        "the host clipboard when a UI with a clipboard peer is attached");

    object_property_add_str(obj, "pasteboard-agent",
                            ipod_touch_get_pb_agent, NULL);
    object_property_set_description(obj, "pasteboard-agent",
        "Whether a guest pasteboard agent is actually running: 'alive', "
        "'stale' or 'absent'. Setting the pasteboard succeeds whether or not "
        "anything is listening, so ask this before believing it");

    object_property_add_str(obj, "pasteboard-status",
                            ipod_touch_get_pb_status, NULL);
    object_property_set_description(obj, "pasteboard-status",
        "Whether host -> guest text actually reached the guest agent: "
        "'queued' (still waiting), 'delivered' (with the text, its size and "
        "how long ago) or 'idle'. This is the readback the other three "
        "properties cannot give you");
}

static inline qemu_irq s5l8900_get_irq(IPodTouchMachineState *s, int n)
{
    return s->irq[n / S5L8720_VIC_SIZE][n % S5L8720_VIC_SIZE];
}

static uint32_t s5l8720_usb_hwcfg[] = {
    0,
    0x7a8f60d0,
    0x082000e8,
    0x01f08024
};

/*
 * The volume buttons are ACTIVE LOW; hold and menu are not.
 *
 * The device tree says so directly: function-button_hold <gpio 0x0c02 0x100>
 * and function-button_menu <gpio 0x0c01 0x100> carry 0x100, while
 * function-button_volup <gpio 0x0902 0x000> and function-button_voldown
 * <gpio 0x0c00 0x000> carry 0. We zeroed every pad at reset and treated
 * pad-set as pressed for all four, so from boot - with no input at all - iOS
 * saw BOTH volume buttons held down. It emitted volume changes continuously,
 * SpringBoard re-showed the HUD on each one so it never dismissed, and because
 * up and down were both held the level wandered instead of sitting still.
 * That is exactly the "volume HUD flickering between 3-4 bars from boot"
 * symptom, and visiting Settings > Sounds only masked it for that session.
 *
 * So park these two high and pull them down to press. Hold and menu are
 * untouched, which is why they always worked.
 */
static bool volbtn_is_pressed(IPodTouchMultitouchState *s, uint32_t gpio)
{
    return gpio_is_off(s->gpio_state->gpio_state, gpio);
}

static void volbtn_press(IPodTouchMultitouchState *s, uint32_t gpio, bool pressed)
{
    if (pressed) {
        gpio_set_off(s->gpio_state->gpio_state, gpio);
    } else {
        gpio_set_on(s->gpio_state->gpio_state, gpio);
    }
}

static void ipod_touch_key_event(void *opaque, int keycode)
{
    bool do_irq = false;
    int gpio_group = 0, gpio_selector = 0;
    uint32_t button_gpio = 0;

    IPodTouchMultitouchState *s = (IPodTouchMultitouchState *)opaque;
    if(keycode == KEY_P_DOWN || keycode == KEY_P_UP) {
        // power button
        gpio_group = GPIO_BUTTON_POWER_IRQ / NUM_GPIO_PINS;
        gpio_selector = GPIO_BUTTON_POWER_IRQ % NUM_GPIO_PINS;
        button_gpio = GPIO_BUTTON_POWER;

        if(keycode == KEY_P_DOWN && gpio_is_off(s->gpio_state->gpio_state, GPIO_BUTTON_POWER)) {
            gpio_set_on(s->gpio_state->gpio_state, GPIO_BUTTON_POWER);
            do_irq = true;
        }
        else if(keycode == KEY_P_UP && !gpio_is_off(s->gpio_state->gpio_state, GPIO_BUTTON_POWER)) {
            gpio_set_off(s->gpio_state->gpio_state, GPIO_BUTTON_POWER);
            do_irq = true;
        }
    }
    else if(keycode == KEY_H_DOWN || keycode == KEY_H_UP) {
        // home button
        gpio_group = GPIO_BUTTON_HOME_IRQ / NUM_GPIO_PINS;
        gpio_selector = GPIO_BUTTON_HOME_IRQ % NUM_GPIO_PINS;
        button_gpio = GPIO_BUTTON_HOME;

        if(keycode == KEY_H_DOWN && gpio_is_off(s->gpio_state->gpio_state, GPIO_BUTTON_HOME)) {
            gpio_set_on(s->gpio_state->gpio_state, GPIO_BUTTON_HOME);
            do_irq = true;
        }
        else if(keycode == KEY_H_UP && !gpio_is_off(s->gpio_state->gpio_state, GPIO_BUTTON_HOME)) {
            gpio_set_off(s->gpio_state->gpio_state, GPIO_BUTTON_HOME);
            do_irq = true;
        }
    }
    else if(keycode == KEY_MIN_DOWN || keycode == KEY_MIN_UP) {
        // volume down button
        gpio_group = GPIO_BUTTON_VOLDOWN_IRQ / NUM_GPIO_PINS;
        gpio_selector = GPIO_BUTTON_VOLDOWN_IRQ % NUM_GPIO_PINS;
        button_gpio = GPIO_BUTTON_VOLDOWN;

        /* Active low: pressed pulls the pad down. See volbtn_press() above. */
        if(keycode == KEY_MIN_DOWN && !volbtn_is_pressed(s, GPIO_BUTTON_VOLDOWN)) {
            volbtn_press(s, GPIO_BUTTON_VOLDOWN, true);
            do_irq = true;
        }
        else if(keycode == KEY_MIN_UP && volbtn_is_pressed(s, GPIO_BUTTON_VOLDOWN)) {
            volbtn_press(s, GPIO_BUTTON_VOLDOWN, false);
            do_irq = true;
        }
    }
    else if(keycode == KEY_PLUS_DOWN || keycode == KEY_PLUS_UP) {
        // volume up button
        gpio_group = GPIO_BUTTON_VOLUP_IRQ / NUM_GPIO_PINS;
        gpio_selector = GPIO_BUTTON_VOLUP_IRQ % NUM_GPIO_PINS;
        button_gpio = GPIO_BUTTON_VOLUP;

        /* Active low: pressed pulls the pad down. See volbtn_press() above. */
        if(keycode == KEY_PLUS_DOWN && !volbtn_is_pressed(s, GPIO_BUTTON_VOLUP)) {
            volbtn_press(s, GPIO_BUTTON_VOLUP, true);
            do_irq = true;
        }
        else if(keycode == KEY_PLUS_UP && volbtn_is_pressed(s, GPIO_BUTTON_VOLUP)) {
            volbtn_press(s, GPIO_BUTTON_VOLUP, false);
            do_irq = true;
        }
    }
    else return;

    // Only raise a GPIO interrupt on a genuine press/release edge. Without this
    // guard, SDL key auto-repeat (which resends KEY_*_DOWN while a key is held)
    // would fire a fresh interrupt each time even though the button state did
    // not change, spamming the interrupt controller with phantom presses.
    if(do_irq) {
        // Mirror the physical pin level into the sysic. GPIO_INTLEVEL was read
        // by the guest (sysic read handler) but never written anywhere, so it
        // always returned 0 regardless of the real pin state. Reflect the
        // current button level here so it is at least consistent with the pad
        // registers and the latched interrupt status.
        if(gpio_is_on(s->gpio_state->gpio_state, button_gpio)) {
            s->sysic->gpio_int_level[gpio_group] |= (1 << gpio_selector);
        } else {
            s->sysic->gpio_int_level[gpio_group] &= ~(1 << gpio_selector);
        }
        s->sysic->gpio_int_status[gpio_group] |= (1 << gpio_selector);
        qemu_irq_raise(s->sysic->gpio_irqs[gpio_group]);

        // The hold (power) and menu (home) buttons are also wake sources routed
        // through the PMU, which is a nested interrupt controller on the SoC.
        // The awake-state path above goes through the GPIO controller; the wake
        // path goes through the PMU. On a press the PMU latches the button's
        // EVENT_C interrupt bit (reg 0x03) and raises its own interrupt (GPIO
        // IRQ 0x61); iOS reads the EVENT block, decodes the bit, and reads the
        // live STAT reg 0x19 to confirm the button and re-enable the display.
        // hold and menu occupy the same bit positions in EVENT_C and STAT.
        if (button_gpio == GPIO_BUTTON_POWER || button_gpio == GPIO_BUTTON_HOME) {
            uint8_t bit = (button_gpio == GPIO_BUTTON_POWER)
                              ? PMU_STAT_HOLD : PMU_STAT_MENU;
            bool pressed = gpio_is_on(s->gpio_state->gpio_state, button_gpio);

            // reg 0x19 tracks the live button level (set while held, cleared on
            // release); the EVENT_C interrupt is edge-latched on the press only.
            pcf50633_set_stat(PCF50633(s->pmu), bit, pressed);
            if (pressed) {
                pcf50633_latch_wake_event(PCF50633(s->pmu), bit);


            }
        }
    }
}

/*
 * Host keyboard -> guest text input.
 *
 * The legacy ipod_touch_key_event above drives the four hardware buttons from
 * bare letter keys (P/H/-/+), which collides with typing. This modern handler
 * replaces it: it moves the buttons behind the host Command modifier so every
 * plain key is free for text, and queues bare printable keys as unichars for
 * injection into the guest's text-input system. The button/PMU-wake logic is
 * reused unchanged - a Command combo just calls ipod_touch_key_event with the
 * scancode that combo used to be.
 */
static IPodTouchMachineState *s_kbd_nms;
static IPodTouchMultitouchState *s_kbd_mt;

static uint16_t qcode_to_unichar(int q, bool shift)
{
	switch (q) {
	case Q_KEY_CODE_A: return shift ? 'A' : 'a';
	case Q_KEY_CODE_B: return shift ? 'B' : 'b';
	case Q_KEY_CODE_C: return shift ? 'C' : 'c';
	case Q_KEY_CODE_D: return shift ? 'D' : 'd';
	case Q_KEY_CODE_E: return shift ? 'E' : 'e';
	case Q_KEY_CODE_F: return shift ? 'F' : 'f';
	case Q_KEY_CODE_G: return shift ? 'G' : 'g';
	case Q_KEY_CODE_H: return shift ? 'H' : 'h';
	case Q_KEY_CODE_I: return shift ? 'I' : 'i';
	case Q_KEY_CODE_J: return shift ? 'J' : 'j';
	case Q_KEY_CODE_K: return shift ? 'K' : 'k';
	case Q_KEY_CODE_L: return shift ? 'L' : 'l';
	case Q_KEY_CODE_M: return shift ? 'M' : 'm';
	case Q_KEY_CODE_N: return shift ? 'N' : 'n';
	case Q_KEY_CODE_O: return shift ? 'O' : 'o';
	case Q_KEY_CODE_P: return shift ? 'P' : 'p';
	case Q_KEY_CODE_Q: return shift ? 'Q' : 'q';
	case Q_KEY_CODE_R: return shift ? 'R' : 'r';
	case Q_KEY_CODE_S: return shift ? 'S' : 's';
	case Q_KEY_CODE_T: return shift ? 'T' : 't';
	case Q_KEY_CODE_U: return shift ? 'U' : 'u';
	case Q_KEY_CODE_V: return shift ? 'V' : 'v';
	case Q_KEY_CODE_W: return shift ? 'W' : 'w';
	case Q_KEY_CODE_X: return shift ? 'X' : 'x';
	case Q_KEY_CODE_Y: return shift ? 'Y' : 'y';
	case Q_KEY_CODE_Z: return shift ? 'Z' : 'z';
	case Q_KEY_CODE_1: return shift ? '!' : '1';
	case Q_KEY_CODE_2: return shift ? '@' : '2';
	case Q_KEY_CODE_3: return shift ? '#' : '3';
	case Q_KEY_CODE_4: return shift ? '$' : '4';
	case Q_KEY_CODE_5: return shift ? '%' : '5';
	case Q_KEY_CODE_6: return shift ? '^' : '6';
	case Q_KEY_CODE_7: return shift ? '&' : '7';
	case Q_KEY_CODE_8: return shift ? '*' : '8';
	case Q_KEY_CODE_9: return shift ? '(' : '9';
	case Q_KEY_CODE_0: return shift ? ')' : '0';
	case Q_KEY_CODE_MINUS: return shift ? '_' : '-';
	case Q_KEY_CODE_EQUAL: return shift ? '+' : '=';
	case Q_KEY_CODE_BRACKET_LEFT:  return shift ? '{' : '[';
	case Q_KEY_CODE_BRACKET_RIGHT: return shift ? '}' : ']';
	case Q_KEY_CODE_BACKSLASH: return shift ? '|' : '\\';
	case Q_KEY_CODE_SEMICOLON: return shift ? ':' : ';';
	case Q_KEY_CODE_APOSTROPHE: return shift ? '"' : '\'';
	case Q_KEY_CODE_GRAVE_ACCENT: return shift ? '~' : '`';
	case Q_KEY_CODE_COMMA: return shift ? '<' : ',';
	case Q_KEY_CODE_DOT: return shift ? '>' : '.';
	case Q_KEY_CODE_SLASH: return shift ? '?' : '/';
	case Q_KEY_CODE_SPC: return ' ';
	case Q_KEY_CODE_RET: return '\n';
	case Q_KEY_CODE_BACKSPACE: return 0x08;
	case Q_KEY_CODE_TAB: return '\t';
	default: return 0;
	}
}

/*
 * Host <-> guest pasteboard.
 *
 * The point of this path is that it is not the keyboard. Typing by synthesising
 * taps on iOS's own on-screen keyboard loses exactly the characters people most
 * want to move between the two machines -- punctuation, spaces in URL fields,
 * anything on the symbols page -- because each of those depends on keyboard page
 * state that has no feedback channel. Handing the text to UIPasteboard and
 * letting the user tap Paste has no geometry in it at all.
 *
 * The host cannot write the guest pasteboard by itself: pasteboardd owns the
 * live state and UIPasteboard is its only client (contrib/it-pasteboard/README).
 * So the host only ever parks text here, and the guest agent collects it over
 * the QC_PB_* ops.
 */
static void ipod_touch_pb_notify(Notifier *notifier, void *data)
{
    /* We only ever publish; host-side clipboard changes are pushed to the
     * guest explicitly (menu item / qom-set), never automatically. */
}

static void ipod_touch_pb_request(QemuClipboardInfo *info,
                                  QemuClipboardType type)
{
    /* Unreachable in practice: we always set the data at the same time as we
     * announce it, and qemu_clipboard_request only calls back for announced
     * types whose data is still missing. */
}

static QemuClipboardPeer ipod_touch_pb_peer = {
    .name = "ipod-touch",
    .notifier = { .notify = ipod_touch_pb_notify },
    .request = ipod_touch_pb_request,
};

/*
 * How long to give the guest before saying nobody took the text. The agent
 * polls four times a second, so anything it is going to collect it collects
 * almost immediately; this only has to be longer than one poll interval plus
 * the slack of a heavily loaded emulator.
 */
#define PB_WARN_MS 10000

static void ipod_touch_pb_warn(void *opaque)
{
    IPodTouchMachineState *nms = opaque;

    if (!nms->pb_out) {
        return;                 /* collected after all */
    }
    if (nms->pb_polls != nms->pb_polls_at_set) {
        /* Something polled but did not take it. Not the missing-daemon case,
         * so say what it actually is rather than sending anyone to the
         * install instructions. */
        warn_report("pasteboard: the guest agent polled but has not collected "
                    "the text after %d ms", PB_WARN_MS);
        return;
    }

    warn_report("pasteboard: %zu bytes queued for the guest and nothing has "
                "polled for them", nms->pb_out_len);
    if (nms->pb_last_poll_ns == 0) {
        error_printf("         No pasteboard agent has EVER polled this "
                     "machine. it_pbd is almost certainly not installed in "
                     "this NAND image -- setting the property succeeds either "
                     "way, which is why this warning exists.\n"
                     "         Install it: contrib/it-pasteboard/README.md "
                     "(and remember the plist must be owned by root, or "
                     "launchd ignores it without a word).\n"
                     "         Check at any time with:  qom-get "
                     "path=/machine property=pasteboard-agent\n");
    } else {
        error_printf("         The agent last polled %" PRId64 " s ago, so it "
                     "has stopped or died. /var/log/it_pbd.log on the guest "
                     "says which.\n",
                     (qemu_clock_get_ns(QEMU_CLOCK_REALTIME) -
                      nms->pb_last_poll_ns) / NANOSECONDS_PER_SECOND);
    }
}

void ipod_touch_pb_set(IPodTouchMachineState *nms, const char *text)
{
    g_free(nms->pb_out);
    nms->pb_out = NULL;
    nms->pb_out_len = 0;

    if (!text || !*text) {
        if (nms->pb_warn_timer) {
            timer_del(nms->pb_warn_timer);
        }
        return;
    }
    /* A clipboard holds one item. Replacing rather than queueing means a
     * second copy on the host wins, which is what the user just asked for. */
    nms->pb_out = g_strndup(text, QC_PB_MAX_LEN);
    nms->pb_out_len = strlen(nms->pb_out);

    /*
     * Arm the "nobody is listening" check. Handing text to a machine with no
     * guest agent used to be indistinguishable from success from the host
     * side -- no error, no log line, the text just sat in pb_out forever.
     */
    nms->pb_polls_at_set = nms->pb_polls;
    if (!nms->pb_warn_timer) {
        nms->pb_warn_timer = timer_new_ms(QEMU_CLOCK_REALTIME,
                                          ipod_touch_pb_warn, nms);
    }
    timer_mod(nms->pb_warn_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + PB_WARN_MS);
}

void ipod_touch_pb_guest_commit(IPodTouchMachineState *nms)
{
    QemuClipboardInfo *info;

    g_free(nms->pb_guest);
    nms->pb_guest = nms->pb_in;
    nms->pb_guest_len = nms->pb_in_len;
    nms->pb_in = NULL;
    nms->pb_in_len = 0;

    if (!nms->pb_guest) {
        return;
    }

    /*
     * Registered on first use rather than at machine init: a headless run has
     * no clipboard peer on the other side at all, and the guest text is still
     * readable there through the guest-pasteboard property.
     */
    if (!nms->pb_peer_registered) {
        qemu_clipboard_peer_register(&ipod_touch_pb_peer);
        nms->pb_peer_registered = true;
    }

    info = qemu_clipboard_info_new(&ipod_touch_pb_peer,
                                   QEMU_CLIPBOARD_SELECTION_CLIPBOARD);
    qemu_clipboard_set_data(&ipod_touch_pb_peer, info,
                            QEMU_CLIPBOARD_TYPE_TEXT,
                            nms->pb_guest_len, nms->pb_guest, true);
    qemu_clipboard_info_unref(info);
}

static char *ipod_touch_get_pasteboard(Object *obj, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    return g_strdup(nms->pb_out ? nms->pb_out : "");
}

static void ipod_touch_set_pasteboard(Object *obj, const char *value,
                                      Error **errp)
{
    ipod_touch_pb_set(IPOD_TOUCH_MACHINE(obj), value);
}

static char *ipod_touch_get_guest_pasteboard(Object *obj, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    return g_strdup(nms->pb_guest ? nms->pb_guest : "");
}

/*
 * "Is anything on the other end?" -- answerable before you rely on it, rather
 * than after wondering why nothing pasted. The agent polls every 250 ms, so a
 * poll inside the last few seconds means it is running right now.
 */
#define PB_ALIVE_NS (5 * NANOSECONDS_PER_SECOND)

static char *ipod_touch_get_pb_agent(Object *obj, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    if (nms->pb_last_poll_ns == 0) {
        return g_strdup("absent: nothing has ever polled -- it_pbd is not "
                        "installed or not running (contrib/it-pasteboard)");
    }
    if (now - nms->pb_last_poll_ns > PB_ALIVE_NS) {
        return g_strdup_printf("stale: last polled %" PRId64 " s ago",
                               (now - nms->pb_last_poll_ns) /
                               NANOSECONDS_PER_SECOND);
    }
    return g_strdup_printf("alive: %" PRIu64 " polls", nms->pb_polls);
}

/*
 * "Did the text I sent get there?" -- which is NOT what any of the properties
 * above answer, and the gap cost a whole investigation.
 *
 * "pasteboard" reads back the item still WAITING, so it empties the instant the
 * guest takes it: collected and never-sent are both "". And "guest-pasteboard"
 * is not a readback at all -- it is the last text COPIED INSIDE the guest, and
 * the agent deliberately marks host text as already-seen so it is never echoed
 * back, so host text can never appear there however well the channel works.
 * Watching it for the string you just set is therefore guaranteed to look like
 * a failure. This property is the one that answers the question.
 */
static char *ipod_touch_get_pb_status(Object *obj, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    g_autofree char *agent = ipod_touch_get_pb_agent(obj, NULL);

    if (nms->pb_out) {
        return g_strdup_printf("queued: %zu bytes still waiting for the guest "
                               "(agent: %s)", nms->pb_out_len, agent);
    }
    if (nms->pb_delivered) {
        /* Truncated: this is a status line, not a transcript. */
        g_autofree char *shown = g_strndup(nms->pb_delivered, 64);
        return g_strdup_printf("delivered: %zu bytes, %" PRId64 " s ago, "
                               "%" PRIu64 " total: \"%s\"%s (agent: %s)",
                               nms->pb_delivered_len,
                               (now - nms->pb_delivered_ns) /
                               NANOSECONDS_PER_SECOND,
                               nms->pb_deliveries, shown,
                               nms->pb_delivered_len > 64 ? "..." : "", agent);
    }
    return g_strdup_printf("idle: nothing has been sent to the guest "
                           "(agent: %s)", agent);
}

static void ipod_touch_kbd_enqueue(IPodTouchMachineState *nms, uint16_t ch)
{
	unsigned next = (nms->kbd_tail + 1) % ARRAY_SIZE(nms->kbd_ring);
	if (next == nms->kbd_head) {
		return; /* ring full - drop, rather than overwrite unread input */
	}
	nms->kbd_ring[nms->kbd_tail] = ch;
	nms->kbd_tail = next;
}

/* Command+combo -> the button scancode the legacy handler already understands. */
/*
 * Command+Left / Command+Right turn the device a quarter turn, the way you would
 * physically rotate it.
 *
 * UIDeviceOrientation's numbering is not rotational, so this steps through the
 * physical order rather than incrementing. Holding the device face-on and turning
 * it clockwise moves the home button bottom -> left -> top -> right, that is
 * Portrait(1) -> LandscapeRight(4) -> PortraitUpsideDown(2) -> LandscapeLeft(3),
 * and counter-clockwise is the same cycle reversed.
 */
static void ipod_touch_kbd_rotate(bool clockwise)
{
	/* indexed by the current orientation; [0] is the unset/face-up case */
	static const uint32_t cw[5]  = { 1, 4, 3, 1, 2 };
	static const uint32_t ccw[5] = { 1, 3, 4, 2, 1 };
	IPodTouchMachineState *nms = s_kbd_nms;
	uint32_t cur;

	if (!nms || !nms->lis302dl_state) {
		return;
	}

	cur = nms->lis302dl_state->orientation;
	if (cur > 4) {
		cur = 0; /* never set, or face up/down: start from portrait */
	}
	lis302dl_apply_orientation(nms->lis302dl_state,
	                           clockwise ? cw[cur] : ccw[cur]);
}

/*
 * Which host keys are currently holding a guest button down, so the release can
 * always be delivered. The button path is gated on Command being held, but
 * releasing Command before the key is natural, and then the key-up fell through
 * to the text path and the button GPIO stayed asserted forever - iOS saw the
 * volume button held and ramped the volume continuously.
 */
static int s_kbd_btn_held[Q_KEY_CODE__MAX];

static void ipod_touch_kbd_button(int qcode, bool shift, bool down)
{
	int base = -1;
	switch (qcode) {
	case Q_KEY_CODE_L: base = KEY_P; break;          /* Command+L      -> power */
	case Q_KEY_CODE_H: if (shift) base = KEY_H; break;/* Command+Shift+H-> home  */
	case Q_KEY_CODE_MINUS: base = KEY_MIN; break;    /* Command+-      -> vol dn */
	case Q_KEY_CODE_EQUAL: base = KEY_PLUS; break;   /* Command+=      -> vol up */
	case Q_KEY_CODE_LEFT:                            /* Command+Left   -> rotate ccw */
	case Q_KEY_CODE_RIGHT:                           /* Command+Right  -> rotate cw  */
		if (down) {
			ipod_touch_kbd_rotate(qcode == Q_KEY_CODE_RIGHT);
		}
		return;
	default: break;
	}
	if (base < 0 || !s_kbd_mt) {
		return;
	}
	if (qcode >= 0 && qcode < Q_KEY_CODE__MAX) {
		s_kbd_btn_held[qcode] = down ? base : 0;
	}
	ipod_touch_key_event(s_kbd_mt, down ? base : (base | KEY_UP));
}

static void ipod_touch_osk_enqueue(IPodTouchMachineState *nms, uint16_t ch);

static void ipod_touch_kbd_event(DeviceState *dev, QemuConsole *src,
                                 InputEvent *evt)
{
	IPodTouchMachineState *nms = s_kbd_nms;
	InputKeyEvent *k = evt->u.key.data;
	int q = qemu_input_key_value_to_qcode(k->key);
	bool down = k->down;

	if (!nms) {
		return;
	}

	switch (q) {
	case Q_KEY_CODE_META_L:
	case Q_KEY_CODE_META_R:
		nms->kbd_cmd = down;
		return;
	case Q_KEY_CODE_SHIFT:
	case Q_KEY_CODE_SHIFT_R:
		nms->kbd_shift = down;
		return;
	default:
		break;
	}

	/*
	 * A key that is currently holding a button down must always release it,
	 * even if Command was let go first.
	 */
	if (!down && q >= 0 && q < Q_KEY_CODE__MAX && s_kbd_btn_held[q]) {
		int base = s_kbd_btn_held[q];
		s_kbd_btn_held[q] = 0;
		if (s_kbd_mt) {
			ipod_touch_key_event(s_kbd_mt, base | KEY_UP);
		}
		return;
	}

	if (nms->kbd_cmd) {
		/* buttons live behind Command now */
		ipod_touch_kbd_button(q, nms->kbd_shift, down);
		return;
	}

	if (down) {
		uint16_t ch = qcode_to_unichar(q, nms->kbd_shift);
		if (ch && nms->osk_enabled) {
			/* Type by tapping iOS's own on-screen keyboard. */
			ipod_touch_osk_enqueue(nms, ch);
			if (getenv("IT_KBD_TRACE")) {
				fprintf(stderr, "[KBD] osk 0x%04x '%c'\n", ch,
				        (ch >= 0x20 && ch < 0x7f) ? ch : '.');
			}
			return;
		}
		if (ch) {
			ipod_touch_kbd_enqueue(nms, ch);
			/* Injection into _GSPostSyntheticKeyEvent is wired up separately;
			 * for now the queue is the interface the drain will consume. */
			if (getenv("IT_KBD_TRACE")) {
				fprintf(stderr, "[KBD] queued 0x%04x '%c'\n", ch,
				        (ch >= 0x20 && ch < 0x7f) ? ch : '.');
			}
		}
	}
}

static const QemuInputHandler ipod_touch_kbd_handler = {
	.name  = "iPod Touch Keyboard",
	.mask  = INPUT_EVENT_MASK_KEY,
	.event = ipod_touch_kbd_event,
};

/*
 * Scripted "slide to power off".
 *
 * iPhone OS 2.1.1 has no remote shutdown: lockdownd has neither a reboot
 * request nor a diagnostics relay, and nothing in the guest runs as root that
 * we can drive. The one path to a *clean* shutdown -- the one that unmounts the
 * root volume and so flushes HFS+'s in-memory catalog to flash -- is the user
 * gesture: hold the hold button until SpringBoard raises its power-off sheet,
 * then drag the slider. So the machine performs that gesture itself.
 *
 * QEMU's own system_powerdown is the trigger, which is exactly what it means
 * elsewhere: ACPI machines send the guest a power-button event and let the OS
 * shut itself down. Here the "power button event" is a synthesised press plus
 * the slide the guest insists on.
 *
 * Everything is timed on QEMU_CLOCK_VIRTUAL so the sequence is deterministic
 * under host load: SpringBoard's hold-to-power-off threshold is measured in
 * guest time, so the hold must be too.
 *
 * The slider geometry is in display pixels on the 320x480 panel. The knob's Y
 * is FOUND, not assumed: it was hardcoded to 68, measured off a 2.1.1
 * screendump, and 3.1.3 puts its sheet at the top of the screen with the knob
 * centred at y=51 -- five pixels above where the synthetic finger came down. It
 * missed the knob entirely, the slide never happened, the PMU latch never
 * cleared, and QEMU sat until the caller's timeout and got killed. The visible
 * damage was two unrelated-looking regression failures (fsck "volume found
 * corrupt", persist "did not shut down cleanly"), both just downstream of an
 * unclean unmount.
 *
 * KNOWN BROKEN ON 3.1.3, and the coordinate is NOT the whole story.
 *
 * 68 is measured off 2.1.1's sheet and works there (clean halt in ~14s). On
 * 3.1.3 system_powerdown never completes: QEMU sits until the caller's timeout
 * and is killed, which surfaces as two unrelated-looking regression failures --
 * fsck "volume found corrupt" and persist "did not shut down cleanly" -- both
 * merely downstream of the unclean unmount.
 *
 * Measured on 3.1.3: the sheet DOES appear, and its knob is a red bar spanning
 * rows 39..63 (centre 51, x 24..91), i.e. 68 lands five pixels below the knob.
 * But dragging at the measured 51 does NOT fix it either, and neither does
 * waking the digitizer with a Home press first. So the miss is real but there
 * is a second cause -- the likeliest suspect is ipod_touch_synth_touch(), which
 * is the ONE toucher that does not go through
 * ipod_touch_multitouch_set_finger(); that path may simply not deliver on
 * 3.1.3. Whoever picks this up: instrument synth_touch and confirm the guest
 * driver ever sees these frames before touching coordinates again.
 *
 * Auto-detection from scanout_base was tried and removed: it chose a row 27 px
 * off, because the panel triple-buffers and that pointer is not reliably the
 * surface on screen. IT_PWROFF_KNOB_Y overrides the row so a fix can be swept
 * without rebuilding.
 */
/*
 * Time for the foreground app to quit and SpringBoard to come forward after the
 * synthetic Home press. The power-off sheet belongs to SpringBoard, so the
 * slide below cannot reach it from inside a full-screen app -- measured: a
 * powerdown requested while GLTest was foreground never completed, and the same
 * build powers off in 14s from the home screen. 3.1.3 has no app switcher, so
 * Home really does terminate the app, which also gives it its
 * applicationWillTerminate: to save with.
 */
#define PWROFF_HOME_MS      2500
#define PWROFF_HOLD_MS      3500   /* > SpringBoard's hold threshold           */
#define PWROFF_SETTLE_MS    1500   /* sheet slides in and settles              */
#define PWROFF_DRAG_STEPS   24
#define PWROFF_DRAG_STEP_MS 80
#define PWROFF_KNOB_X       65
#define PWROFF_KNOB_Y       68     /* 2.1.1, verified; see the note above       */
#define PWROFF_TRACK_END_X  295

/*
 * Which row the knob sits on, once per powerdown.
 *
 * This is a measured constant per OS, not something derivable: reading
 * scanout_base directly was tried and picked a row 27 px off, because the panel
 * triple-buffers and that pointer is not reliably the surface being displayed.
 * Rather than chase framebuffer semantics for a shutdown gesture, the value is
 * explicit and overridable.
 */
static int pwroff_knob_row(void)
{
    const char *e = getenv("IT_PWROFF_KNOB_Y");

    return e ? atoi(e) : PWROFF_KNOB_Y;
}

enum {
	PWROFF_IDLE = 0,
	PWROFF_HOME,
	PWROFF_PRESSED,
	PWROFF_SETTLING,
	PWROFF_DRAGGING,
	PWROFF_DONE,
};

/* Same effect as a mouse event on the display, but in panel pixels. */
static void ipod_touch_synth_touch(IPodTouchMachineState *nms,
                                   int px, int py, int state)
{
	IPodTouchLCDState *lcd = nms->lcd_state;
	IPodTouchMultitouchState *mt;

	if (!lcd || !lcd->mt) {
		return;
	}
	mt = lcd->mt;

	mt->prev_touch_x = mt->touch_x;
	mt->prev_touch_y = mt->touch_y;
	mt->touch_x = (float)px / 320.0f;
	mt->touch_y = 1.0f - (float)py / 480.0f;

	if (state && !mt->touch_down) {
		ipod_touch_multitouch_on_touch(mt);
	} else if (!state && mt->touch_down) {
		ipod_touch_multitouch_on_release(mt);
	}
	/*
	 * While the finger is down the digitizer's own report timer (60 Hz since
	 * the touch-rate work, and it also emits on motion) keeps producing
	 * TOUCH_MOVED frames from touch_x/touch_y, so updating them is the move.
	 *
	 * This is the one caller that writes touch_x/touch_y directly instead of
	 * going through ipod_touch_multitouch_set_finger(). It works because slot
	 * 0 is mirrored into those fields, but it cannot express a second finger.
	 */
}

static void ipod_touch_powerdown_arm(IPodTouchMachineState *nms, int ms)
{
	timer_mod(nms->pwroff_timer,
	          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
	              (int64_t)ms * SCALE_MS);
}

/*
 * Press or release a hardware button from a host UI that has no keyboard.
 *
 * On a Mac the four buttons are Command combos routed through
 * ipod_touch_kbd_button(); a phone has nowhere to type them, and without the
 * power and home buttons an emulated device that sleeps can never be woken.
 * This is the same key path, addressed by name instead of by keystroke.
 */
void ipod_touch_press_button(IPodTouchButton button, bool down)
{
	int base;

	if (!s_kbd_mt) {
		return;
	}

	switch (button) {
	case IPOD_TOUCH_BUTTON_HOME:    base = KEY_H;    break;
	case IPOD_TOUCH_BUTTON_POWER:   base = KEY_P;    break;
	case IPOD_TOUCH_BUTTON_VOLUP:   base = KEY_PLUS; break;
	case IPOD_TOUCH_BUTTON_VOLDOWN: base = KEY_MIN;  break;
	default: return;
	}

	ipod_touch_key_event(s_kbd_mt, down ? base : (base | KEY_UP));
}

static void ipod_touch_powerdown_tick(void *opaque)
{
	IPodTouchMachineState *nms = opaque;
	bool trace = getenv("IT_PWROFF_TRACE") != NULL;

	switch (nms->pwroff_phase) {
	case PWROFF_HOME:
		/* Home is released here; the press went in with the request. */
		if (s_kbd_mt) {
			ipod_touch_key_event(s_kbd_mt, KEY_H_UP);
		}
		if (trace) {
			fprintf(stderr, "[PWROFF] home pressed; holding the hold button\n");
		}
		if (s_kbd_mt) {
			ipod_touch_key_event(s_kbd_mt, KEY_P_DOWN);
		}
		nms->pwroff_phase = PWROFF_PRESSED;
		ipod_touch_powerdown_arm(nms, PWROFF_HOLD_MS);
		break;

	case PWROFF_PRESSED:
		/* Hold expired: release the button. The sheet is already up. */
		if (s_kbd_mt) {
			ipod_touch_key_event(s_kbd_mt, KEY_P_UP);
		}
		nms->pwroff_phase = PWROFF_SETTLING;
		ipod_touch_powerdown_arm(nms, PWROFF_SETTLE_MS);
		break;

	case PWROFF_SETTLING:
		/* The sheet is up and still, so this is the moment its knob can be
		 * located. Found once and held for the whole drag: re-scanning per
		 * step would follow the knob we are ourselves dragging. */
		nms->pwroff_knob_y = pwroff_knob_row();
		if (trace) {
			fprintf(stderr, "[PWROFF] knob row %d\n", nms->pwroff_knob_y);
		}
		ipod_touch_synth_touch(nms, PWROFF_KNOB_X, nms->pwroff_knob_y, 1);
		nms->pwroff_phase = PWROFF_DRAGGING;
		nms->pwroff_step = 0;
		ipod_touch_powerdown_arm(nms, PWROFF_DRAG_STEP_MS);
		break;

	case PWROFF_DRAGGING: {
		int i = ++nms->pwroff_step;
		int x = PWROFF_KNOB_X +
		        (PWROFF_TRACK_END_X - PWROFF_KNOB_X) * i / PWROFF_DRAG_STEPS;

		if (i < PWROFF_DRAG_STEPS) {
			ipod_touch_synth_touch(nms, x, nms->pwroff_knob_y, 1);
			ipod_touch_powerdown_arm(nms, PWROFF_DRAG_STEP_MS);
		} else {
			ipod_touch_synth_touch(nms, PWROFF_TRACK_END_X,
			                       nms->pwroff_knob_y, 0);
			/* Back to IDLE, not DONE. The sequence is over -- the
			 * guard this feeds only exists to stop a second request
			 * interleaving with an IN-FLIGHT drag. Parking in DONE
			 * meant the phase never returned to IDLE for the life of
			 * the machine, so a second system_powerdown was silently
			 * a no-op: if the first attempt did not halt the guest,
			 * every later one (including the app's quit-time flush)
			 * did nothing at all and the HFS+ catalog was lost. */
			nms->pwroff_phase = PWROFF_IDLE;
			if (trace) {
				fprintf(stderr, "[PWROFF] slider released; "
				                "waiting for the guest to halt\n");
			}
		}
		break;
	}

	default:
		break;
	}
}

static void ipod_touch_powerdown_req(Notifier *n, void *opaque)
{
	IPodTouchMachineState *nms = s_kbd_nms;

	if (!nms || !nms->pwroff_timer) {
		return;
	}
	if (nms->pwroff_phase != PWROFF_IDLE) {
		return;   /* a sequence is already running */
	}
	if (getenv("IT_PWROFF_TRACE")) {
		fprintf(stderr, "[PWROFF] home first, to get SpringBoard in front\n");
	}
	/*
	 * Clear the USB-cable bit while powering down.
	 *
	 * DISPROVEN RATIONALE, KEPT AS A WARNING. This was added believing that a
	 * real device refuses to power off while plugged in and that 3.1.3 was
	 * faithfully honouring that. **That is false.** Tested on real hardware
	 * (iPod touch 2G, MB528, build 7E18, tethered over USB the whole time):
	 * hold power, slide, and it powers off normally with the cable attached.
	 * So the guest's refusal here is OUR bug, not emulated fidelity.
	 *
	 * What is still true is the observation, not the explanation: with the cable
	 * bit asserted the guest sits in its power-source poll forever at 100% CPU
	 * -- reg 0x04 read as 0x08, two writes to 0x40, repeat, one PC -- and
	 * clearing the bit demonstrably advances it to 0x05/0x06/0x0a. It just does
	 * not finish, so this is a perturbation that moves the stall, not a fix.
	 *
	 * The better lead, from the same hardware test: our PMU answers the
	 * power-source block as 0x04=cable-bit-only, 0x05=0, 0x06=0, and the header
	 * notes AppleD1759PMUPowerSource reads 0x04..0x06 AS A BLOCK. Real silicon
	 * returns a coherent charger/battery state across all three. Returning zeros
	 * for two thirds of it is the likelier reason the driver never concludes,
	 * and it explains why toggling one bit in 0x04 changes where it hangs
	 * without ever letting it finish. Model 0x04..0x06 properly before touching
	 * anything else.
	 *
	 * Do NOT revisit the slide gesture: it is accepted and correct on 3.1.3
	 * (screendumped mid-drag, knob on the track). Two investigations died there.
	 */
	/* Home FIRST. The sheet the slide targets is SpringBoard's, so a
	 * powerdown requested while an app is foreground had nothing to slide and
	 * simply never completed -- which is precisely when a user hits quit. */
	if (s_kbd_mt) {
		ipod_touch_key_event(s_kbd_mt, KEY_H_DOWN);
	}
	nms->pwroff_phase = PWROFF_HOME;
	ipod_touch_powerdown_arm(nms, PWROFF_HOME_MS);
}

static Notifier ipod_touch_powerdown_notifier = {
	.notify = ipod_touch_powerdown_req,
};

/*
 * Host keyboard -> taps on the on-screen keyboard (IT_OSK=1).
 *
 * The guest-agent route (contrib/it-kbd-agent, QC_POLL_INPUT) cannot work in
 * general: on iPhone OS the focused text field and its UIKeyboardImpl live in
 * the frontmost APPLICATION's process, so an agent injected into SpringBoard
 * has nothing to insert into, and -[UIKeyboardImpl acceptInputString:] is a
 * stub on the 2.1.1 device UIKit anyway. Tapping iOS's own on-screen keyboard
 * sidesteps both: the taps go to whoever is frontmost, through the same path a
 * finger takes, so it needs no injected code and no armv6 toolchain.
 *
 * The cost is that the OSK must be visible, and that we have to track its page
 * and shift state. We only ever change those states ourselves, so tracking is
 * exact as long as the guest does not auto-capitalise behind our back - turn
 * "Auto-Capitalization" off in Settings > General > Keyboard (or bake
 * KeyboardAutocapitalization=false into the image) before relying on case.
 */
#define OSK_TAP_DOWN_MS 60    /* finger-down duration; long enough to register,
                                 short enough not to read as a long press      */
#define OSK_TAP_GAP_MS  140   /* between taps, so UIKit finishes each keystroke */

enum { OSK_IDLE = 0, OSK_DOWN, OSK_GAP };

/*
 * Key centres in panel pixels for the portrait QWERTY keyboard, 320x480.
 * The keyboard occupies the bottom 216 px (y 264..480); rows are 44 px apart.
 *
 * MEASURED from a real 2.1.1 keyboard (Notes, portrait) by locating the light
 * key faces in a screendump: row 1 has ten 32px-pitch keys centred on 32i+15,
 * row 2 nine on 32i+31, row 3 seven on 32i+63, rows 54px apart. Verified by
 * typing: 'qwerty 42' came out exactly right, 9/9 characters.
 */
#define OSK_ROW1_Y 296        /* q w e r t y u i o p */
#define OSK_ROW2_Y 350        /* a s d f g h j k l   */
#define OSK_ROW3_Y 404        /* shift z x c v b n m delete */
#define OSK_ROW4_Y 458        /* .?123  space  return */

#define OSK_SHIFT_X   24
#define OSK_DELETE_X 298
#define OSK_PAGE_X    30      /* ".?123" on the letters page, "ABC" on the other */
#define OSK_SPACE_X  160
#define OSK_RETURN_X 285

/* Row contents, in the order they appear on screen. */
static const char osk_alpha_row1[] = "qwertyuiop";
static const char osk_alpha_row2[] = "asdfghjkl";
static const char osk_alpha_row3[] = "zxcvbnm";
static const char osk_num_row1[]   = "1234567890";
static const char osk_num_row2[]   = "-/:;()$&@\"";
static const char osk_num_row3[]   = ".,?!'";

/*
 * Measured key centres. The three letter rows are each evenly spaced at a 32px
 * pitch but start at a different left offset, so index them by row rather than
 * deriving the offset from the key count.
 */
static const int osk_row_x0[4] = { 0, 15, 31, 63 };

static int osk_row_x(int index, int row)
{
	return osk_row_x0[row] + 32 * index;
}

/*
 * Where is `ch` on the keyboard? Returns true and fills in the tap position,
 * the page it lives on and whether shift must be latched.
 */
static bool osk_locate(uint16_t ch, int *x, int *y, bool *numeric, bool *shift)
{
	const char *p;
	char lower;

	*numeric = false;
	*shift = false;

	if (ch == ' ') {
		*x = OSK_SPACE_X; *y = OSK_ROW4_Y; return true;
	}
	if (ch == '\n') {
		*x = OSK_RETURN_X; *y = OSK_ROW4_Y; return true;
	}
	if (ch == 0x08) {
		*x = OSK_DELETE_X; *y = OSK_ROW3_Y; return true;
	}

	if (ch >= 'A' && ch <= 'Z') {
		*shift = true;
		lower = ch - 'A' + 'a';
	} else {
		lower = (char)ch;
	}

	if ((p = strchr(osk_alpha_row1, lower)) && lower) {
		*x = osk_row_x(p - osk_alpha_row1, 1); *y = OSK_ROW1_Y; return true;
	}
	if ((p = strchr(osk_alpha_row2, lower)) && lower) {
		*x = osk_row_x(p - osk_alpha_row2, 2); *y = OSK_ROW2_Y; return true;
	}
	if ((p = strchr(osk_alpha_row3, lower)) && lower) {
		*x = osk_row_x(p - osk_alpha_row3, 3); *y = OSK_ROW3_Y; return true;
	}

	*numeric = true;
	if ((p = strchr(osk_num_row1, (char)ch)) && ch) {
		*x = osk_row_x(p - osk_num_row1, 1); *y = OSK_ROW1_Y; return true;
	}
	if ((p = strchr(osk_num_row2, (char)ch)) && ch) {
		*x = osk_row_x(p - osk_num_row2, 2); *y = OSK_ROW2_Y; return true;
	}
	/*
	 * Row 3 of the NUMERIC page is deliberately not mapped. It is not the
	 * letters-page geometry - its leftmost key is "#+=", which switches to a
	 * THIRD page this state machine does not model. Reusing the letters-page
	 * coordinates here put a tap on "#+=", stranding the keyboard on the
	 * symbols page, after which every subsequent coordinate was wrong and
	 * characters landed silently in the wrong places (observed: typing "Zz"
	 * produced ".."). Failing loudly is much better than desynchronising.
	 *
	 * To support . , ? ! ' properly, measure that row on the numeric page and
	 * add it with its own offsets, exactly as the letter rows are handled.
	 */
	return false;   /* not typeable without modelling the #+= third page */
}

static void osk_push_tap(IPodTouchMachineState *nms, int x, int y)
{
	unsigned next = (nms->osk_t_tail + 1) % ARRAY_SIZE(nms->osk_tapx);

	if (next == nms->osk_t_head) {
		return;
	}
	nms->osk_tapx[nms->osk_t_tail] = x;
	nms->osk_tapy[nms->osk_t_tail] = y;
	nms->osk_t_tail = next;
}

/*
 * Turn the next queued character into taps, including whatever page and shift
 * changes it needs first. Returns false when nothing is queued.
 */
static bool osk_expand_next_char(IPodTouchMachineState *nms)
{
	int x, y;
	bool numeric, shift;
	uint16_t ch;

	while (nms->osk_p_head != nms->osk_p_tail) {
		ch = nms->osk_pending[nms->osk_p_head];
		nms->osk_p_head = (nms->osk_p_head + 1) % ARRAY_SIZE(nms->osk_pending);

		if (!osk_locate(ch, &x, &y, &numeric, &shift)) {
			continue;   /* character this keyboard cannot produce */
		}

		if (numeric != nms->osk_numeric) {
			osk_push_tap(nms, OSK_PAGE_X, OSK_ROW4_Y);
			nms->osk_numeric = numeric;
			/* switching page drops any latched shift */
			nms->osk_shifted = false;
		}
		if (!numeric && shift != nms->osk_shifted) {
			osk_push_tap(nms, OSK_SHIFT_X, OSK_ROW3_Y);
			nms->osk_shifted = shift;
		}

		osk_push_tap(nms, x, y);

		/* iOS's shift is one-shot: it releases itself after one character. */
		if (nms->osk_shifted) {
			nms->osk_shifted = false;
		}
		return true;
	}
	return false;
}

static void ipod_touch_osk_tick(void *opaque)
{
	IPodTouchMachineState *nms = opaque;
	int x, y;

	switch (nms->osk_phase) {
	case OSK_DOWN:
		/* lift the finger where we put it down */
		ipod_touch_synth_touch(nms, nms->osk_last_x, nms->osk_last_y, 0);
		nms->osk_phase = OSK_GAP;
		timer_mod(nms->osk_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
		                              (int64_t)OSK_TAP_GAP_MS * SCALE_MS);
		return;

	case OSK_GAP:
	case OSK_IDLE:
	default:
		break;
	}

	if (nms->osk_t_head == nms->osk_t_tail && !osk_expand_next_char(nms)) {
		nms->osk_phase = OSK_IDLE;
		return;
	}

	x = nms->osk_tapx[nms->osk_t_head];
	y = nms->osk_tapy[nms->osk_t_head];
	nms->osk_t_head = (nms->osk_t_head + 1) % ARRAY_SIZE(nms->osk_tapx);

	if (getenv("IT_OSK_TRACE")) {
		fprintf(stderr, "[OSK] tap (%d,%d)\n", x, y);
	}
	nms->osk_last_x = x;
	nms->osk_last_y = y;
	ipod_touch_synth_touch(nms, x, y, 1);
	nms->osk_phase = OSK_DOWN;
	timer_mod(nms->osk_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
	                              (int64_t)OSK_TAP_DOWN_MS * SCALE_MS);
}

static void ipod_touch_osk_enqueue(IPodTouchMachineState *nms, uint16_t ch)
{
	unsigned next = (nms->osk_p_tail + 1) % ARRAY_SIZE(nms->osk_pending);

	if (next == nms->osk_p_head) {
		return;   /* typing faster than the OSK can be tapped - drop */
	}
	nms->osk_pending[nms->osk_p_tail] = ch;
	nms->osk_p_tail = next;

	if (nms->osk_phase == OSK_IDLE) {
		timer_mod(nms->osk_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1);
	}
}

/* Ten devices here were only ever `qdev_new`ed and mapped, never realized.
 * An unrealized device is not parented into the QOM tree, so it is invisible
 * to BOTH machine reset and migration: a dc->reset on it would be dead code,
 * and its VMStateDescription would never be written to a snapshot. None of
 * them has a dc->realize hook, so this is purely tree membership. */
static void it_realize_into_qom_tree(DeviceState *dev)
{
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);
}

/*
 * A UART's Rx DMA REQUEST line, routed to the DMAC. `n` carries the request id
 * so one handler serves any port; `opaque` is the controller the port is wired
 * to. The line is high only while that port has Rx DMA selected and bytes
 * waiting, which is what keeps an idle port from feeding the DMAC an empty
 * FIFO -- see pl080_attach_paced_peripheral() below.
 */
static void it_uart_rx_dma_req(void *opaque, int n, int level)
{
    pl080_set_dma_request((PL080State *)opaque, n, level);
}

static void ipod_touch_machine_init(MachineState *machine)
{
	IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(machine);
	MemoryRegion *sysmem;
    AddressSpace *nsas;
    ARMCPU *cpu;

    if (!ipod_touch_boot_args_env_aliases(nms, &error_fatal) ||
        !ipod_touch_time_env_alias(nms, &error_fatal) ||
        !ipod_touch_bt_env_aliases(nms, &error_fatal)) {
        return;
    }
    ipod_touch_direct_boot_env_aliases(nms);
    ipod_touch_audio_env_alias(nms);
    ipod_touch_osk_env_alias(nms);
    ipod_touch_wdt_env_alias(nms);
    ipod_touch_h264_env_alias(nms);
    ipod_touch_scaler_env_alias(nms);
    ipod_touch_mpvd_env_alias(nms);
    ipod_touch_amc_env_alias(nms);
    ipod_touch_lcd_planes_env_alias(nms);
    ipod_touch_forge_sigcheck_env_alias(nms);
    ipod_touch_cpu_setup(machine, &sysmem, &cpu, &nsas);

    // setup clock
    nms->sysclk = clock_new(OBJECT(machine), "SYSCLK");
    clock_set_hz(nms->sysclk, 12000000ULL);

    nms->cpu = cpu;
    nms->nsas = nsas;

    // setup VICs
    nms->irq = g_malloc0(sizeof(qemu_irq *) * 2);
    DeviceState *uart1_dev;
    DeviceState *dev = pl192_manual_init("vic0", qdev_get_gpio_in(DEVICE(nms->cpu), ARM_CPU_IRQ), qdev_get_gpio_in(DEVICE(nms->cpu), ARM_CPU_FIQ), NULL);
    PL192State *s = PL192(dev);
    nms->vic0 = s;
    memory_region_add_subregion(sysmem, VIC0_MEM_BASE, &nms->vic0->iomem);
    nms->irq[0] = g_malloc0(sizeof(qemu_irq) * 32);
    for (int i = 0; i < 32; i++) { nms->irq[0][i] = qdev_get_gpio_in(dev, i); }

    dev = pl192_manual_init("vic1", NULL);
    s = PL192(dev);
    nms->vic1 = s;
    memory_region_add_subregion(sysmem, VIC1_MEM_BASE, &nms->vic1->iomem);
    nms->irq[1] = g_malloc0(sizeof(qemu_irq) * 32);
    for (int i = 0; i < 32; i++) { nms->irq[1][i] = qdev_get_gpio_in(dev, i); }

    // // chain VICs together
    nms->vic1->daisy = nms->vic0;

    // init clock 0
    dev = qdev_new("ipodtouch.clock");
    IPodTouchClockState *clock0_state = IPOD_TOUCH_CLOCK(dev);
    nms->clock0 = clock0_state;
    memory_region_add_subregion(sysmem, CLOCK0_MEM_BASE, &clock0_state->iomem);
    it_realize_into_qom_tree(dev);

    // init clock 1
    dev = qdev_new("ipodtouch.clock");
    IPodTouchClockState *clock1_state = IPOD_TOUCH_CLOCK(dev);
    nms->clock1 = clock1_state;
    memory_region_add_subregion(sysmem, CLOCK1_MEM_BASE, &clock1_state->iomem);
    it_realize_into_qom_tree(dev);

    // init the timer
    dev = qdev_new("ipodtouch.timer");
    IPodTouchTimerState *timer_state = IPOD_TOUCH_TIMER(dev);
    nms->timer1 = timer_state;
    timer_state->dilation = nms->time_dilation;
    memory_region_add_subregion(sysmem, TIMER1_MEM_BASE, &timer_state->iomem);
    SysBusDevice *busdev = SYS_BUS_DEVICE(dev);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_TIMER1_IRQ));
    //sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_TIMER1_IRQ - 1));
    timer_state->sysclk = nms->sysclk;
    it_realize_into_qom_tree(dev);

    // init sysic
    dev = qdev_new("ipodtouch.sysic");
    IPodTouchSYSICState *sysic_state = IPOD_TOUCH_SYSIC(dev);
    nms->sysic = sysic_state;
    sysic_state->direct_boot = nms->direct_iboot[0] != 0;
    memory_region_add_subregion(sysmem, SYSIC_MEM_BASE, &sysic_state->iomem);
    busdev = SYS_BUS_DEVICE(dev);
    for(int grp = 0; grp < GPIO_NUMINTGROUPS_2; grp++) {
        sysbus_connect_irq(busdev, grp, s5l8900_get_irq(nms, S5L8900_GPIO_IRQS[grp]));
    }
    /* Unrealized devices are never parented into the QOM tree, so their reset
     * handlers never run -- see the MIPI DSI note. */
    sysbus_realize(busdev, &error_fatal);

    // init GPIO
    dev = qdev_new("ipodtouch.gpio");
    IPodTouchGPIOState *gpio_state = IPOD_TOUCH_GPIO(dev);
    nms->gpio_state = gpio_state;
    memory_region_add_subregion(sysmem, GPIO_MEM_BASE, &gpio_state->iomem);
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);

    // init SDIO
    dev = qdev_new("ipodtouch.sdio");
    IPodTouchSDIOState *sdio_state = IPOD_TOUCH_SDIO(dev);
    nms->sdio_state = sdio_state;
    sdio_state->card_present = nms->wifi;
    memory_region_add_subregion(sysmem, SDIO_MEM_BASE, &sdio_state->iomem);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_SDIO_IRQ));
    if (nms->wifi) {
        /* Bridge the dongle's 802.3 channel to "-netdev ...,id=wifi0". */
        ipod_touch_sdio_setup_net(sdio_state);
    }

    dev = exynos4210_uart_create(UART0_MEM_BASE, 256, 0, serial_hd(0), nms->irq[0][24], nms->direct_iboot[0] != 0);
    if (!dev) {
        hw_error("Failed to create UART0 device!");
    }

    /*
     * UART1 carries the Bluetooth HCI (the DeviceTree puts a "bluetooth,n72"
     * node under /arm-io/uart1). Its Rx DMA request line is connected below,
     * once DMAC0 exists.
     */
    uart1_dev = exynos4210_uart_create(UART1_MEM_BASE, 256, 1,
                                       it_bt_chardev(serial_hd(1), nms->bt_enabled, nms->bt_latency_us),
                                       nms->irq[0][25], nms->direct_iboot[0] != 0);
    if (!uart1_dev) {
        hw_error("Failed to create UART1 device!");
    }

    dev = exynos4210_uart_create(UART2_MEM_BASE, 256, 2, serial_hd(2), nms->irq[0][26], nms->direct_iboot[0] != 0);
    if (!dev) {
        hw_error("Failed to create UART0 device!");
    }

    dev = exynos4210_uart_create(UART3_MEM_BASE, 256, 3, serial_hd(3), nms->irq[0][27], nms->direct_iboot[0] != 0);
    if (!dev) {
        hw_error("Failed to create UART0 device!");
    }

    // dev = exynos4210_uart_create(UART4_MEM_BASE, 256, 4, serial_hd(4), nms->irq[0][28], nms->direct_iboot[0] != 0);
    // if (!dev) {
    //     printf("Failed to create uart4 device!\n");
    //     abort();
    // }

    // init spis
    set_spi_base(0);
    dev = sysbus_create_simple("ipodtouch.spi", SPI0_MEM_BASE, s5l8900_get_irq(nms, S5L8720_SPI0_IRQ));
    IPodTouchSPIState *spi0_state = IPOD_TOUCH_SPI(dev);
    spi0_state->nor->nor_path = nms->nor_path;
    spi0_state->nor->boot_args = nms->boot_args;
    if (nms->nor_rw_path[0]) {
        ipod_touch_nor_spi_open_overlay(spi0_state->nor, nms->nor_rw_path, &error_fatal);
    }
    nms->spi0_state = spi0_state;
    qdev_connect_gpio_out(DEVICE(gpio_state), 0,
        qdev_get_gpio_in_named(DEVICE(spi0_state->nor), SSI_GPIO_CS, 0));

    set_spi_base(1);
    dev = sysbus_create_simple("ipodtouch.spi", SPI1_MEM_BASE, s5l8900_get_irq(nms, S5L8720_SPI1_IRQ));
    IPodTouchSPIState *spi1_state = IPOD_TOUCH_SPI(dev);
    nms->spi1_state = spi1_state;

    set_spi_base(2);
    sysbus_create_simple("ipodtouch.spi", SPI2_MEM_BASE, s5l8900_get_irq(nms, S5L8720_SPI2_IRQ));

    set_spi_base(3);
    sysbus_create_simple("ipodtouch.spi", SPI3_MEM_BASE, s5l8900_get_irq(nms, S5L8720_SPI3_IRQ));

    set_spi_base(4);
    dev = sysbus_create_simple("ipodtouch.spi", SPI4_MEM_BASE, s5l8900_get_irq(nms, S5L8720_SPI4_IRQ));
    IPodTouchSPIState *spi4_state = IPOD_TOUCH_SPI(dev);
    spi4_state->mt->sysic = sysic_state;
    spi4_state->mt->gpio_state = gpio_state;
    nms->spi4_state = spi4_state;

    // init the chip ID module
    dev = qdev_new("ipodtouch.chipid");
    IPodTouchChipIDState *chipid_state = IPOD_TOUCH_CHIPID(dev);
    nms->chipid_state = chipid_state;
    memory_region_add_subregion(sysmem, CHIPID_MEM_BASE, &chipid_state->iomem);
    it_realize_into_qom_tree(dev);

    // init the TVOut instance
    dev = qdev_new("ipodtouch.tvout");
    IPodTouchTVOutState *tvout_state = IPOD_TOUCH_TVOUT(dev);
    nms->tvout_state = tvout_state;
    memory_region_add_subregion(sysmem, TVOUT_MIXER1_MEM_BASE, &tvout_state->mixer1_iomem);
    memory_region_add_subregion(sysmem, TVOUT_MIXER2_MEM_BASE, &tvout_state->mixer2_iomem);
    memory_region_add_subregion(sysmem, TVOUT_SDO_MEM_BASE, &tvout_state->sdo_iomem);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_TVOUT_SDO_IRQ));
    sysbus_connect_irq(busdev, 1, s5l8900_get_irq(nms, S5L8720_TVOUT_VSYNC_IRQ));

    // init the unknown1 module
    dev = qdev_new("ipodtouch.unknown1");
    IPodTouchUnknown1State *unknown1_state = IPOD_TOUCH_UNKNOWN1(dev);
    memory_region_add_subregion(sysmem, UNKNOWN1_MEM_BASE, &unknown1_state->iomem);
    it_realize_into_qom_tree(dev);

    // init the watchdog timer (models reset so the guest can reboot itself)
    dev = qdev_new("ipodtouch.wdt");
    IPodTouchWDTState *wdt_state = IPOD_TOUCH_WDT(dev);
    wdt_state->noreset = nms->wdt_noreset;
    memory_region_add_subregion(sysmem, WDT_MEM_BASE, &wdt_state->iomem);
    it_realize_into_qom_tree(dev);

    // back the MPVD register window so the power-state path does not fault
    dev = qdev_new("ipodtouch.mpvd");
    IPodTouchMPVDState *mpvd_state = IPOD_TOUCH_MPVD(dev);
    qdev_prop_set_bit(dev, "decode", nms->mpvd_decode);
    memory_region_add_subregion(sysmem, MPVD_MEM_BASE, &mpvd_state->iomem);
    it_realize_into_qom_tree(dev);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, s5l8900_get_irq(nms, 45));

    if (nms->h264_decode) {
        sysbus_create_simple("ipodtouch.h264", H264_MEM_BASE, s5l8900_get_irq(nms, 35));
    }

    // init USB OTG
    dev = ipod_touch_init_usb_otg(s5l8900_get_irq(nms, S5L8720_USB_OTG_IRQ), s5l8720_usb_hwcfg);
    synopsys_usb_state *usb_otg = S5L8900USBOTG(dev);
    nms->usb_otg = usb_otg;
    if (nms->usb_tcp_addr[0]) {
        char *dup = g_strdup(nms->usb_tcp_addr);
        char *colon = strrchr(dup, ':');
        if (colon) {
            *colon = '\0';
            usb_otg->server_port = atoi(colon + 1);
        }
        if (!usb_otg->server_port) {
            usb_otg->server_port = 1235;
        }
        usb_otg->server_host = g_strdup(dup[0] ? dup : "127.0.0.1");
        g_free(dup);
    }
    /*
     * Unlike every other sysbus device here, this one was never realized, so
     * its reset handler never ran and none of the register defaults applied -
     * FIFO sizes and GRSTCTL all read back as zero. AHBIDLE reading clear is
     * what made AppleSynopsysOTG2::_coreInit panic with "AHB not idle".
     */
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion(sysmem, USBOTG_MEM_BASE, &nms->usb_otg->iomem);

    // init two pl080 DMAC0 devices
    dev = qdev_new("pl080");
    PL080State *pl080_1 = PL080(dev);
    object_property_set_link(OBJECT(dev), "downstream", OBJECT(sysmem), &error_fatal);
    memory_region_add_subregion(sysmem, DMAC0_MEM_BASE, &pl080_1->iomem1);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize(busdev, &error_fatal);
    /*
     * The UART RECEIVE request lines. Without these the model invents data that
     * was never received, and the invented completion is what made DMAC0's
     * interrupt undeliverable.
     *
     * The kernel's serial driver arms a 2048-byte peripheral-to-memory channel
     * on the console UART's URXH with the terminal-count interrupt enabled and
     * leaves it armed. On real hardware that channel moves a byte only when the
     * UART asserts its DMA request, so an idle console never completes it and
     * never raises a terminal count. This model ignored the request lines, so
     * the whole 2048 bytes were "read" out of an empty FIFO inside the single
     * Config write, terminal count fired immediately, and -- because the driver
     * polls for this transfer rather than acknowledging it -- the pending bit
     * sat in IntTCStatus for the rest of the boot with the interrupt line held
     * high. Deliver that line to the kernel (which is what audio needs, since
     * its channel is on this same controller) and the serial driver services a
     * completion for a read that never happened, spins ~32000 register polls
     * waiting for data, times out, rearms and repeats forever: measured as a
     * boot that stops at the Apple logo, which is the wedge every previous
     * attempt to route this interrupt correctly ran into.
     *
     * Declaring the lines paced is the honest model: nothing drives them, so an
     * idle serial port produces no bytes and no interrupt, exactly as on real
     * hardware. (If a chardev with real input is ever wired up, the UART model
     * should drive these through pl080_set_dma_request().)
     */
    pl080_attach_paced_peripheral(pl080_1, UART0_RX_DMA_REQ_ID);
    pl080_attach_paced_peripheral(pl080_1, UART1_RX_DMA_REQ_ID);
    /*
     * ...and now one of them IS driven. UART1's Rx path is DMA: the Bluetooth
     * driver arms a peripheral->memory channel on URXH and never reads the
     * register itself, so bytes we put in the Rx FIFO sat there forever and
     * BTServer retried HCI_Reset every 10 s for the whole session. Every
     * BluetoothManager call in the guest then blocked for its full client-side
     * timeout (~1 s), which is what stalls SpringBoard's status bar and eats
     * the app-launch animation.
     */
    sysbus_connect_irq(SYS_BUS_DEVICE(uart1_dev), 2,
                       qemu_allocate_irq(it_uart_rx_dma_req, pl080_1,
                                         UART1_RX_DMA_REQ_ID));
    /*
     * DMAC0's completion IRQ is VIC line 0x10, and DMAC1's is 0x11. They are
     * NOT a shared line: our own 3.1.3 DeviceTree says so directly --
     * /arm-io/dmac0 has interrupts = <0x10> and /arm-io/dmac1 has <0x11>, both
     * with compatible = "dmac,pl080".
     *
     * This used to put DMAC0 on 0x11 under IT_DIRECT_IBOOT, on the theory that
     * the 3.1.3 NAND stack blocked on it and root would not mount on 0x10. That
     * was a misattribution. What actually happened on 0x10 was the wedge
     * described at the paced UART receive lines above: DMAC0's line was held
     * high by a terminal count invented for a serial read that never happened,
     * so the kernel never got out of the handler and the boot stopped at the
     * Apple logo -- which looks exactly like "root did not mount". With the
     * receive lines paced and the interrupt masks no longer sticky (see
     * pl080_refresh_masks in hw/dma/pl080.c), 0x10 boots and reboots normally
     * and the kernel services DMAC0 for the first time: measured over one boot,
     * 16 IntStatus reads and 32 IntTCClear writes on DMAC0, against zero on
     * 0x11, with DMAC1's own servicing unchanged.
     *
     * Putting DMAC0 back on 0x11 also silently broke DMAC1, because
     * s5l8900_get_irq() hands both devices the SAME qemu_irq and qemu_set_irq
     * is last-writer-wins, so either controller's deassertion dropped the
     * other's pending interrupt. Audio's DMA channel is channel 5 on DMAC0, so
     * its per-period terminal counts went the same way.
     *
     * IT_DMAC0_IRQ=<decimal> still overrides the line, for bisecting.
     */
    int dmac0_irq = S5L8720_DMAC0_IRQ;
    if (getenv("IT_DMAC0_IRQ")) {
        dmac0_irq = atoi(getenv("IT_DMAC0_IRQ"));
    }
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, dmac0_irq));

    dev = qdev_new("pl080");
    PL080State *pl080_2 = PL080(dev);
    object_property_set_link(OBJECT(dev), "downstream", OBJECT(sysmem), &error_fatal);
    memory_region_add_subregion(sysmem, DMAC1_0_MEM_BASE, &pl080_2->iomem1);
    memory_region_add_subregion(sysmem, DMAC1_1_MEM_BASE, &pl080_2->iomem2);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_DMAC1_IRQ));

    // Init I2C0
    dev = qdev_new("ipodtouch.i2c");
    IPodTouchI2CState *i2c_state = IPOD_TOUCH_I2C(dev);
    i2c_state->base = 0;
    nms->i2c0_state = i2c_state;
    busdev = SYS_BUS_DEVICE(dev);
    memory_region_add_subregion(sysmem, I2C0_MEM_BASE, &i2c_state->iomem);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_I2C0_IRQ));

    // init the PMU
    I2CSlave * pmu = i2c_slave_create_simple(i2c_state->bus, "pcf50633", 0x73);
    spi4_state->mt->pmu = PCF50633(pmu);
    PCF50633(pmu)->usb_cable = nms->usb_attached;
    nms->pmu_state = PCF50633(pmu);
    pcf50633_set_battery_adc(nms->pmu_state, nms->battery_adc);
    pcf50633_set_battery_drain(nms->pmu_state, nms->battery_drain);
    nms->pmu_state->charging_mode = nms->battery_charging;
    qdev_connect_gpio_out(DEVICE(pmu), 0,
                         qdev_get_gpio_in(DEVICE(sysic_state), PMU_WAKE_IRQ));
    ipod_touch_mbx_set_patch_usb_gate(nms->usb_patch_mux_gate);

    // init the accelerometer. Keep the handle so the machine's QMP properties
    // (accel-orientation / accel-x/y/z / accel-shake, added in instance_init)
    // can drive it, e.g.  qom-set path=/machine property=accel-orientation value=3
    I2CSlave *accelerometer = i2c_slave_create_simple(i2c_state->bus, "lis302dl", 0x1D);
    nms->lis302dl_state = LIS302DL(accelerometer);
    lis302dl_apply_attitude(nms->lis302dl_state, nms->accel_pitch, nms->accel_roll, nms->accel_flat);
    nms->lis302dl_state->rate_hz = nms->accel_rate_hz;

    /*
     * Simulated "demo card" / AppleTetheredDevice.  The N72AP DeviceTree has an
     * I2C node at i2c0/0x29 (compatible "tethered,tethereddevice"); the kext's
     * probe reads one byte and requires 0x82 before it reports a card present.
     * Present it only when asked, so ordinary runs stay untethered.
     */
    if (getenv("IT_TETHERED") != NULL) {
        i2c_slave_create_simple(i2c_state->bus, TYPE_IPOD_TOUCH_TETHERED, 0x29);
    }

    // init the audio codec (CS42L58, device tree /arm-io/i2c0/audio0) and the
    // LM48821 speaker amp (/arm-io/i2c0/spkr-amp)
    if (ipod_touch_audio_hw_enabled(nms)) {
        I2CSlave *codec = i2c_slave_create_simple(i2c_state->bus, "cs42l58", 0x4A);
        I2CSlave *amp = i2c_slave_create_simple(i2c_state->bus, "lm48821", 0x76);

        /*
         * I2S0. The TX FIFO at +0x10 is a PL080 DMA target (dma-parent is
         * dmac0), so PCM arrives as ordinary MMIO writes from the DMA engine.
         *
         * The interrupt goes through the *GPIO* controller, not the VIC. The
         * device tree says i2s0 has interrupts=<0x2c> but its interrupt-parent
         * is the GPIO IC -- the same numbering the multi-touch (0x6d -> group
         * 3, bit 13) and the buttons use -- so 0x2c means GPIO group 1, bit 12,
         * and wiring it as VIC line 0x2c would target an unrelated device.
         *
         * It is NOT optional, contrary to what this comment used to say. The
         * driver enables that source and sleeps on it for 10 s before it will
         * program a PL080 channel; measured with a gdbstub breakpoint, that
         * sleep always timed out and the channel start returned
         * kIOReturnNotReady. The I2S model raises it itself (see
         * it_i2s_arm_ready), which is why it needs the sysic handle.
         */
        dev = qdev_new(TYPE_IPOD_TOUCH_I2S);
        IPOD_TOUCH_I2S(dev)->amplifier = LM48821(amp);
        qdev_connect_clock_in(dev, "lrclk",
                              qdev_get_clock_out(DEVICE(codec), "lrclk"));
        busdev = SYS_BUS_DEVICE(dev);
        IPOD_TOUCH_I2S(dev)->sysic = sysic_state;
        /*
         * The TX DMA request line back to dmac0. The driver programs the
         * channel with Config 0x00008a81 -- flow type 1 (memory to peripheral,
         * DMAC as flow controller) with destination peripheral id 10 -- so
         * request line 10 on dmac0 is this FIFO's, and without it the PL080
         * drains the guest's whole 72 KB audio ring in zero guest time, before
         * the audio stack has written a single sample into it. See the pacing
         * comment in ipod_touch_i2s.c.
         */
        IPOD_TOUCH_I2S(dev)->dmac = pl080_1;
        IPOD_TOUCH_I2S(dev)->dma_req_id = I2S0_DMA_REQ_ID;
        pl080_attach_paced_peripheral(pl080_1, I2S0_DMA_REQ_ID);
        sysbus_realize(busdev, &error_fatal);
        memory_region_add_subregion(sysmem, I2S0_MEM_BASE,
                                    &IPOD_TOUCH_I2S(dev)->iomem);
    }

    // Init I2C1
    dev = qdev_new("ipodtouch.i2c");
    i2c_state = IPOD_TOUCH_I2C(dev);
    nms->i2c1_state = i2c_state;
    i2c_state->base = 1;
    busdev = SYS_BUS_DEVICE(dev);
    memory_region_add_subregion(sysmem, I2C1_MEM_BASE, &i2c_state->iomem);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_I2C1_IRQ));
    
    // Init the light sensor
    I2CSlave *isl29003dl = i2c_slave_create_simple(i2c_state->bus, "isl29003dl", 0x44);

    // init the Mikey
    I2CSlave *cd327mikey = i2c_slave_create_simple(i2c_state->bus, "cd3272mikey", 0x39);

    /* AMC (audio media codec) -- see ipod_touch_audio_hw_enabled(). */
    if (ipod_touch_audio_hw_enabled(nms)) {
        dev = qdev_new(TYPE_IPOD_TOUCH_AMC);
        qdev_prop_set_uint8(dev, "mode", nms->amc_mode);
        busdev = SYS_BUS_DEVICE(dev);
        sysbus_realize(busdev, &error_fatal);
        memory_region_add_subregion(sysmem, AMC_MEM_BASE,
                                    &IPOD_TOUCH_AMC(dev)->iomem);
        sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_AMC_IRQ));
    }

    // init the FMSS flash controller
    dev = qdev_new("ipodtouch.fmss");
    IPodTouchFMSSState *fmss_state = IPOD_TOUCH_FMSS(dev);
    fmss_state->nand_path = nms->nand_path;
    fmss_state->nand_overlay = nms->nand_overlay[0] ? nms->nand_overlay : NULL;
    nms->fmss_state = fmss_state;
    fmss_state->direct_boot = nms->direct_iboot[0] != 0;
    busdev = SYS_BUS_DEVICE(dev);
    memory_region_add_subregion(sysmem, FMSS_MEM_BASE, &fmss_state->iomem);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_FMSS_IRQ));

    // init the USB module
    dev = qdev_new("ipodtouch.usbphys");
    IPodTouchUSBPhysState *usb_phys_state = IPOD_TOUCH_USB_PHYS(dev);
    nms->usb_phys_state = usb_phys_state;
    memory_region_add_subregion(sysmem, USBPHYS_MEM_BASE, &usb_phys_state->iomem);
    it_realize_into_qom_tree(dev);

    ipod_touch_memory_setup(machine, sysmem, nsas);

    // init the MIPI SDI controller
    dev = qdev_new("ipodtouch.mipidsi");
    IPodTouchMIPIDSIState *mipi_dsi_state = IPOD_TOUCH_MIPI_DSI(dev);
    nms->mipi_dsi_state = mipi_dsi_state;
    mipi_dsi_state->direct_boot = nms->direct_iboot[0] != 0;
    memory_region_add_subregion(sysmem, MIPI_DSI_MEM_BASE, &mipi_dsi_state->iomem);
    /* Has to be realized, not just created: an unrealized device is never
     * parented into the QOM tree, so qemu_devices_reset() never reaches it and
     * its DeviceClass reset handler is dead code. Without this the DSI link
     * kept the previous boot's panel-ID latch across a warm reset and the panel
     * was never brought up on the second boot. */
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);

    // init LCD
    dev = qdev_new("ipodtouch.lcd");
    IPodTouchLCDState *lcd_state = IPOD_TOUCH_LCD(dev);
    qdev_prop_set_bit(dev, "planes", nms->lcd_planes);
    lcd_state->sysmem = sysmem;
    lcd_state->mt = spi4_state->mt;
    nms->lcd_state = lcd_state;
    busdev = SYS_BUS_DEVICE(dev);
    memory_region_add_subregion(sysmem, DISPLAY_MEM_BASE, &lcd_state->iomem);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_LCD_IRQ));

    if (nms->scaler_decode) {
        dev = qdev_new("ipodtouch.scaler");
        busdev = SYS_BUS_DEVICE(dev);
        sysbus_realize(busdev, &error_fatal);
        sysbus_mmio_map(busdev, 0, SCALER_CSC_MEM_BASE);
        sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, 0x25));
    } else {
        create_unimplemented_device("scaler-csc", SCALER_CSC_MEM_BASE, 0x1000);
    }

    // init SHA1 engine
    dev = qdev_new("ipodtouch.sha1");
    IPodTouchSHA1State *sha1_state = IPOD_TOUCH_SHA1(dev);
    nms->sha1_state = sha1_state;
    busdev = SYS_BUS_DEVICE(dev);
    memory_region_add_subregion(sysmem, SHA1_MEM_BASE, &sha1_state->iomem);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_SHA1_IRQ));

    // init AES engine
    dev = qdev_new("ipodtouch.aes");
    IPodTouchAESState *aes_state = IPOD_TOUCH_AES(dev);
    nms->aes_state = aes_state;
    memory_region_add_subregion(sysmem, AES_MEM_BASE, &aes_state->iomem);
    it_realize_into_qom_tree(dev);

    // init PKE engine
    dev = qdev_new("ipodtouch.pke");
    IPodTouchPKEState *pke_state = IPOD_TOUCH_PKE(dev);
    nms->pke_state = pke_state;
    pke_state->sha1 = nms->sha1_state;
    qdev_prop_set_bit(dev, "forge-sigcheck", nms->forge_sigcheck);
    memory_region_add_subregion(sysmem, PKE_MEM_BASE, &pke_state->iomem);
    it_realize_into_qom_tree(dev);

    // init the MBX
    dev = qdev_new("ipodtouch.mbx");
    IPodTouchMBXState *mbx_state = IPOD_TOUCH_MBX(dev);
    nms->mbx_state = mbx_state;
    mbx_state->irq_enabled = nms->mbx_irq;
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, s5l8900_get_irq(nms, S5L8720_MBX_IRQ));
    memory_region_add_subregion(sysmem, MBX1_MEM_BASE, &mbx_state->iomem1);
    memory_region_add_subregion(sysmem, MBX2_MEM_BASE, &mbx_state->iomem2);

    qemu_register_reset(ipod_touch_cpu_reset, nms);

    /*
     * Route the host keyboard through the modern input handler so we can see
     * the Command modifier (a 0xE0-prefixed extended scancode the legacy path
     * mangles) and tell button combos apart from text. See ipod_touch_kbd_event.
     */
    s_kbd_nms = nms;
    s_kbd_mt = spi4_state->mt;
    qemu_input_handler_register(DEVICE(nms->cpu), &ipod_touch_kbd_handler);

    /*
     * system_powerdown -> the slide-to-power-off gesture (see
     * ipod_touch_powerdown_tick). This is what gives the guest a chance to
     * unmount its filesystems before the machine stops.
     */
    nms->osk_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                  ipod_touch_osk_tick, nms);
    nms->osk_phase = OSK_IDLE;

    nms->pwroff_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                     ipod_touch_powerdown_tick, nms);
    nms->pwroff_phase = PWROFF_IDLE;
    qemu_register_powerdown_notifier(&ipod_touch_powerdown_notifier);
}

static void ipod_touch_machine_class_init(ObjectClass *klass, void *data)
{
    object_class_property_add(klass, "bt", "bool", ipod_touch_get_bt,
                              ipod_touch_set_bt, NULL, NULL);
    object_class_property_set_description(klass, "bt", "Emulated Bluetooth HCI controller (default on)");
    object_class_property_add(klass, "bt-latency-us", "uint32", ipod_touch_get_bt_latency_us,
                              ipod_touch_set_bt_latency_us, NULL, NULL);
    object_class_property_set_description(klass, "bt-latency-us", "HCI reply delay in microseconds (default 2000)");
    object_class_property_add(klass, "time-dilation", "uint32", ipod_touch_get_time_dilation,
                              ipod_touch_set_time_dilation, NULL, NULL);
    object_class_property_set_description(klass, "time-dilation", "Timer-4 interrupt interval multiplier (1..1000000)");
    object_class_property_add(klass, "h264-decode", "bool", ipod_touch_get_h264_decode,
                              ipod_touch_set_h264_decode, NULL, NULL);
    object_class_property_set_description(klass, "h264-decode", "Enable hardware H.264 decoding instead of the legacy RAM register window");
    object_class_property_add(klass, "scaler-decode", "bool", ipod_touch_get_scaler_decode,
                              ipod_touch_set_scaler_decode, NULL, NULL);
    object_class_property_set_description(klass, "scaler-decode", "Enable scaler color conversion instead of the legacy register stub");
    object_class_property_add(klass, "mpvd-decode", "bool", ipod_touch_get_mpvd_decode,
                              ipod_touch_set_mpvd_decode, NULL, NULL);
    object_class_property_set_description(klass, "mpvd-decode", "Enable MPEG-4 Part 2 decoding instead of register-only MPVD");
    object_class_property_add_str(klass, "amc-mode", ipod_touch_get_amc_mode, ipod_touch_set_amc_mode);
    object_class_property_set_description(klass, "amc-mode", "AMC registers, handshake-only bring-up, or compressed audio decode");
    object_class_property_add_str(klass, "direct-iboot", ipod_touch_get_direct_iboot, ipod_touch_set_direct_iboot);
    object_class_property_add_str(klass, "direct-llb", ipod_touch_get_direct_llb, ipod_touch_set_direct_llb);
    object_class_property_add(klass, "forge-sigcheck", "bool", ipod_touch_get_forge_sigcheck,
                              ipod_touch_set_forge_sigcheck, NULL, NULL);
    object_class_property_set_description(klass, "forge-sigcheck", "Allow malformed boot signature recovery for unsigned-image compatibility");
    object_class_property_add(klass, "lcd-planes", "bool", ipod_touch_get_lcd_planes,
                              ipod_touch_set_lcd_planes, NULL, NULL);
    object_class_property_set_description(klass, "lcd-planes", "Enable LCD multi-plane composition");
    object_class_property_add(klass, "wdt-noreset", "bool", ipod_touch_get_wdt_noreset,
                              ipod_touch_set_wdt_noreset, NULL, NULL);
    object_class_property_set_description(klass, "wdt-noreset", "Suppress guest watchdog reset commands for debugging");
    object_class_property_add(klass, "osk", "bool", ipod_touch_get_osk,
                              ipod_touch_set_osk, NULL, NULL);
    object_class_property_set_description(klass, "osk",
        "Type host keys by tapping the legacy on-screen keyboard");
    object_class_property_add(klass, "audio-hw", "OnOffAuto", ipod_touch_get_audio_hw,
                              ipod_touch_set_audio_hw, NULL, NULL);
    object_class_property_set_description(klass, "audio-hw",
        "CS42L58 codec and AMC hardware (auto: enabled for direct iBoot)");
    MachineClass *mc = MACHINE_CLASS(klass);
    mc->desc = "iPod Touch";
    mc->init = ipod_touch_machine_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm1176");
}

static const TypeInfo ipod_touch_machine_info = {
    .name          = TYPE_IPOD_TOUCH_MACHINE,
    .parent        = TYPE_MACHINE,
    .instance_size = sizeof(IPodTouchMachineState),
    .class_size    = sizeof(IPodTouchMachineClass),
    .class_init    = ipod_touch_machine_class_init,
    .instance_init = ipod_touch_instance_init,
    .instance_finalize = ipod_touch_instance_finalize,
};

static void ipod_touch_machine_types(void)
{
    type_register_static(&ipod_touch_machine_info);
}

type_init(ipod_touch_machine_types)
