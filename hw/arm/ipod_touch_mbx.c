/*
 * S5L8720 PowerVR MBX Lite graphics core, as used by AppleMBX (com.apple.driver.AppleMBX) and MBX2D.
 *
 * The model emulates what the driver submits rather than the GPU's microarchitecture:
 *  - Interrupts. The driver (AppleMBXDevice::handleInterrupt, 0xc04ea478) handles STATUS & MASK, clears those bits
 *    through CLEAR and tracks "2DIdle" and "3DIdle" from them. A 2D batch it submits without seeing MBX_INT_2D_DONE
 *    within 500 ms fires a Graphics Recovery Event (0xc04e8648), which resets the core and fakes the missing
 *    completions by writing them to STATUS.
 *  - The MMU (AppleMBXMMU). Surfaces are mapped into a 32 MB GPU address space through a two-level page table whose
 *    eight directory entries live in registers; the model walks it in guest memory for every access.
 *  - The 2D core. MBX2D builds blocks of 2D commands in a command buffer, the kernel adds the surfaces' GPU addresses
 *    and copies them to the 2D slave port (writeTo2DSlavePort, 0xc04e5ac8), followed by a FLUSH word. The model
 *    parses that stream and carries out the fills and copies straight in guest memory.
 *  - 3D blits. For compositing that the 2D core cannot do (blending layers with their own alpha, which is most of
 *    SpringBoard's), MBX2D asks the kernel for a 3D blit (AppleMBXDevice::setup3DBlit, 0xc04e67ec). The kernel
 *    fills in object templates in a parameter buffer it built at start-up, points every region of the render
 *    rectangle at one object list and starts a render. The model draws that object list: textured quads, with the
 *    destination read back as a second texture for blending, clipped by depth-only quads.
 *  - The tile accelerator, which builds parameter buffers for OpenGL ES scenes, is not emulated yet.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/log.h"
#include "exec/address-spaces.h"
#include "migration/vmstate.h"
#include "hw/arm/ipod_touch_mbx.h"
#include "trace.h"

static void ipod_touch_mbx_update_irq(IPodTouchMBXState *s)
{
    uint32_t pending = s->int_status & s->regs[MBX_INT_MASK / 4] & 0xffff;
    trace_ipod_touch_mbx_irq(s->int_status, s->regs[MBX_INT_MASK / 4], pending != 0);
    qemu_set_irq(s->irq, pending != 0);
}

static void ipod_touch_mbx_raise(IPodTouchMBXState *s, uint32_t events)
{
    s->int_status |= events;
    ipod_touch_mbx_update_irq(s);
}

static void ipod_touch_mbx_2d_done(void *opaque)
{
    ipod_touch_mbx_raise(opaque, MBX_INT_2D_DONE);
}

static void ipod_touch_mbx_3d_done(void *opaque)
{
    ipod_touch_mbx_raise(opaque, MBX_INT_3D_DONE);
}

/*
 * GPU memory access
 */

static bool ipod_touch_mbx_translate(IPodTouchMBXState *s, uint32_t va, hwaddr *pa)
{
    uint32_t dir, pte;

    if (!(s->mmu_ctrl & MBX_MMU_CTRL_ENABLE)) {
        *pa = va;
        return true;
    }
    if (va >= MBX_GPU_VA_SIZE) {
        return false;
    }
    dir = s->regs[MBX_MMU_DIR / 4 + (va >> 22)];
    if (!dir) {
        return false;
    }
    pte = address_space_ldl_le(s->dma_as, (dir & ~0xfff) + ((va >> 12) & 0x3ff) * 4, MEMTXATTRS_UNSPECIFIED, NULL);
    *pa = (pte & ~0xfff) | (va & 0xfff);
    return true;
}

static void ipod_touch_mbx_access(IPodTouchMBXState *s, uint32_t va, uint8_t *buf, uint32_t len, bool write)
{
    while (len) {
        uint32_t chunk = MIN(len, 0x1000 - (va & 0xfff));
        hwaddr pa;

        if (ipod_touch_mbx_translate(s, va, &pa)) {
            address_space_rw(s->dma_as, pa, MEMTXATTRS_UNSPECIFIED, buf, chunk, write);
        } else {
            trace_ipod_touch_mbx_mmu_fault(va);
            if (!write) {
                memset(buf, 0, chunk);
            }
        }
        va += chunk;
        buf += chunk;
        len -= chunk;
    }
}

/*
 * Pixels, converted to and from ARGB8888
 */

static unsigned ipod_touch_mbx_bpp(uint32_t format)
{
    switch (format) {
    case MBX_FMT_ARGB4444:
    case MBX_FMT_RGB555:
    case MBX_FMT_ARGB1555:
    case MBX_FMT_RGB565:
        return 2;
    case MBX_FMT_RGB888:
        return 3;
    case MBX_FMT_ARGB8888:
        return 4;
    default:
        return 0;
    }
}

static inline uint32_t expand5(uint32_t v) { return (v << 3) | (v >> 2); }
static inline uint32_t expand6(uint32_t v) { return (v << 2) | (v >> 4); }

