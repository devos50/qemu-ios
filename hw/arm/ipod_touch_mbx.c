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
 *  - The 3D core is not emulated yet: renders (3D blits and scenes) complete without drawing.
 */
#include "qemu/osdep.h"
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
