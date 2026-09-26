#include "hw/arm/ipod_touch_lcd.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "ui/pixel_ops.h"
#include "ui/console.h"
#include "hw/display/framebuffer.h"
#include "exec/cpu-common.h"
#include "qemu/log.h"

static int lcd_brightness = 255;

bool lcd_backlight_is_off(void)
{
    return qatomic_read(&lcd_brightness) == 0;
}

#define LCD_FB_WIDTH  320
#define LCD_FB_HEIGHT 480

/*
 * IT_LCD_FRAMETRACE: one line per frame-pipeline event, with both clocks.
 *
 * Frame timing on this machine has three stages and they are paced by three
 * different things, so a trace that only records one of them cannot tell a
 * dropped guest frame from a host sampling artefact:
 *
 *   vsync  the model's 60 Hz frame interrupt to the guest
 *   flip   the guest writing a new scanout base to reg 0x24 (a present)
 *   scan   this model reading the framebuffer out to the host window
 *
 * Timestamps are taken here rather than on the host's stderr reader because
 * pipe buffering adds tens of milliseconds of jitter to a 16 ms signal.
 */
static int lcd_frametrace(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("IT_LCD_FRAMETRACE") ? 1 : 0;
    }
    return on;
}

/*
 * The same caching for the two flags consulted from the MMIO handlers. Both
 * sit on paths the guest hits constantly -- IT_LCD_READY guards registers this
 * file's own comment describes as "polled thousands of times", and LCD_TRACE is
 * read on every single register write -- and getenv() is a linear scan of
 * environ each time, run synchronously on the vCPU thread. None of these are
 * meant to be togglable mid-run.
 */
static int lcd_ready_hack(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("IT_LCD_READY") ? 1 : 0;
    }
    return on;
}

static int lcd_trace(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("LCD_TRACE") ? 1 : 0;
    }
    return on;
}


/*
 * IT_VSYNC_DIVISOR: deliver the panel's frame interrupt every Nth period.
 *
 * The panel signals 60 Hz because that is what the hardware did, but under an
 * interpreter the guest may only manage one or two frames a second. It still
 * takes all sixty interrupts and runs the whole compositing path for each,
 * discovering only at the end that it is out of time -- so the great majority
 * of that work is thrown away, and the frames it does finish are late. Asking
 * for fewer frames it can actually complete may produce more of them.
 */
static int lcd_vsync_divisor(void)
{
    static int n = -1;
    if (n < 0) {
        const char *v = getenv("IT_VSYNC_DIVISOR");
        n = v ? atoi(v) : 1;
        if (n < 1) {
            n = 1;
        }
    }
    return n;
}

/* True while refresh_timer_tick is driving a frame out; see lcd_refresh(). */
static bool lcd_in_vsync_present;

static int lcd_vsync_legacy(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("IT_LCD_VSYNC_LEGACY") ? 1 : 0;
    }
    return on;
}

static int lcd_vsync_trace(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("IT_LCD_VSYNC_TRACE") ? 1 : 0;
    }
    return on;
}

static void lcd_ft(const char *ev, uint32_t arg)
{
    if (!lcd_frametrace()) {
        return;
    }
    fprintf(stderr, "[FT] %s %" PRId64 " %" PRId64 " 0x%08x\n", ev,
            qemu_clock_get_ns(QEMU_CLOCK_REALTIME),
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), arg);
}

/*
 * Rotation the host window should be presenting, in degrees clockwise. Written
 * by it_display_set_orientation() (accelerometer / Command+Left / Command+Right
 * / the accel-orientation QMP property) and picked up by the next refresh,
 * which owns the console resize.
 *
 * iOS keeps its framebuffer in portrait and rotates the UI *inside* it, so to
 * show an upright landscape image the model has to turn the picture back the
 * same way the user "turned" the device.
 */
static int it_display_rotation_req;

/* 7E18 AppleM2CLCD enables sources at +8 and acknowledges +0xc with W1C.
 * Its idle path clears enable bit 0; a constant status of 1 and an interrupt
 * driven by the last acknowledgement caused unexpected interrupts at 60 Hz. */
static void lcd_update_irq(IPodTouchLCDState *s)
{
    qemu_set_irq(s->irq, (s->irq_status & s->irq_enable) != 0);
}

static void lcd_write_irq(IPodTouchLCDState *s, unsigned offset, uint32_t value)
{
    if (offset == 8) {
        s->irq_enable = value;
    } else {
        s->render = value; /* Preserve the old snapshot wire field. */
        s->irq_status &= ~value;
    }
    lcd_update_irq(s);
}

static void lcd_vblank_irq(IPodTouchLCDState *s)
{
    s->irq_status |= 1;
    lcd_update_irq(s);
}

static void lcd_restore_irq(IPodTouchLCDState *s, int version)
{
    if (version < 3) {
        s->irq_enable = version >= 2 ? s->plane_regs[2] : (s->render == 1);
        s->irq_status = 0;
    }
    lcd_update_irq(s);
}

static uint64_t ipod_touch_lcd_read(void *opaque, hwaddr addr, unsigned size)
{
    // printf("%s: read from location 0x%08x\n", __func__, addr);

    IPodTouchLCDState *s = (IPodTouchLCDState *)opaque;
    if (s->planes_enabled && !(addr & 3) && addr >= 0x10 &&
        addr < sizeof(s->plane_regs)) return s->plane_regs[addr / 4];
    switch(addr)
    {
        case 0x0:
            return 2;
        case 0x4:
            return s->lcd_con;
        case 0x8:
            return s->irq_enable;
        case 0xC:
            return s->irq_status;
        case 0x20:
            return s->w1_display_depth_info;
        case 0x24:
            return s->w1_framebuffer_base;
        case 0x28:
            return s->w1_hspan;
        case 0x30:
            return s->w1_display_resolution_info;
        case 0x1b10:
            return 2;
	case 0x1b14:
	    return 0x3;
        default:
            /*
             * IT_LCD_READY: report "ready" for every register this model does
             * not implement. 3.1.3's AppleM2TVOut close path sleeps waiting on
             * three independent ready handshakes (SDO_CLKCON & 0x2,
             * enable.reg.clkgating_rdy, mix_ctrl.reg.reg_mixer_ready_clk_down)
             * and logs "TVOUT SHUT DOWN PROBLEM: ..." when they never assert.
             * That wait happens on the SpringBoard thread, so the UI never
             * comes up. Reads seen spinning here: 0x410 (status beside the
             * 0x408/0x40c indirect address/data port, polled thousands of
             * times) and the 0x1b80..0x1bb0 mixer/SDO status block.
             *
             * Returning all-ones satisfies any `status & mask` readiness test.
             * It is a bring-up probe, not a model: once the specific bits are
             * known they should be implemented properly per register.
             */
            if (lcd_ready_hack()) {
                return 0xffffffff;
            }
            qemu_log_mask(LOG_UNIMP, "%s: read invalid location 0x%08x.\n",
                          __func__, (unsigned)addr);
            break;
    }
    return 0;
}