static uint32_t ipod_touch_mbx_unpack(uint32_t format, const uint8_t *p)
{
    uint32_t v;

    switch (format) {
    case MBX_FMT_ARGB4444:
        v = lduw_le_p(p);
        return ((v >> 12) & 0xf) * 0x11000000 | ((v >> 8) & 0xf) * 0x110000 | ((v >> 4) & 0xf) * 0x1100 |
               (v & 0xf) * 0x11;
    case MBX_FMT_RGB555:
    case MBX_FMT_ARGB1555:
        v = lduw_le_p(p);
        return (format == MBX_FMT_RGB555 || (v & 0x8000) ? 0xff000000 : 0) | expand5((v >> 10) & 0x1f) << 16 |
               expand5((v >> 5) & 0x1f) << 8 | expand5(v & 0x1f);
    case MBX_FMT_RGB565:
        v = lduw_le_p(p);
        return 0xff000000 | expand5(v >> 11) << 16 | expand6((v >> 5) & 0x3f) << 8 | expand5(v & 0x1f);
    case MBX_FMT_RGB888:
        return 0xff000000 | p[2] << 16 | p[1] << 8 | p[0];
    case MBX_FMT_ARGB8888:
        return ldl_le_p(p);
    default:
        return 0;
    }
}

static void ipod_touch_mbx_pack(uint32_t format, uint8_t *p, uint32_t argb)
{
    uint32_t a = argb >> 24, r = (argb >> 16) & 0xff, g = (argb >> 8) & 0xff, b = argb & 0xff;

    switch (format) {
    case MBX_FMT_ARGB4444:
        stw_le_p(p, (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4);
        break;
    case MBX_FMT_RGB555:
    case MBX_FMT_ARGB1555:
        stw_le_p(p, (format == MBX_FMT_ARGB1555 && a >= 0x80 ? 0x8000 : 0) | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3);
        break;
    case MBX_FMT_RGB565:
        stw_le_p(p, (r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
        break;
    case MBX_FMT_RGB888:
        p[0] = b;
        p[1] = g;
        p[2] = r;
        break;
    case MBX_FMT_ARGB8888:
        stl_le_p(p, argb);
        break;
    }
}

// Composites a premultiplied source pixel over the destination, scaled by the global alpha in bits 12-19 of the
// blend word.
static uint32_t ipod_touch_mbx_blend(uint32_t blend, uint32_t src, uint32_t dst)
{
    uint32_t global = (blend >> 12) & 0xff;
    uint32_t inv, out = 0;

    if (global != 0xff) {
        src = ((src >> 24) * global / 255) << 24 | (((src >> 16) & 0xff) * global / 255) << 16 |
              (((src >> 8) & 0xff) * global / 255) << 8 | ((src & 0xff) * global / 255);
    }
    inv = 255 - (src >> 24);
    for (int shift = 0; shift < 32; shift += 8) {
        uint32_t c = ((src >> shift) & 0xff) + ((dst >> shift) & 0xff) * inv / 255;
        out |= MIN(c, 0xff) << shift;
    }
    return out;
}

/*
 * 2D core
 */

static void ipod_touch_mbx_2d_reset(IPodTouchMBXState *s)
{
    s->block_len = 0;
    s->blend = 0;
    s->scale_x = 32;
    s->scale_y = 32;
}

static void ipod_touch_mbx_2d_surface(MBX2DSurface *surf, uint32_t header, uint32_t addr)
{
    surf->format = (header >> MBX_2D_SURF_FMT_SHIFT) & MBX_2D_SURF_FMT_MASK;
    surf->stride = sextract32(header, 0, 15);
    surf->addr = addr;
}

static uint32_t ipod_touch_mbx_2d_block_len(uint32_t header)
{
    switch (header >> 28) {
    case MBX_2D_CLIP:
        return 1 + 2 * MAX(header & 0xff, 1);
    case MBX_2D_CTRL:
        return (header & 4) ? 2 : 1;
    case MBX_2D_BLIT:
        return 4;
    case MBX_2D_SRC_SURF:
    case MBX_2D_DST_SURF:
        return 2;
    default:
        return 1;
    }
}

// Maps a destination pixel, at offset (u, v) in the w x h destination rectangle, to its source pixel. The rotations
// follow MBX2D's _pack2DCtxBlitCopy, which moves the source offset when it clips a rotated destination.
static void ipod_touch_mbx_2d_map(IPodTouchMBXState *s, unsigned rot, int u, int v, int w, int h, int *sx, int *sy)
{
    int ux, uy;

    switch (rot) {
    case 1:
        ux = v;
        uy = w - 1 - u;
        break;
    case 2:
        ux = w - 1 - u;
        uy = h - 1 - v;
        break;
    case 3:
        ux = h - 1 - v;
        uy = u;
        break;
    default:
        ux = u;
        uy = v;
        break;
    }
    *sx = s->src_x + ux * 32 / (int)s->scale_x;
    *sy = s->src_y + uy * 32 / (int)s->scale_y;
}

static void ipod_touch_mbx_2d_blit(IPodTouchMBXState *s, uint32_t header, uint32_t colour, uint32_t start,
                                   uint32_t end)
{
    uint32_t rop = header & 0xffff;
    unsigned rot = (header >> MBX_2D_BLIT_ROT_SHIFT) & 3;
    bool odd = rot & 1;
    bool alpha = header & MBX_2D_BLIT_ALPHA;
    bool use_src = rop == 0xcccc;
    unsigned dst_bpp = ipod_touch_mbx_bpp(s->dst.format);
    unsigned src_bpp = ipod_touch_mbx_bpp(s->src.format);
    // the rectangle words hold x in their high half, unless the blit rotates by 90 or 270 degrees
    int x0 = odd ? start & 0x1fff : (start >> 16) & 0x1fff;
    int y0 = odd ? (start >> 16) & 0x1fff : start & 0x1fff;
    int x1 = odd ? end & 0x1fff : (end >> 16) & 0x1fff;
    int y1 = odd ? (end >> 16) & 0x1fff : end & 0x1fff;
    int w = x1 - x0, h = y1 - y0;
    int cx0 = x0, cy0 = y0, cx1 = x1, cy1 = y1;
    int sx_min = 0, sy_min = 0, sw = 0, sh = 0;
    uint32_t fill = 0;
    uint8_t *src_buf = NULL, *row;

    trace_ipod_touch_mbx_2d_blit(rop, rot, x0, y0, x1, y1, alpha ? s->blend : 0);
    trace_ipod_touch_mbx_2d_surfaces(s->dst.addr, s->dst.format, s->dst.stride, use_src ? s->src.addr : colour,
                                     s->src.format, s->src.stride, s->src_x, s->src_y);
    if (rop != 0xcccc && rop != 0xf0f0) {
        qemu_log_mask(LOG_UNIMP, "%s: ROP 0x%04x not implemented\n", __func__, rop);
        return;
    }
    if (!dst_bpp || (use_src && !src_bpp)) {
        qemu_log_mask(LOG_UNIMP, "%s: surface format %u/%u not implemented\n", __func__, s->dst.format,
                      s->src.format);
        return;
    }
    if (header & MBX_2D_BLIT_CLIP) {
        uint32_t cx = odd ? s->clip_y0 : s->clip_x0, cy = odd ? s->clip_x0 : s->clip_y0;
        uint32_t cxe = odd ? s->clip_y1 : s->clip_x1, cye = odd ? s->clip_x1 : s->clip_y1;
        cx0 = MAX(cx0, (int)cx);
        cy0 = MAX(cy0, (int)cy);
        cx1 = MIN(cx1, (int)cxe);
        cy1 = MIN(cy1, (int)cye);
    }
    if (cx1 <= cx0 || cy1 <= cy0) {
        return;
    }

    if (use_src) {
        // read the source pixels the clipped rectangle maps to; this also makes overlapping copies safe
        int corners[4][2] = { { cx0, cy0 }, { cx1 - 1, cy0 }, { cx0, cy1 - 1 }, { cx1 - 1, cy1 - 1 } };
        int sx_max = INT_MIN, sy_max = INT_MIN;

        sx_min = INT_MAX;
        sy_min = INT_MAX;
        for (int i = 0; i < 4; i++) {
            int sx, sy;
            ipod_touch_mbx_2d_map(s, rot, corners[i][0] - x0, corners[i][1] - y0, w, h, &sx, &sy);
            sx_min = MIN(sx_min, sx);
            sy_min = MIN(sy_min, sy);
            sx_max = MAX(sx_max, sx);
            sy_max = MAX(sy_max, sy);
        }
        sw = sx_max - sx_min + 1;
        sh = sy_max - sy_min + 1;
        src_buf = g_malloc((size_t)sw * sh * src_bpp);
        for (int y = 0; y < sh; y++) {
            ipod_touch_mbx_access(s, s->src.addr + (int64_t)(sy_min + y) * s->src.stride + sx_min * src_bpp,
                                  src_buf + (size_t)y * sw * src_bpp, sw * src_bpp, false);
        }
    } else {
        // fill colours are ARGB8888 for blended fills and in the destination format otherwise
        uint8_t raw[4];
        stl_le_p(raw, colour);
        fill = alpha ? colour : ipod_touch_mbx_unpack(s->dst.format, raw);
    }

    row = g_malloc((size_t)(cx1 - cx0) * dst_bpp);
    for (int y = cy0; y < cy1; y++) {
        uint32_t row_addr = s->dst.addr + (int64_t)y * s->dst.stride + cx0 * dst_bpp;

        if (alpha) {
            ipod_touch_mbx_access(s, row_addr, row, (cx1 - cx0) * dst_bpp, false);
        }
        for (int x = cx0; x < cx1; x++) {
            uint8_t *p = row + (x - cx0) * dst_bpp;
            uint32_t pixel = fill;

            if (use_src) {
                int sx, sy;
                ipod_touch_mbx_2d_map(s, rot, x - x0, y - y0, w, h, &sx, &sy);
                pixel = ipod_touch_mbx_unpack(s->src.format,
                                              src_buf + ((size_t)(sy - sy_min) * sw + (sx - sx_min)) * src_bpp);
            }
            if (alpha) {
                pixel = ipod_touch_mbx_blend(s->blend, pixel, ipod_touch_mbx_unpack(s->dst.format, p));
            }
            ipod_touch_mbx_pack(s->dst.format, p, pixel);
        }
        ipod_touch_mbx_access(s, row_addr, row, (cx1 - cx0) * dst_bpp, true);
    }
    g_free(row);
    g_free(src_buf);
}

static void ipod_touch_mbx_2d_execute(IPodTouchMBXState *s)
{
    uint32_t header = s->block[0];

    switch (header >> 28) {
    case MBX_2D_CLIP:
        if ((header & 0xff) > 1) {
            qemu_log_mask(LOG_UNIMP, "%s: %u clip rectangles, using the first\n", __func__, header & 0xff);
        }
        s->clip_x0 = s->block[1] & 0xffff;
        s->clip_x1 = s->block[1] >> 16;
        s->clip_y0 = s->block[2] & 0xffff;
        s->clip_y1 = s->block[2] >> 16;
        break;
    case MBX_2D_CTRL:
        if (header & 4) {
            s->blend = s->block[1];
        }
        if (header & ~(0xf0000000 | 4)) {
            qemu_log_mask(LOG_UNIMP, "%s: control block 0x%08x not implemented\n", __func__, header);
        }
        break;
    case MBX_2D_SRC_OFF:
        s->src_x = (header >> 14) & 0x1fff;
        s->src_y = header & 0x1fff;
        break;
    case MBX_2D_SCALE:
        s->scale_x = (header >> 18) & 0x3ff ?: 32;
        s->scale_y = (header >> 4) & 0x3ff ?: 32;
        break;
    case MBX_2D_FENCE:
        break;
    case MBX_2D_BLIT:
        ipod_touch_mbx_2d_blit(s, header, s->block[1], s->block[2], s->block[3]);
        break;
    case MBX_2D_SRC_SURF:
        ipod_touch_mbx_2d_surface(&s->src, header, s->block[1]);
        break;
    case MBX_2D_DST_SURF:
        ipod_touch_mbx_2d_surface(&s->dst, header, s->block[1]);
        break;
    case MBX_2D_FLUSH:
        trace_ipod_touch_mbx_2d_flush();
        timer_mod(s->timer_2d, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MBX_2D_DELAY_NS);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: 2D block 0x%08x not implemented\n", __func__, header);
        break;
    }
}

static void ipod_touch_mbx_2d_push(IPodTouchMBXState *s, uint32_t word)
{
    s->block[s->block_len++] = word;
    if (s->block_len < ipod_touch_mbx_2d_block_len(s->block[0])) {
        return;
    }
    trace_ipod_touch_mbx_2d_block(s->block[0], s->block_len);
    ipod_touch_mbx_2d_execute(s);
    s->block_len = 0;
}

/*
 * 3D core
 */

static uint32_t ipod_touch_mbx_ldl(IPodTouchMBXState *s, uint32_t va)
{
    uint8_t buf[4];
    ipod_touch_mbx_access(s, va, buf, 4, false);
    return ldl_le_p(buf);
}

static float ipod_touch_mbx_ldf(IPodTouchMBXState *s, uint32_t va)
{
    union { uint32_t u; float f; } v = { .u = ipod_touch_mbx_ldl(s, va) };
    return v.f;
}

// Maps the pixel format codes of the render target mode and of texture control words to the 2D formats.
static uint32_t ipod_touch_mbx_3d_format(uint32_t code)
{
    switch (code) {
    case 0:
    case 3:
        return MBX_FMT_ARGB1555;
    case 1:
        return MBX_FMT_RGB565;
    case 2:
        return MBX_FMT_ARGB4444;
    case 6:
        return MBX_FMT_ARGB8888;
    default:
        return 0;
    }
}

typedef struct MBXRenderTarget {
    uint32_t addr;
    uint32_t stride;
    uint32_t format;
    unsigned bpp;
    int x0, y0, x1, y1;
    uint8_t *pixels; // the render rectangle, read before and written back after the render
    uint8_t *mask;   // pixels the depth-only objects left visible
} MBXRenderTarget;

typedef struct MBXTexture {
    uint32_t addr;
    uint32_t stride;
    uint32_t format;
    unsigned bpp;
    int w, h;
    int x0, y0, cw, ch; // the part of the texture the object samples, cached in data
    uint8_t *data;
    bool is_target;
} MBXTexture;

// Decodes a texture's two control words: log2(width) - 3 in bits 24-27 and log2(height) - 3 in bits 20-23 of the
// first, its format in bits 14-17; the stride in pixels is split over bit 0 (bit 3 of the stride) and bits 18-24 of
// the second (bits 4-10), which also holds the address >> 7 in bits 0-17.
static bool ipod_touch_mbx_3d_texture(IPodTouchMBXState *s, MBXRenderTarget *rt, uint32_t a, uint32_t b,
                                      MBXTexture *tex)
{
    memset(tex, 0, sizeof(*tex));
    tex->w = 8 << ((a >> 24) & 0xf);
    tex->h = 8 << ((a >> 20) & 0xf);
    tex->format = ipod_touch_mbx_3d_format((a >> 14) & 0xf);
    tex->bpp = ipod_touch_mbx_bpp(tex->format);
    tex->stride = ((((b >> 18) & 0x7f) << 4) | ((a & 1) << 3)) * tex->bpp;
    tex->addr = (b & 0x3ffff) << 7;
    tex->is_target = tex->addr == rt->addr && tex->stride == rt->stride && tex->format == rt->format;
    if (!tex->bpp) {
        qemu_log_mask(LOG_UNIMP, "%s: texture format %u not implemented\n", __func__, (a >> 14) & 0xf);
        return false;
    }
    return true;
}

static void ipod_touch_mbx_3d_texture_load(IPodTouchMBXState *s, MBXTexture *tex, float umin, float vmin,
                                           float umax, float vmax)
{
    int x0 = MAX((int)floorf(umin * tex->w) - 1, 0), y0 = MAX((int)floorf(vmin * tex->h) - 1, 0);
    int x1 = MIN((int)ceilf(umax * tex->w) + 1, tex->w), y1 = MIN((int)ceilf(vmax * tex->h) + 1, tex->h);

    tex->x0 = x0;
    tex->y0 = y0;
    tex->cw = MAX(x1 - x0, 1);
    tex->ch = MAX(y1 - y0, 1);
    tex->data = g_malloc0((size_t)tex->cw * tex->ch * tex->bpp);
    for (int y = 0; y < tex->ch; y++) {
        ipod_touch_mbx_access(s, tex->addr + (y0 + y) * tex->stride + x0 * tex->bpp,
                              tex->data + (size_t)y * tex->cw * tex->bpp, tex->cw * tex->bpp, false);
    }
}

static uint32_t ipod_touch_mbx_3d_sample(MBXRenderTarget *rt, MBXTexture *tex, float u, float v)
{
    int x = (int)floorf(u * tex->w), y = (int)floorf(v * tex->h);

    if (tex->is_target) {
        x = MIN(MAX(x, rt->x0), rt->x1 - 1);
        y = MIN(MAX(y, rt->y0), rt->y1 - 1);
        return ipod_touch_mbx_unpack(rt->format,
                                     rt->pixels + ((size_t)(y - rt->y0) * (rt->x1 - rt->x0) + x - rt->x0) * rt->bpp);
    }
    x = MIN(MAX(x - tex->x0, 0), tex->cw - 1);
    y = MIN(MAX(y - tex->y0, 0), tex->ch - 1);
    return ipod_touch_mbx_unpack(tex->format, tex->data + ((size_t)y * tex->cw + x) * tex->bpp);
}

static uint32_t ipod_touch_mbx_scale(uint32_t argb, uint32_t alpha)
{
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        out |= (((argb >> shift) & 0xff) * alpha / 255) << shift;
    }
    return out;
}

static uint32_t ipod_touch_mbx_over(uint32_t src, uint32_t dst)
{
    uint32_t inv = 255 - (src >> 24), out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        out |= MIN(((src >> shift) & 0xff) + ((dst >> shift) & 0xff) * inv / 255, 0xff) << shift;
    }
    return out;
}

#define MBX_3D_MAX_ATTRS 5 // colour, then u and v of up to two textures

// Rasterises a four-vertex strip (triangles 0-1-2 and 1-2-3), covering each pixel at most once. For every covered
// pixel inside the render rectangle, calls the shader with the attributes interpolated at its centre.
static void ipod_touch_mbx_3d_strip(MBXRenderTarget *rt, float xy[4][2], float attr[4][MBX_3D_MAX_ATTRS],
                                    void (*shade)(MBXRenderTarget *, int, int, float *, void *), void *opaque)
{
    static const int tris[2][3] = { { 0, 1, 2 }, { 1, 2, 3 } };
    float xmin = xy[0][0], xmax = xy[0][0], ymin = xy[0][1], ymax = xy[0][1];

    for (int i = 1; i < 4; i++) {
        xmin = MIN(xmin, xy[i][0]);
        xmax = MAX(xmax, xy[i][0]);
        ymin = MIN(ymin, xy[i][1]);
        ymax = MAX(ymax, xy[i][1]);
    }
    int px0 = MAX(rt->x0, (int)floorf(xmin)), px1 = MIN(rt->x1, (int)ceilf(xmax));
    int py0 = MAX(rt->y0, (int)floorf(ymin)), py1 = MIN(rt->y1, (int)ceilf(ymax));

    for (int y = py0; y < py1; y++) {
        for (int x = px0; x < px1; x++) {
            float cx = x + 0.5f, cy = y + 0.5f;

            for (int t = 0; t < 2; t++) {
                const float *a = xy[tris[t][0]], *b = xy[tris[t][1]], *c = xy[tris[t][2]];
                float area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
                float w0, w1, w2, out[MBX_3D_MAX_ATTRS];

                if (area == 0) {
                    continue;
                }
                w0 = ((b[0] - cx) * (c[1] - cy) - (b[1] - cy) * (c[0] - cx)) / area;
                w1 = ((c[0] - cx) * (a[1] - cy) - (c[1] - cy) * (a[0] - cx)) / area;
                w2 = 1.0f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) {
                    continue;
                }
                for (int i = 0; i < MBX_3D_MAX_ATTRS; i++) {
                    out[i] = w0 * attr[tris[t][0]][i] + w1 * attr[tris[t][1]][i] + w2 * attr[tris[t][2]][i];
                }
                shade(rt, x, y, out, opaque);
                break;
            }
        }
    }
}

static void ipod_touch_mbx_3d_shade_mask(MBXRenderTarget *rt, int x, int y, float *attr, void *opaque)
{
    rt->mask[(y - rt->y0) * (rt->x1 - rt->x0) + x - rt->x0] = *(uint8_t *)opaque;
}

typedef struct MBXShader {
    int textures;
    MBXTexture tex[2];
    uint32_t colour;
} MBXShader;

static void ipod_touch_mbx_3d_shade(MBXRenderTarget *rt, int x, int y, float *attr, void *opaque)
{
    MBXShader *sh = opaque;
    size_t i = (size_t)(y - rt->y0) * (rt->x1 - rt->x0) + x - rt->x0;
    uint8_t *p = rt->pixels + i * rt->bpp;
    uint32_t alpha = sh->colour >> 24, pixel;

    if (!rt->mask[i]) {
        return;
    }
    if (sh->textures == 0) {
        pixel = sh->colour;
    } else if (sh->textures == 2) {
        // MBX2D's blended copies read the destination back as the second texture: premultiplied source over it,
        // scaled by the vertex alpha
        uint32_t src = ipod_touch_mbx_3d_sample(rt, &sh->tex[0], attr[1], attr[2]);
        uint32_t dst = ipod_touch_mbx_3d_sample(rt, &sh->tex[1], attr[3], attr[4]);
        pixel = ipod_touch_mbx_over(ipod_touch_mbx_scale(src, alpha), dst);
    } else if (sh->tex[0].is_target) {
        // blended colour fills read the destination back as their only texture
        uint32_t dst = ipod_touch_mbx_3d_sample(rt, &sh->tex[0], attr[1], attr[2]);
        pixel = ipod_touch_mbx_over(ipod_touch_mbx_scale(sh->colour | 0xff000000, alpha), dst);
    } else {
        pixel = ipod_touch_mbx_3d_sample(rt, &sh->tex[0], attr[1], attr[2]);
    }
    ipod_touch_mbx_pack(rt->format, p, pixel);
}