static void ipod_touch_lcd_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IPodTouchLCDState *s = (IPodTouchLCDState *)opaque;
    // printf("%s: writing 0x%08x to 0x%08x\n", __func__, val, addr);

    if (lcd_trace()) {
        fprintf(stderr, "[LCD] wr [0x%04x] <- 0x%08x\n", (unsigned)addr, (unsigned)val);
    }

    if (!(addr & 3) && addr < sizeof(s->plane_regs)) s->plane_regs[addr / 4] = val;
    switch(addr) {
        case 0x4:
            s->lcd_con = val;
            break;
        case 0x8:
        case 0xC:
            lcd_write_irq(s, addr, val);
            break;
        case 0x20:
            s->w1_display_depth_info = val;
            break;
        case 0x24:
            if (val != s->w1_framebuffer_base) {
                lcd_ft("flip", (uint32_t)val);
            }
            s->w1_framebuffer_base = val;
            break;
        case 0x28:
            s->w1_hspan = val;
            break;
        case 0x30:
            s->w1_display_resolution_info = val;
            break;
    }
}

void lcd_changebrightness(int brightness)
{
    qatomic_set(&lcd_brightness, brightness & 0xFF);
    if (lcd_trace()) {
        fprintf(stderr, "[LCD] brightness <- %d\n", lcd_brightness);
    }
}

/*
 * IT_LCD_BRIGHT overrides the panel backlight level the guest programmed.
 * draw_line32_32 scales every channel by lcd_brightness/255, so a guest that
 * leaves the backlight at 0 produces a solid-black screendump even when the
 * framebuffer is full of pixels. Useful both as a probe and for guests whose
 * backlight register we do not model.
 */
/*
 * Set only for the duration of a screen COPY. The backlight scaling is lossy --
 * at a low level most of the framebuffer's bits are gone by the time the pixel
 * is written -- so a legible copy has to be RENDERED at full exposure, not
 * brightened afterwards. See qemu_display_register_capture_exposure().
 */
static bool lcd_capture_exposure;

static void lcd_set_capture_exposure(bool on)
{
    lcd_capture_exposure = on;
}

static int lcd_bright_effective(void)
{
    static int ovr = -2;
    if (lcd_capture_exposure) {
        return 255;
    }
    if (ovr == -2) {
        const char *v = getenv("IT_LCD_BRIGHT");
        ovr = v ? atoi(v) : -1;
    }
    return ovr >= 0 ? ovr : lcd_brightness;
}

void it_display_set_orientation(uint32_t orientation)
{
    int rot;

    switch (orientation) {
    case 2:  rot = 180; break;   /* portrait upside down */
    case 3:  rot = 270; break;   /* landscape left  - device turned ccw */
    case 4:  rot = 90;  break;   /* landscape right - device turned cw  */
    default: rot = 0;   break;   /* portrait, face up/down, unset */
    }
    if (rot != it_display_rotation_req) {
        it_display_rotation_req = rot;
        if (lcd_trace()) {
            fprintf(stderr, "[LCD] orientation %u -> window rotation %d\n",
                    orientation, rot);
        }
    }
}

/*
 * Brightness as a lookup table.
 *
 * The panel's backlight scales every channel of every pixel, which used to
 * mean a getenv-backed accessor call and three float multiply-and-rounds per
 * pixel -- 153,600 pixels a frame, up to ninety frames a second, on the
 * iothread with the BQL held. The arithmetic only has 256 possible answers.
 */
static uint8_t lcd_bright_lut[256];
static int lcd_bright_lut_for = -1;

static void lcd_bright_lut_sync(int bri)
{
    int i;

    if (lcd_bright_lut_for == bri) {
        return;
    }
    for (i = 0; i < 256; i++) {
        lcd_bright_lut[i] = (uint8_t)lround(i * (double)bri / 255.0);
    }
    lcd_bright_lut_for = bri;
}

static void lcd_invalidate(void *opaque)
{
    IPodTouchLCDState *s = opaque;
    s->invalidate = 1;
}

/*
 * Set for a frame whose conversion is the identity: rgb_to_pixel32() packs
 * 0x00RRGGBB, which is byte-for-byte the guest's little-endian BGRX pixel, so
 * at full backlight into an x8r8g8b8 surface the "conversion" is a copy with
 * the ignored top byte zeroed. lcd_refresh() owns this; see there for why the
 * surface format has to be checked and not assumed.
 */
static bool lcd_line_is_copy;

static void draw_line32_32(void *opaque, uint8_t *d, const uint8_t *s, int width, int deststep)
{
    uint32_t *dp = (uint32_t *)d;

    if (lcd_line_is_copy) {
        memcpy(dp, s, (size_t)width * 4);
        return;
    }

    /* The LUT is synchronised once per frame by lcd_refresh(). */
    do {
        *dp++ = rgb_to_pixel32(lcd_bright_lut[s[2]], lcd_bright_lut[s[1]],
                               lcd_bright_lut[s[0]]);
        s += 4;
    } while (-- width != 0);
}

/* 7E18 AppleM2CLCD c05e3640..c05e3efc: RGB enables 4[4:5],
 * video enable 4[3], limited range 4[8], plane geometry and Q10 matrix.
 * ponytail: progressive NV12 and BGRA only; other formats/transforms still
 * require register traces. Filter rounding/edge extension need hardware QA. */