// Draws one object. Its pointer holds the block's offset in words in bits 0-19 and its layout in bits 20-27:
// 0x02 is a depth-only quad (the ISP word, four x/y pairs and four z), the others start with a header word and the
// control words of 0 (0x0a), 1 (0x12) or 2 (0x1a) textures, each followed by its TSP word, then the ISP word, the
// four x/y pairs, four z and four 1/w, and per vertex the colour and the u/v pairs of the textures.
static void ipod_touch_mbx_3d_object(IPodTouchMBXState *s, MBXRenderTarget *rt, uint32_t objects, uint32_t ptr)
{
    uint32_t layout = (ptr >> 20) & 0xff;
    uint32_t va = objects + (ptr & 0xfffff) * 4;
    float xy[4][2], attr[4][MBX_3D_MAX_ATTRS] = { { 0 } };
    MBXShader sh = { 0 };
    uint32_t isp;

    trace_ipod_touch_mbx_3d_object(ptr);
    if ((layout & 7) != 2 || layout > 0x1a) {
        qemu_log_mask(LOG_UNIMP, "%s: object 0x%08x not implemented\n", __func__, ptr);
        return;
    }
    if (layout == 0x02) {
        uint8_t visible;

        isp = ipod_touch_mbx_ldl(s, va);
        for (int i = 0; i < 4; i++) {
            xy[i][0] = ipod_touch_mbx_ldf(s, va + 4 + i * 8);
            xy[i][1] = ipod_touch_mbx_ldf(s, va + 8 + i * 8);
        }
        // MBX2D's 3D blits clip with two such quads: the whole screen with ISP bit 12 clear, then the clip
        // rectangle with it set
        visible = (isp >> 12) & 1;
        ipod_touch_mbx_3d_strip(rt, xy, attr, ipod_touch_mbx_3d_shade_mask, &visible);
        return;
    }

    sh.textures = (layout >> 3) - 1;
    va += 4;
    for (int t = 0; t < sh.textures; t++) {
        uint32_t a = ipod_touch_mbx_ldl(s, va), b = ipod_touch_mbx_ldl(s, va + 4);
        trace_ipod_touch_mbx_3d_texture(t, a, b, ipod_touch_mbx_ldl(s, va + 8));
        if (!ipod_touch_mbx_3d_texture(s, rt, a, b, &sh.tex[t])) {
            return;
        }
        va += 12;
    }
    isp = ipod_touch_mbx_ldl(s, va);
    va += 4;
    for (int i = 0; i < 4; i++) {
        xy[i][0] = ipod_touch_mbx_ldf(s, va + i * 8);
        xy[i][1] = ipod_touch_mbx_ldf(s, va + 4 + i * 8);
    }
    va += 4 * 4 * 4; // x/y pairs, z and 1/w
    for (int i = 0; i < 4; i++) {
        uint32_t colour = ipod_touch_mbx_ldl(s, va);
        va += 4;
        // the blits have a single colour, so it is not interpolated
        sh.colour = colour;
        for (int j = 0; j < sh.textures * 2; j++) {
            attr[i][1 + j] = ipod_touch_mbx_ldf(s, va);
            va += 4;
        }
    }
    trace_ipod_touch_mbx_3d_draw(sh.textures, isp, sh.colour, (int)xy[0][0], (int)xy[0][1], (int)xy[3][0], (int)xy[3][1]);

    for (int t = 0; t < sh.textures; t++) {
        float umin = attr[0][1 + 2 * t], umax = umin, vmin = attr[0][2 + 2 * t], vmax = vmin;
        for (int i = 1; i < 4; i++) {
            umin = MIN(umin, attr[i][1 + 2 * t]);
            umax = MAX(umax, attr[i][1 + 2 * t]);
            vmin = MIN(vmin, attr[i][2 + 2 * t]);
            vmax = MAX(vmax, attr[i][2 + 2 * t]);
        }
        if (!sh.tex[t].is_target) {
            ipod_touch_mbx_3d_texture_load(s, &sh.tex[t], umin, vmin, umax, vmax);
        }
    }
    ipod_touch_mbx_3d_strip(rt, xy, attr, ipod_touch_mbx_3d_shade, &sh);
    for (int t = 0; t < sh.textures; t++) {
        g_free(sh.tex[t].data);
    }
}