static bool lcd_plane_range(uint32_t base, unsigned stride, unsigned rows,
                            unsigned bytes)
{
    return base >= 0x08000000 && rows && stride >= bytes &&
        (uint64_t)base + (uint64_t)(rows - 1) * stride + bytes <= 0x10000000;
}

/* The driver uploads nine half-phases, high halfword first, signed Q7.
 * Phases 9..15 are the reversed taps of phases 7..1. */
static int lcd_filter_tap(const uint32_t *bank, unsigned taps,
                          unsigned phase, unsigned tap)
{
    if (phase > 8) {
        phase = 16 - phase;
        tap = taps - 1 - tap;
    }
    unsigned v = bank[phase * taps / 2 + tap / 2];
    v = (v >> ((tap & 1) ? 0 : 16)) & 0xfff;
    return (v & 0x800) ? (int)v - 4096 : v;
}

static uint8_t lcd_filter_sample(const uint8_t *src, unsigned count,
                                 unsigned step, uint32_t position,
                                 const uint32_t *bank, unsigned taps)
{
    int sum = 0, first = (int)(position >> 16) - (int)(taps / 2 - 1);
    unsigned phase = (position >> 12) & 15;
    for (unsigned t = 0; t < taps; t++) {
        int x = CLAMP(first + (int)t, 0, (int)count - 1);
        sum += src[x * step] * lcd_filter_tap(bank, taps, phase, t);
    }
    return CLAMP((sum + 64) >> 7, 0, 255);
}

static void lcd_filter_plane(const uint8_t *src, unsigned sw, unsigned sh,
                              unsigned stride, unsigned step, uint8_t *dst,
                              unsigned dw, unsigned dh, unsigned xr, unsigned yr,
                              unsigned xp, unsigned yp,
                              const uint32_t *hbank, unsigned htaps,
                              const uint32_t *vbank)
{
    g_autofree uint8_t *rows = g_malloc((size_t)dw * sh);
    for (unsigned y = 0; y < sh; y++)
        for (unsigned x = 0; x < dw; x++)
            rows[y * dw + x] = lcd_filter_sample(src + y * stride, sw, step,
                                                xp + x * xr, hbank, htaps);
    for (unsigned y = 0; y < dh; y++)
        for (unsigned x = 0; x < dw; x++)
            dst[y * dw + x] = lcd_filter_sample(rows + x, sh, dw,
                                                yp + y * yr, vbank, 4);
}

static bool lcd_compose_planes(const uint32_t *r, uint8_t *out)
{
    const unsigned pw = LCD_FB_WIDTH, ph = LCD_FB_HEIGHT;
    memset(out, 0, pw * ph * 4);
    if (r[4/4] & 8) {
        unsigned sw = r[0x134/4] >> 16, sh = r[0x134/4] & 0xffff;
        unsigned cx = r[0x130/4] >> 16, cy = r[0x130/4] & 0xffff;
        unsigned dw = r[0x13c/4] >> 16, dh = r[0x13c/4] & 0xffff;
        unsigned x0 = r[0x138/4] >> 16, y0 = r[0x138/4] & 0xffff;
        unsigned ys = r[0x2e0/4] >> 17, uvs = r[0x2e4/4] >> 17;
        /* Mode 3 matches CA's transition quad: (s,t) -> (height-1-t,s). */
        unsigned rotation = r[0x118/4] & 7;
        unsigned ow = rotation ? dh : dw, oh = rotation ? dw : dh;
        unsigned xr = r[(rotation ? 0x2c4 : 0x2c0)/4];
        unsigned yr = r[(rotation ? 0x2c0 : 0x2c4)/4];
        bool scaled = xr != 0x10000 || yr != 0x10000;
        unsigned cw = (sw + (cx & 1) + 1) & ~1u;
        unsigned ch = (sh + (cy & 1) + 1) / 2;
        uint32_t ybase = r[0x11c/4], uvbase = r[0x120/4];
        if (!sw || !sh || sw > 2048 || sh > 2048 ||
            r[0x118/4] != rotation ||
            (rotation != 0 && rotation != 3) ||
            !ow || !oh || ow > 2048 || oh > 2048 ||
            xr != ((uint64_t)sw << 16) / ow ||
            yr != ((uint64_t)sh << 16) / oh ||
            !lcd_plane_range(ybase, ys, cy + sh, cx + sw) ||
            !lcd_plane_range(uvbase, uvs, (cy + sh + 1) / 2,
                              (cx + sw + 1) & ~1u)) return false;
        ybase += cy * ys + cx;
        uvbase += (cy / 2) * uvs + (cx & ~1u);
        g_autofree uint8_t *y = g_malloc((size_t)sw * sh);
        g_autofree uint8_t *uv = g_malloc((size_t)cw * ch);
        for (unsigned row = 0; row < sh; row++)
            cpu_physical_memory_read(ybase + row * ys, y + row * sw, sw);
        for (unsigned row = 0; row < ch; row++)
            cpu_physical_memory_read(uvbase + row * uvs, uv + row * cw, cw);
        g_autofree uint8_t *filtered = scaled ? g_malloc((size_t)ow * oh * 3) : NULL;
        if (scaled) {
            lcd_filter_plane(y, sw, sh, sw, 1, filtered, ow, oh, xr, yr, 0, 0,
                             r + 0x140/4, 8, r + 0x1d0/4);
            for (unsigned c = 0; c < 2; c++)
                lcd_filter_plane(uv + c, cw / 2, ch, cw, 2,
                                 filtered + (c + 1) * ow * oh, ow, oh,
                                 xr / 2, yr / 2, (cx & 1) * 0x8000,
                                 (cy & 1) * 0x8000, r + 0x220/4, 4, r + 0x270/4);
        }
        int m[9];
        for (unsigned i = 0; i < 9; i++) {
            unsigned v = r[0x70/4 + i];
            m[i] = (v & 0x1000) ? -(int)(v & 0xfff) : v & 0xfff;
        }
        for (unsigned dy = 0; dy < dh && dy + y0 < ph; dy++) {
            for (unsigned dx = 0; dx < dw && dx + x0 < pw; dx++) {
                unsigned sx = rotation ? dy : dx;
                unsigned sy = rotation ? oh - 1 - dx : dy;
                int input[] = {
                    (scaled ? filtered[sy * ow + sx] : y[sy * sw + sx]) -
                        ((r[1] & 0x100) ? 16 : 0),
                    (scaled ? filtered[ow * oh + sy * ow + sx] :
                        uv[((sy + (cy & 1)) / 2) * cw + ((sx + (cx & 1)) & ~1u)]) - 128,
                    (scaled ? filtered[2 * ow * oh + sy * ow + sx] :
                        uv[((sy + (cy & 1)) / 2) * cw + ((sx + (cx & 1)) & ~1u) + 1]) - 128 };
                uint8_t *p = out + ((dy + y0) * pw + dx + x0) * 4;
                for (unsigned c = 0; c < 3; c++) {
                    int value = (m[c*3] * input[0] + m[c*3+1] * input[1] +
                                 m[c*3+2] * input[2] + 512) >> 10;
                    p[2-c] = CLAMP(value, 0, 255);
                }
                p[3] = 255;
            }
        }
    }
    for (unsigned plane = 0; plane < 2; plane++) {
        if (!(r[1] & (0x10u << plane))) continue;
        const uint32_t *p = r + (0x20 + plane * 0x20) / 4;
        unsigned w = p[4] >> 16, h = p[4] & 0xffff, stride = p[2] * 4;
        unsigned x0 = p[5] >> 16, y0 = p[5] & 0xffff;
        unsigned rotation = p[0] >> 22;
        if (!w || w > 2048 || !h || h > 2048 || p[2] > 8192 || p[3] ||
            (p[0] & 0xf00) != 0x700 || (rotation != 0 && rotation != 3) ||
            !lcd_plane_range(p[1], stride, h, w * 4)) return false;
        /* RGB scanout can bypass CA composition too. Mode 3 rotates its
         * padded landscape surface into the physical portrait panel, just
         * like the video plane. Geometry and origin are guest registers. */
        unsigned dw = rotation ? h : w, dh = rotation ? w : h;
        g_autofree uint8_t *pixels = g_malloc((size_t)w * h * 4);
        for (unsigned sy = 0; sy < h; sy++)
            cpu_physical_memory_read(p[1] + sy * stride,
                                     pixels + (size_t)sy * w * 4, w * 4);
        for (unsigned dy = 0; dy < dh && dy + y0 < ph; dy++) {
            for (unsigned dx = 0; dx < dw && dx + x0 < pw; dx++) {
                unsigned sx = rotation ? dy : dx;
                unsigned sy = rotation ? h - 1 - dx : dy;
                const uint8_t *src = pixels + ((size_t)sy * w + sx) * 4;
                uint8_t *dst = out + ((dy + y0) * pw + dx + x0) * 4;
                unsigned alpha = plane ? src[3] : 255;
                for (unsigned c = 0; c < 3; c++)
                    dst[c] = MIN(255, src[c] + (dst[c] * (255 - alpha) + 127) / 255);
                dst[3] = 255;
            }
        }
    }
    return true;
}