// Renders the object lists of the regions in MBX_3D_REGIONS into the render target. The regions of a 3D blit
// all share one object list, which is drawn once over the whole render rectangle.
static void ipod_touch_mbx_3d_render(IPodTouchMBXState *s)
{
    MBXRenderTarget rt = {
        .addr = s->regs[MBX_3D_RT_ADDR / 4],
        .stride = s->regs[MBX_3D_RT_STRIDE / 4] * 4,
        .format = ipod_touch_mbx_3d_format(s->regs[MBX_3D_RT_MODE / 4]),
        .x0 = s->regs[MBX_3D_RT_XRANGE / 4] & 0xffff,
        .x1 = s->regs[MBX_3D_RT_XRANGE / 4] >> 16,
        .y0 = s->regs[MBX_3D_RT_YRANGE / 4] & 0xffff,
        .y1 = s->regs[MBX_3D_RT_YRANGE / 4] >> 16,
    };
    uint32_t objects = s->regs[MBX_3D_OBJECTS / 4];
    uint32_t region = s->regs[MBX_3D_REGIONS / 4];
    uint32_t list = ipod_touch_mbx_ldl(s, region + 4) & MBX_3D_REGION_LIST_MASK;
    int w = rt.x1 - rt.x0, h = rt.y1 - rt.y0;

    rt.bpp = ipod_touch_mbx_bpp(rt.format);
    trace_ipod_touch_mbx_3d_render(rt.addr, rt.format, rt.stride, rt.x0, rt.y0, rt.x1, rt.y1);
    if (!rt.bpp || w <= 0 || h <= 0) {
        qemu_log_mask(LOG_UNIMP, "%s: render target mode %u not implemented\n", __func__,
                      s->regs[MBX_3D_RT_MODE / 4]);
        return;
    }

    rt.pixels = g_malloc((size_t)w * h * rt.bpp);
    rt.mask = g_malloc((size_t)w * h);
    memset(rt.mask, 1, (size_t)w * h);
    for (int y = 0; y < h; y++) {
        ipod_touch_mbx_access(s, rt.addr + (rt.y0 + y) * rt.stride + rt.x0 * rt.bpp,
                              rt.pixels + (size_t)y * w * rt.bpp, w * rt.bpp, false);
    }
    for (int i = 0; i < MBX_3D_MAX_OBJECTS; i++) {
        uint32_t ptr = ipod_touch_mbx_ldl(s, list + i * 4);
        if (ptr == MBX_3D_OBJECT_END) {
            break;
        }
        ipod_touch_mbx_3d_object(s, &rt, objects, ptr);
    }
    for (int y = 0; y < h; y++) {
        ipod_touch_mbx_access(s, rt.addr + (rt.y0 + y) * rt.stride + rt.x0 * rt.bpp,
                              rt.pixels + (size_t)y * w * rt.bpp, w * rt.bpp, true);
    }
    g_free(rt.pixels);
    g_free(rt.mask);
}