/* The linear full-panel RGB0 case keeps framebuffer dirty tracking. Every
 * other enabled plane configuration must honor its programmed stride,
 * dimensions, destination origin, and transform before host-window rotation. */
static bool lcd_needs_plane_composition(const uint32_t *r)
{
    if (r[1] & 0x28) return true;
    if (!(r[1] & 0x10)) return false;
    const uint32_t *p = r + 0x20 / 4;
    return (p[0] >> 22) || (p[0] & 0xf00) != 0x700 || p[3] ||
        p[2] != LCD_FB_WIDTH || p[4] != ((LCD_FB_WIDTH << 16) | LCD_FB_HEIGHT) || p[5];
}

/*
 * Blit the guest's portrait framebuffer into a rotated host surface.
 *
 * framebuffer_update_display() can only walk source and destination with fixed
 * strides, which cannot express a transpose, so the rotated case gets its own
 * blit -- and with it, no dirty tracking: the whole frame is redrawn every
 * time. Rotation is a rare, deliberate state, so that is left alone.
 */
static void lcd_refresh_rotated(IPodTouchLCDState *lcd, DisplaySurface *surface,
                                int rot, bool composed)
{
    const int sw = LCD_FB_WIDTH, sh = LCD_FB_HEIGHT;
    int dw = (rot == 90 || rot == 270) ? sh : sw;
    int dh = (rot == 90 || rot == 270) ? sw : sh;
    uint8_t *dst = surface_data(surface);
    int dstride = surface_stride(surface);
    int bri = lcd_bright_effective();

    lcd_bright_lut_sync(bri);
    lcd->last_bright = bri;   /* keeps the duplicate-frame test in lcd_refresh honest */
    int sx, sy;

    /*
     * Same identity-conversion shortcut the unrotated path uses (see
     * draw_line32_32 / lcd_line_is_copy): at full backlight into an x8r8g8b8
     * surface the guest's BGRX pixel is already the destination pixel, so the
     * per-pixel copy skips three LUT loads and the rgb_to_pixel32 call. This is
     * the steady-state path for landscape games, which stay rotated the whole
     * session, so it is not the "rare state" the comment above once assumed.
     */
    bool fast_copy = (bri == 255 &&
                      surface_format(surface) == PIXMAN_x8r8g8b8);

    if (surface_width(surface) != dw || surface_height(surface) != dh) {
        return; /* resize has not landed yet; skip this frame */
    }
    if (!lcd->rotbuf) {
        lcd->rotbuf = g_malloc(sw * sh * 4);
    }
    if (!composed) cpu_physical_memory_read(lcd->scanout_base, lcd->rotbuf, sw * sh * 4);

    /*
     * A source row maps to a straight line in the destination for every one of
     * the three rotations, so the transpose is a start address and a signed
     * step per row -- the per-pixel switch it replaces was re-deciding the same
     * thing 153,600 times a frame.
     */
    for (sy = 0; sy < sh; sy++) {
        const uint8_t *s = lcd->rotbuf + (size_t)sy * sw * 4;
        uint8_t *d;
        int step;

        switch (rot) {
        case 0:
            d = dst + (size_t)sy * dstride;
            step = 4;
            break;
        case 90:
            d = dst + (size_t)(sh - 1 - sy) * 4;
            step = dstride;
            break;
        case 270:
            d = dst + (size_t)(sw - 1) * dstride + (size_t)sy * 4;
            step = -dstride;
            break;
        default:
            d = dst + (size_t)(sh - 1 - sy) * dstride + (size_t)(sw - 1) * 4;
            step = -4;
            break;
        }
        if (fast_copy) {
            for (sx = 0; sx < sw; sx++, s += 4, d += step) {
                *(uint32_t *)d = *(const uint32_t *)s;
            }
        } else {
            for (sx = 0; sx < sw; sx++, s += 4, d += step) {
                *(uint32_t *)d = rgb_to_pixel32(lcd_bright_lut[s[2]],
                                                lcd_bright_lut[s[1]],
                                                lcd_bright_lut[s[0]]);
            }
        }
    }
    dpy_gfx_update(lcd->con, 0, 0, dw, dh);
}

static void lcd_refresh(void *opaque)
{
    // printf("%s: refreshing LCD screen\n", __func__);

    IPodTouchLCDState *lcd = (IPodTouchLCDState *) opaque;
    DisplaySurface *surface = qemu_console_surface(lcd->con);
    drawfn draw_line;
    int src_width, dest_width;
    int height, first, last;
    int width, linesize;

    if (!lcd || !lcd->con || !surface_bits_per_pixel(surface))
        return;

    /* "vscan" = pushed by the panel's frame interrupt, "scan" = QEMU's own
     * free-running 30 ms display poll. The distinction is the whole point of
     * the change; a trace that cannot tell them apart cannot show it worked. */
    lcd_ft(lcd_in_vsync_present ? "vscan" : "scan", lcd->scanout_base);

    /*
     * The frame interrupt already presents every frame at the panel's own
     * 60 Hz. QEMU's display poll then asks for a second conversion of a frame
     * the window has already been shown -- about 33 of them a second, none of
     * which can contain anything the vsync push did not already draw.
     *
     * Gated on the push having actually run recently rather than on "a UI is
     * attached": with -display none nothing else ever draws, and a screendump
     * arriving through this same path must still be able to force a frame.
     * The frame is only a duplicate if nothing outside guest memory has changed
     * either -- Cmd+C re-renders at full exposure through exactly this call and
     * would otherwise copy the dimmed frame the panel last presented.
     */
    if (!lcd_in_vsync_present && !lcd->invalidate &&
        lcd->rotation == it_display_rotation_req &&
        lcd_bright_effective() == lcd->last_bright &&
        lcd->last_present_ns &&
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - lcd->last_present_ns <
            2 * LCD_VSYNC_PERIOD_NS) {
        lcd_ft("scanskip", lcd->scanout_base);
        return;
    }

    /*
     * Pick up a pending orientation change. The console resize has to happen
     * from the graphics update, not from the I2C/QMP thread that moved the
     * accelerometer.
     */
    if (lcd->rotation != it_display_rotation_req) {
        int rot = it_display_rotation_req;
        bool land = (rot == 90 || rot == 270);

        lcd->rotation = rot;
        qemu_console_resize(lcd->con,
                            land ? LCD_FB_HEIGHT : LCD_FB_WIDTH,
                            land ? LCD_FB_WIDTH  : LCD_FB_HEIGHT);
        surface = qemu_console_surface(lcd->con);
        lcd->invalidate = 1;
        if (!surface_bits_per_pixel(surface)) {
            return;
        }
    }

    bool composed = false;
    if (lcd->planes_enabled && lcd_needs_plane_composition(lcd->plane_scanout)) {
        if (!lcd->rotbuf) lcd->rotbuf = g_malloc(LCD_FB_WIDTH * LCD_FB_HEIGHT * 4);
        composed = lcd_compose_planes(lcd->plane_scanout, lcd->rotbuf);
        if (!composed) {
            static bool warned;
            if (!warned) { warned = true; fprintf(stderr, "[LCD] unsupported plane configuration\n"); }
        }
    }
    if (lcd->rotation != 0 || composed) {
        int64_t t = lcd_frametrace() ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : 0;
        lcd_refresh_rotated(lcd, surface, lcd->rotation, composed);
        if (t) {
            lcd_ft("blitrot",
                   (uint32_t)((qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t)/1000));
        }
        lcd->invalidate = 0;
        if (lcd_in_vsync_present) {
            lcd->last_present_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        }
        return;
    }

    int64_t t_blit = lcd_frametrace() ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : 0;
    int bri = lcd_bright_effective();

    dest_width = 4;
    draw_line = draw_line32_32;

    /* Resolution */
    first = last = 0;
    width = LCD_FB_WIDTH;
    height = LCD_FB_HEIGHT;

    src_width =  4 * width;
    linesize = surface_stride(surface);

    /*
     * Only the lines the guest wrote since the last frame are converted, so
     * every way the frame can change without a guest write has to force a full
     * one instead. A flip installs a buffer whose contents this surface has
     * never been diffed against; a new surface starts blank; and the backlight
     * rescales every pixel from the same memory. fbsection also caches a host
     * pointer, so it has to be rebuilt on a flip either way.
     */
    if (lcd->scanout_base != lcd->fbsection_base || !lcd->fbsection.mr) {
        framebuffer_update_memory_section(&lcd->fbsection, lcd->sysmem,
                                          lcd->scanout_base, height, src_width);
        lcd->fbsection_base = lcd->scanout_base;
        lcd->invalidate = 1;
    }
    if (lcd->last_surface != surface) {
        lcd->last_surface = surface;
        lcd->invalidate = 1;
    }
    if (lcd->last_bright != bri) {
        lcd->last_bright = bri;
        lcd->invalidate = 1;
    }
    lcd_bright_lut_sync(bri);

    /* See draw_line32_32(). x8r8g8b8 is what qemu_default_pixman_format() hands
     * out here, but a frontend is free to ask for something else. */
    lcd_line_is_copy = (bri == 255 &&
                        surface_format(surface) == PIXMAN_x8r8g8b8);

    framebuffer_update_display(surface, &lcd->fbsection,
                               width, height,
                               src_width,       /* Length of source line, in bytes.  */
                               linesize,        /* Bytes between adjacent horizontal output pixels.  */
                               dest_width,      /* Bytes between adjacent vertical output pixels.  */
                               lcd->invalidate,
                               draw_line, NULL,
                               &first, &last);
    if (first >= 0) {
        dpy_gfx_update(lcd->con, 0, first, width, last - first + 1);
    }
    if (lcd_frametrace()) {
        /* "blit" converted lines, "still" found the scanout buffer untouched:
         * the two have to be distinguishable to count conversions per second. */
        lcd_ft(first >= 0 ? "blit" : "still",
               (uint32_t)((qemu_clock_get_ns(QEMU_CLOCK_REALTIME)
                           - t_blit)/1000));
    }
    lcd->invalidate = 0;
    if (lcd_in_vsync_present) {
        lcd->last_present_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    }
}