/*
 * Registers
 */

static uint64_t ipod_touch_mbx1_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodTouchMBXState *s = opaque;
    uint64_t val;

    if (addr >= sizeof(s->regs)) {
        qemu_log_mask(LOG_UNIMP, "%s: read from 0x%06" HWADDR_PRIx "\n", __func__, addr);
        return 0;
    }
    switch (addr) {
    case MBX_INT_STATUS:
        // the 2D FIFO (bits 16-23) is always empty: blocks are carried out as they arrive
        val = s->int_status;
        break;
    case MBX_INT_CLEAR:
    case MBX_3D_START:
    case MBX_TA_START:
    case MBX_REGION_START:
    case MBX_PB_INIT:
        val = 0;
        break;
    case MBX_CORE_ID:
        val = (1 << 24) | (2 << 16); // the only revision AppleMBXDevice::start accepts
        break;
    case MBX_MMU_CTRL:
        // the MMU state follows the enable bit at once, and TLB reloads finish at once
        val = (s->mmu_ctrl & ~(MBX_MMU_CTRL_ENABLED | MBX_MMU_CTRL_RELOAD_BUSY)) |
              ((s->mmu_ctrl & MBX_MMU_CTRL_ENABLE) ? MBX_MMU_CTRL_ENABLED : 0);
        break;
    default:
        val = s->regs[addr / 4];
        break;
    }
    trace_ipod_touch_mbx_read(addr, val);
    return val;
}