static const MemoryRegionOps lcd_ops = {
    .read = ipod_touch_lcd_read,
    .write = ipod_touch_lcd_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const GraphicHwOps gfx_ops = {
    .invalidate  = lcd_invalidate,
    .gfx_update  = lcd_refresh,
};

/*
 * Undo the rotation lcd_refresh_rotated() applied, so the multitouch model
 * always sees coordinates in the guest's portrait framebuffer space (x left to
 * right, y measured from the bottom). Without this, touch lands in the wrong
 * place -- or a transposed place -- as soon as the window is turned.
 *
 * x/y come in as 0..2^15 fractions of the *window*, y measured downwards, which
 * is what both the legacy mouse handler and the multi-touch handler below get.
 */
static void ipod_touch_lcd_map_touch(IPodTouchLCDState *lcd, int x, int y,
                                     float *fx, float *fy)
{
    float cx = x / (float)pow(2, 15);
    float cy = y / (float)pow(2, 15);

    switch (lcd->rotation) {
    case 90:  *fx = cy;        *fy = cx;        break;
    case 270: *fx = 1.0f - cy; *fy = 1.0f - cx; break;
    case 180: *fx = 1.0f - cx; *fy = cy;        break;
    default:  *fx = cx;        *fy = 1.0f - cy; break;
    }
}

static void ipod_touch_lcd_mouse_event(void *opaque, int x, int y, int z, int buttons_state)
{
    // printf("x %d y %d z %d state %d\n", x, y, z, buttons_state);

    IPodTouchLCDState *lcd = (IPodTouchLCDState *) opaque;
    float fx, fy;

    ipod_touch_lcd_map_touch(lcd, x, y, &fx, &fy);

    /*
     * Only the position moves here. prev_touch_* is the reference the reported
     * velocity is measured from, and get_frame() advances it when a frame is
     * actually emitted -- moving it on every host event instead silently
     * dropped the distance covered by any motion the report-rate coalescer
     * skipped, and left a finger held still reporting the speed of its last
     * step forever.
     */
    lcd->mt->touch_x = fx;
    lcd->mt->touch_y = fy;

    if(buttons_state && !lcd->mt->touch_down) {
        ipod_touch_multitouch_on_touch(lcd->mt);
    }
    else if(!buttons_state && lcd->mt->touch_down) {
        ipod_touch_multitouch_on_release(lcd->mt);
    }
    else if(buttons_state) {
        /* Drag: report the motion now rather than waiting for the next tick. */
        ipod_touch_multitouch_on_motion(lcd->mt);
    }
}

/*
 * Multi-touch input.
 *
 * The legacy mouse handler above can only ever describe one contact, so pinch
 * and rotate were simply unreachable however good the digitizer model got.
 * QEMU's own multi-touch event kind (INPUT_EVENT_KIND_MTT, "mtt") carries a
 * slot and a tracking id per contact and is what a touchscreen frontend or QMP
 * `input-send-event` sends, so the panel accepts that directly and hands each
 * slot to the digitizer.
 *
 * The wire protocol is the usual two-phase one: DATA events carry the X and Y
 * for a slot, then BEGIN/UPDATE/END commits them. Positions are therefore
 * latched per slot until a commit arrives -- an END for a slot we never saw is
 * ignored rather than reported at (0,0), which would be a phantom tap in the
 * corner.
 *
 * Slots beyond MT_MAX_FINGERS are dropped by the digitizer, not here, so the
 * limit lives in one place.
 */
static void ipod_touch_lcd_mtt_event(DeviceState *dev, QemuConsole *src,
                                     InputEvent *evt)
{
    IPodTouchLCDState *lcd = IPOD_TOUCH_LCD(dev);
    InputMultiTouchEvent *mtt = evt->u.mtt.data;
    int slot = mtt->slot;
    float fx, fy;

    if (!lcd->mt || slot < 0 || slot >= MT_MAX_FINGERS) {
        return;
    }

    switch (mtt->type) {
    case INPUT_MULTI_TOUCH_TYPE_DATA:
        if (mtt->axis == INPUT_AXIS_X) {
            lcd->mtt_x[slot] = mtt->value;
        } else {
            lcd->mtt_y[slot] = mtt->value;
        }
        lcd->mtt_seen[slot] = true;
        return;

    case INPUT_MULTI_TOUCH_TYPE_BEGIN:
    case INPUT_MULTI_TOUCH_TYPE_UPDATE:
        if (!lcd->mtt_seen[slot]) {
            return;
        }
        ipod_touch_lcd_map_touch(lcd, lcd->mtt_x[slot], lcd->mtt_y[slot],
                                 &fx, &fy);
        ipod_touch_multitouch_set_finger(lcd->mt, slot, fx, fy, true);
        return;

    case INPUT_MULTI_TOUCH_TYPE_END:
    case INPUT_MULTI_TOUCH_TYPE_CANCEL:
        if (!lcd->mtt_seen[slot]) {
            return;
        }
        ipod_touch_lcd_map_touch(lcd, lcd->mtt_x[slot], lcd->mtt_y[slot],
                                 &fx, &fy);
        ipod_touch_multitouch_set_finger(lcd->mt, slot, fx, fy, false);
        lcd->mtt_seen[slot] = false;
        return;

    default:
        return;
    }
}

static const QemuInputHandler ipod_touch_lcd_mtt_handler = {
    .name  = "iPod Touch Multitouch",
    .mask  = INPUT_EVENT_MASK_MTT,
    .event = ipod_touch_lcd_mtt_event,
};

/*
 * The panel's frame interrupt, and with it the whole visible frame pipeline.
 *
 * Two things happen here that used not to.
 *
 * 1. The next tick is scheduled from the PREVIOUS DEADLINE, not from "now".
 *    Re-arming at `now + period` folds the callback's own dispatch latency
 *    into the period, permanently: every tick starts its interval from
 *    wherever it happened to run. Measured on an idle 3.1.3 lock screen that
 *    latency is ~1.0-1.4 ms, so a nominally 60 Hz frame interrupt was
 *    delivered at 55.5 Hz -- the guest's animation clock ran 7.5% slow and
 *    random-walked in phase, because the error never got corrected. With an
 *    absolute deadline a tick that runs late is followed by one that runs on
 *    its own schedule, so lateness stays bounded instead of accumulating.
 *
 * 2. When something is actually looking at the console, the frame is pushed to
 *    the host here rather than waiting to be sampled. QEMU's own display
 *    refresh is a free-running 30 ms timer (GUI_REFRESH_INTERVAL_DEFAULT), so
 *    a guest presenting at ~57 fps was being resampled at 33.3 Hz: the host
 *    window advanced by one guest frame on some updates and two on others,
 *    which is judder of exactly the kind the device is reported to show, and
 *    it is entirely an artefact of the sampling rate. Driving the update from
 *    the frame interrupt puts the host window on the guest's own cadence and
 *    phase. Skipped when nothing is listening (-display none), where the blit
 *    would be pure cost.
 */
static void refresh_timer_tick(void *opaque)
{
    IPodTouchLCDState *s = (IPodTouchLCDState *)opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (lcd_frametrace()) {
        lcd_ft("vsync", (uint32_t)(now - s->next_vsync));
    }

    if (lcd_vsync_trace()) {
        static uint64_t enabled, masked;
        if (s->irq_enable & 1) enabled++; else masked++;
        if ((enabled + masked) % 300 == 0) {
            fprintf(stderr, "[LCDV] vsync: %" PRIu64 " enabled, %" PRIu64
                    " masked (enable=%08x pending=%08x)\n",
                    enabled, masked, s->irq_enable, s->irq_status);
        }
    }
    lcd_vblank_irq(s);

    /*
     * Latch the scanout base, as the panel does at vblank. Without this the
     * blit reads whatever base the guest has installed at the instant the blit
     * happens, so a caller that is not on the frame cadence -- QEMU's own 30 ms
     * display poll, or a screendump -- can show a frame the panel would not
     * have reached yet. The driver already assumes the register takes effect at
     * the next vblank; that is what its triple buffering is for.
     */
    s->scanout_base = s->w1_framebuffer_base;
    memcpy(s->plane_scanout, s->plane_regs, sizeof(s->plane_regs));

    if (s->con && qemu_console_is_visible(s->con) && !lcd_vsync_legacy()) {
        lcd_in_vsync_present = true;
        graphic_hw_update(s->con);
        lcd_in_vsync_present = false;
    }

    /* IT_LCD_VSYNC_LEGACY restores the old re-arm-from-now behaviour, so the
     * two can be A/B'd from one binary. Bisecting only. */
    if (lcd_vsync_legacy()) {
        s->next_vsync = now + LCD_VSYNC_PERIOD_NS * lcd_vsync_divisor();
        timer_mod(s->refresh_timer, s->next_vsync);
        return;
    }

    s->next_vsync += LCD_VSYNC_PERIOD_NS * lcd_vsync_divisor();
    if (s->next_vsync <= now) {
        /*
         * More than a whole frame behind -- the host was descheduled, or the
         * machine was stopped. Catching up by firing back-to-back ticks would
         * hand the guest a burst of frame interrupts it cannot use, so
         * resynchronise to the current time and carry on.
         */
        s->next_vsync = now + LCD_VSYNC_PERIOD_NS * lcd_vsync_divisor();
    }
    timer_mod(s->refresh_timer, s->next_vsync);
}

/*
 * Clear the display controller's registers on a warm reset.
 *
 * These are all programmed fresh by iBoot and the kernel on every boot, but
 * until they are the model would otherwise answer reads with the previous
 * boot's values and keep scanning out of the old framebuffer base. invalidate
 * is set so the first frame after the reset is drawn in full rather than
 * diffed against whatever was on screen before.
 */
static void ipod_touch_lcd_reset(DeviceState *dev)
{
    IPodTouchLCDState *s = IPOD_TOUCH_LCD(dev);

    lcd_changebrightness(255);

    memset(s->plane_regs, 0, sizeof(s->plane_regs));
    memset(s->plane_scanout, 0, sizeof(s->plane_scanout));
    s->lcd_con = 0;
    s->render = 0;
    s->irq_enable = s->irq_status = 0;
    lcd_update_irq(s);
    s->w1_display_resolution_info = 0;
    s->w1_framebuffer_base = 0;
    s->scanout_base = 0;
    s->w1_hspan = 0;
    s->w1_display_depth_info = 0;
    s->invalidate = 1;
    /* Release through the helper that took the reference: it also turns
     * DIRTY_MEMORY_VGA logging back off. Zeroing the struct by hand dropped the
     * only pointer that could do either, so every warm reset and every restore
     * leaked a MemoryRegion reference and left dirty tracking on for all of
     * guest RAM. */
    if (s->fbsection.mr) {
        /* Only when it was actually populated: the helper dereferences its root
         * MemoryRegion, and at the first reset this section is still zeroed. */
        framebuffer_update_memory_section(&s->fbsection, s->sysmem, 0, 0, 0);
    }
    memset(&s->fbsection, 0, sizeof(s->fbsection));
    s->fbsection_base = 0;
    s->last_surface = NULL;
    s->last_bright = -1;
    s->last_present_ns = 0;
    /*
     * The device comes up portrait. it_display_rotation_req is file-scope and
     * survived reset, so a reboot taken in landscape inherited the previous
     * boot's rotation. s->rotation is deliberately NOT cleared here: the
     * refresh path resizes the console only when rotation != request, so
     * leaving the applied value is what makes it rotate back. rotbuf is a
     * fixed-size scratch buffer refilled from guest memory every frame, not
     * state.
     */
    it_display_rotation_req = 0;
    s->next_vsync = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + LCD_VSYNC_PERIOD_NS;
    if (lcd_trace()) {
        fprintf(stderr, "[LCD] ==== reset ====\n");
    }
}

static void ipod_touch_lcd_realize(DeviceState *dev, Error **errp)
{
    IPodTouchLCDState *s = IPOD_TOUCH_LCD(dev);
    s->con = graphic_console_init(dev, 0, &gfx_ops, s);
    qemu_console_resize(s->con, 320, 480);

    // add mouse handler
    qemu_add_mouse_event_handler(ipod_touch_lcd_mouse_event, s, 1, "iPod Touch Touchscreen");

    /* ... and the multi-touch one, which routes by event mask and so coexists
     * with the mouse rather than replacing it. */
    qemu_input_handler_register(dev, &ipod_touch_lcd_mtt_handler);

    /* Let a frontend copying the screen ask for an undimmed render. */
    qemu_display_register_capture_exposure(lcd_set_capture_exposure);

    // initialize the refresh timer
    s->refresh_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, refresh_timer_tick, s);
    s->next_vsync = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + LCD_VSYNC_PERIOD_NS;
    timer_mod(s->refresh_timer, s->next_vsync);
}