static void ipod_touch_mbx1_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IPodTouchMBXState *s = opaque;

    if (addr >= MBX_2D_PORT && addr < MBX_2D_PORT + MBX_2D_PORT_SIZE) {
        ipod_touch_mbx_2d_push(s, val);
        return;
    }
    if (addr >= MBX_TA_PORT && addr < MBX_TA_PORT + MBX_TA_PORT_SIZE) {
        trace_ipod_touch_mbx_ta_write(addr - MBX_TA_PORT, val);
        return;
    }
    if (addr >= sizeof(s->regs)) {
        qemu_log_mask(LOG_UNIMP, "%s: write 0x%08" PRIx64 " to 0x%06" HWADDR_PRIx "\n", __func__, val, addr);
        return;
    }

    trace_ipod_touch_mbx_write(addr, val);
    switch (addr) {
    case MBX_SOFT_RESET:
        if (val) {
            ipod_touch_mbx_2d_reset(s);
        }
        break;
    case MBX_INT_STATUS:
        s->int_status |= val & MBX_INT_EVENTS;
        break;
    case MBX_INT_CLEAR:
        s->int_status &= ~val;
        break;
    case MBX_MMU_CTRL:
        s->mmu_ctrl = val & ~MBX_MMU_CTRL_RELOAD;
        break;
    case MBX_3D_START:
        if (val & 1) {
            trace_ipod_touch_mbx_3d_start();
            ipod_touch_mbx_3d_render(s);
            timer_mod(s->timer_3d, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MBX_3D_DELAY_NS);
        }
        break;
    case MBX_TA_START:
        if (val & 1) {
            qemu_log_mask(LOG_UNIMP, "%s: tile accelerator not implemented\n", __func__);
        }
        break;
    case MBX_REGION_START:
        if (val & 1) {
            s->int_status |= MBX_INT_REGION_DONE;
        }
        break;
    case MBX_PB_INIT:
        if (val & 1) {
            s->int_status |= MBX_INT_RENDER_DONE;
        }
        break;
    }
    s->regs[addr / 4] = val;
    ipod_touch_mbx_update_irq(s);
}