static void ipod_touch_lcd_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(sbd);
    IPodTouchLCDState *s = IPOD_TOUCH_LCD(dev);

    memory_region_init_io(&s->iomem, obj, &lcd_ops, s, "lcd", 0x10000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

/*
 * con, sysmem, mt, irq and refresh_timer are all machine wiring, rebuilt by
 * realize on the destination before the snapshot is read. rotbuf is a scratch
 * buffer refilled from guest memory every frame. fbsection is a cached
 * MemoryRegionSection - a host pointer - and must never be migrated; it is
 * rebuilt from w1_framebuffer_base. post_load forces a full repaint, otherwise
 * the destination's blank surface is diffed against a framebuffer it never
 * drew and most of the screen stays black.
 */
static int ipod_touch_lcd_pre_save(void *opaque)
{
    IPodTouchLCDState *s = opaque;
    s->saved_planes_enabled = s->planes_enabled;
    return 0;
}

static int ipod_touch_lcd_post_load(void *opaque, int version_id)
{
    IPodTouchLCDState *s = opaque;
    if (version_id >= 4 && s->saved_planes_enabled != s->planes_enabled) return -EINVAL;
    lcd_restore_irq(s, version_id);
    if (version_id < 2) {
        memset(s->plane_regs, 0, sizeof(s->plane_regs));
        memset(s->plane_scanout, 0, sizeof(s->plane_scanout));
    }


    /* Release through the helper that took the reference: it also turns
     * DIRTY_MEMORY_VGA logging back off. Zeroing the struct by hand dropped the
     * only pointer that could do either, so every warm reset and every restore
     * leaked a MemoryRegion reference and left dirty tracking on for all of
     * guest RAM. */
    if (s->fbsection.mr) {
        /* Only when it was actually populated: the helper dereferences its root
         * MemoryRegion, and at the first reset this section is still zeroed. */
        framebuffer_update_memory_section(&s->fbsection, s->sysmem, 0, 0, 0);
    }
    memset(&s->fbsection, 0, sizeof(s->fbsection));
    s->invalidate = 1;
    s->fbsection_base = 0;
    s->last_surface = NULL;
    s->last_bright = -1;
    /* ...and let the first refresh after the restore through, rather than
     * having it dropped as a duplicate of a frame from before the snapshot. */
    s->last_present_ns = 0;
    /* scanout_base is re-latched by the next frame interrupt anyway, but a
     * repaint can be asked for before that and would otherwise draw black. */
    s->scanout_base = s->w1_framebuffer_base;
    return 0;
}

static const VMStateDescription vmstate_ipod_touch_lcd = {
    .name = "ipod_touch_lcd",
    .version_id = 4,
    .minimum_version_id = 1,
    .pre_save = ipod_touch_lcd_pre_save,
    .post_load = ipod_touch_lcd_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY_V(plane_regs, IPodTouchLCDState, 0x300 / 4, 2),
        VMSTATE_UINT32_ARRAY_V(plane_scanout, IPodTouchLCDState, 0x300 / 4, 2),
        VMSTATE_UINT8(brightness, IPodTouchLCDState),
        VMSTATE_UINT32(lcd_con, IPodTouchLCDState),
        VMSTATE_UINT32(w1_display_resolution_info, IPodTouchLCDState),
        VMSTATE_UINT32(w1_framebuffer_base, IPodTouchLCDState),
        VMSTATE_UINT32(w1_hspan, IPodTouchLCDState),
        VMSTATE_UINT32(w1_display_depth_info, IPodTouchLCDState),
        VMSTATE_UINT32(render, IPodTouchLCDState),
        VMSTATE_INT32(rotation, IPodTouchLCDState),
        VMSTATE_UINT32_V(irq_enable, IPodTouchLCDState, 3),
        VMSTATE_UINT32_V(irq_status, IPodTouchLCDState, 3),
        VMSTATE_BOOL_V(saved_planes_enabled, IPodTouchLCDState, 4),
        VMSTATE_END_OF_LIST()
    }
};

static const Property lcd_properties[] = {
    DEFINE_PROP_BOOL("planes", IPodTouchLCDState, planes_enabled, false),
};

static void ipod_touch_lcd_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ipod_touch_lcd_realize;
    device_class_set_legacy_reset(dc, ipod_touch_lcd_reset);
    dc->vmsd = &vmstate_ipod_touch_lcd;
    device_class_set_props(dc, lcd_properties);
}

static const TypeInfo ipod_touch_lcd_info = {
    .name          = TYPE_IPOD_TOUCH_LCD,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchLCDState),
    .instance_init = ipod_touch_lcd_init,
    .class_init    = ipod_touch_lcd_class_init,
};

static void ipod_touch_machine_types(void)
{
    type_register_static(&ipod_touch_lcd_info);
}

type_init(ipod_touch_machine_types)