// MBX2 is the "wrapper" around the core (AppleMBXController): a read cache and a write buffer.
static uint64_t ipod_touch_mbx2_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodTouchMBXState *s = opaque;

    switch (addr) {
    case 0x4:
        return 0xff; // the write buffer is drained when this reads anything but 1
    case 0xc:
        return s->wrapper_regs[addr / 4] & ~1; // resets finish at once
    default:
        return s->wrapper_regs[addr / 4];
    }
}

static void ipod_touch_mbx2_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IPodTouchMBXState *s = opaque;
    s->wrapper_regs[addr / 4] = val;
}

static const MemoryRegionOps ipod_touch_mbx1_ops = {
    .read = ipod_touch_mbx1_read,
    .write = ipod_touch_mbx1_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static const MemoryRegionOps ipod_touch_mbx2_ops = {
    .read = ipod_touch_mbx2_read,
    .write = ipod_touch_mbx2_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void ipod_touch_mbx_reset(DeviceState *dev)
{
    IPodTouchMBXState *s = IPOD_TOUCH_MBX(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->wrapper_regs, 0, sizeof(s->wrapper_regs));
    s->int_status = 0;
    s->mmu_ctrl = 0;
    timer_del(s->timer_2d);
    timer_del(s->timer_3d);
    ipod_touch_mbx_2d_reset(s);
    qemu_irq_lower(s->irq);
}

static void ipod_touch_mbx_init(Object *obj)
{
    IPodTouchMBXState *s = IPOD_TOUCH_MBX(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem1, obj, &ipod_touch_mbx1_ops, s, TYPE_IPOD_TOUCH_MBX, MBX1_SIZE);
    sysbus_init_mmio(sbd, &s->iomem1);
    memory_region_init_io(&s->iomem2, obj, &ipod_touch_mbx2_ops, s, TYPE_IPOD_TOUCH_MBX, MBX2_SIZE);
    sysbus_init_mmio(sbd, &s->iomem2);
    sysbus_init_irq(sbd, &s->irq);
    s->timer_2d = timer_new_ns(QEMU_CLOCK_VIRTUAL, ipod_touch_mbx_2d_done, s);
    s->timer_3d = timer_new_ns(QEMU_CLOCK_VIRTUAL, ipod_touch_mbx_3d_done, s);
    s->dma_as = &address_space_memory;
}

static void ipod_touch_mbx_finalize(Object *obj)
{
    IPodTouchMBXState *s = IPOD_TOUCH_MBX(obj);
    timer_free(s->timer_2d);
    timer_free(s->timer_3d);
}

static const VMStateDescription vmstate_mbx_2d_surface = {
    .name = "ipodtouch.mbx.2d-surface",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(addr, MBX2DSurface),
        VMSTATE_INT32(stride, MBX2DSurface),
        VMSTATE_UINT32(format, MBX2DSurface),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_ipod_touch_mbx = {
    .name = TYPE_IPOD_TOUCH_MBX,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IPodTouchMBXState, MBX_NUM_REGS),
        VMSTATE_UINT32_ARRAY(wrapper_regs, IPodTouchMBXState, MBX2_SIZE / 4),
        VMSTATE_UINT32(int_status, IPodTouchMBXState),
        VMSTATE_UINT32(mmu_ctrl, IPodTouchMBXState),
        VMSTATE_TIMER_PTR(timer_2d, IPodTouchMBXState),
        VMSTATE_TIMER_PTR(timer_3d, IPodTouchMBXState),
        VMSTATE_UINT32_ARRAY(block, IPodTouchMBXState, MBX_2D_MAX_BLOCK),
        VMSTATE_UINT32(block_len, IPodTouchMBXState),
        VMSTATE_STRUCT(dst, IPodTouchMBXState, 1, vmstate_mbx_2d_surface, MBX2DSurface),
        VMSTATE_STRUCT(src, IPodTouchMBXState, 1, vmstate_mbx_2d_surface, MBX2DSurface),
        VMSTATE_UINT32(src_x, IPodTouchMBXState),
        VMSTATE_UINT32(src_y, IPodTouchMBXState),
        VMSTATE_UINT32(blend, IPodTouchMBXState),
        VMSTATE_UINT32(clip_x0, IPodTouchMBXState),
        VMSTATE_UINT32(clip_x1, IPodTouchMBXState),
        VMSTATE_UINT32(clip_y0, IPodTouchMBXState),
        VMSTATE_UINT32(clip_y1, IPodTouchMBXState),
        VMSTATE_UINT32(scale_x, IPodTouchMBXState),
        VMSTATE_UINT32(scale_y, IPodTouchMBXState),
        VMSTATE_END_OF_LIST()
    }
};

static void ipod_touch_mbx_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = ipod_touch_mbx_reset;
    dc->vmsd = &vmstate_ipod_touch_mbx;
}

static const TypeInfo ipod_touch_mbx_type_info = {
    .name = TYPE_IPOD_TOUCH_MBX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchMBXState),
    .instance_init = ipod_touch_mbx_init,
    .instance_finalize = ipod_touch_mbx_finalize,
    .class_init = ipod_touch_mbx_class_init,
};

static void ipod_touch_mbx_register_types(void)
{
    type_register_static(&ipod_touch_mbx_type_info);
}

type_init(ipod_touch_mbx_register_types)
