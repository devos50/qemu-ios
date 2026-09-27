/*
 * mbxshim -- a drop-in replacement for
 * /System/Library/Frameworks/OpenGLES.framework/MBXGLEngine.bundle
 *
 * The stock bundle drives the PowerVR MBX through IOKit. This one forwards
 * every GL call to QEMU instead, where a real host OpenGL context executes it.
 * No IOKit, no AppleMBX, no MBX register contract.
 *
 * WHAT THE FRAMEWORK ACTUALLY REQUIRES OF US
 *
 * All of it was read out of the device's own MBXGLEngine, not assumed. Verified
 * against REAL HARDWARE 2026-08-05: the binary pulled off a physical iPod touch
 * 2G (MB528, build 7E18) is md5 92ddd55fc2968356835760b0ec9b4a15, and that is
 * byte-identical to the ARMV6 SLICE of the 3.1.3 SDK copy. Note the slice: the
 * SDK ships a fat armv6+armv7 bundle (549232 bytes, md5 90c5c928...), so a
 * whole-file comparison against the device's 299648 bytes differs and looks
 * alarming. `lipo -thin armv6` first.
 *
 *   - The bundle exports exactly ONE symbol, _GLESGetEGLInterface. Everything
 *     else reaches us through the table it returns. `nm -gU` on the real binary
 *     shows 43 exports and the only GLES* one is that.
 *
 *   - GLESGetEGLInterface is three instructions: return &table. The table is
 *     nine function pointers, then pairs of {const char *extension, u32 bit}.
 *     The framework never indexes past the ninth pointer.
 *
 *   - GLESCreateGC(sharegroup, X+0x10, X+0xCE8, X+0xC) is called from
 *     -[EAGLContext initWithAPI:properties:] on a 6592-byte calloc'd block X.
 *     The real one callocs a 5440-byte GC, stores its two pointer arguments
 *     inside that GC, writes the GC through its FOURTH argument (`str r5,[r11]`
 *     at 0x9b40), and RETURNS 1 ON SUCCESS, 0 on failure.
 *
 *     That return value is easy to get backwards, and getting it backwards
 *     costs a whole debug cycle: -[EAGLContext initWithAPI:] simply hands back
 *     nil, after our bundle has been loaded and all three of our entry points
 *     called, so every log line looks healthy. The success path is not the
 *     function's fall-through epilogue -- that one is `mov r0,#0` and is the
 *     failure exit. Success leaves through `cmp r0,#1; beq` at 0x9de8, where
 *     the comparison itself has already put 1 in r0 and the branch just runs
 *     into the pop. Reading only the epilogue makes it look like every path
 *     returns 0.
 *
 *   - X+0x10 is the dispatch table the framework's gl* trampolines index. Each
 *     trampoline is six instructions: read the context from TSD key 30, load
 *     the function from ctx+(0x10+slot*4), load the GC from ctx+0xC, tail-call.
 *     So arg0 of every entry point is the GC, and we own every table entry.
 *
 * Build with contrib/armv6-toolchain.
 */

#include "gles_stubs.h"

/* ------------------------------------------------------------ QEMU_CALL --- */

#define QC_GLES 0x140

#define QC_GLES_INLINE_ARGS 4

/* Mirrors qemu_call_t: call_number(4) + args(32) + retval(8) + error(8) = 52,
 * packed. Frozen -- see include/hw/arm/guest-services/general.h. */
typedef struct __attribute__((packed)) {
    unsigned int call_number;
    struct __attribute__((packed)) {
        unsigned int slot;
        unsigned int ctx;
        unsigned int argc;
        unsigned int spill;
        unsigned int args[QC_GLES_INLINE_ARGS];
    } gles;
    long long retval;
    long long error;
} qemu_call_t;

#define GLES_OP_PRESENT         0x1000
#define GLES_OP_PRESENT_SURFACE 0x1001
#define GLES_OP_DRAWABLE_STORAGE 0x1008

/* The two drawable formats CA hands the engine. Declared up here because the
 * surface plausibility check needs the pixel size before the drawable code
 * below gets to them; see the fourcc mapping at GLESBindView. */
#define CA_FOURCC_BGRA 0x42475241
#define CA_FOURCC_555L 0x4c353535
#define CA_FOURCC_565L 0x4c353635

extern long write(int, const void *, unsigned long);

static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
/*
 * The shim's log goes to fd 2 AND to the host.
 *
 * fd 2 is nowhere readable for an app SpringBoard launched, so everything this
 * file reports -- whether CoreAnimation accepted a surface, whether it accepted
 * the frames presented into it -- used to be invisible from the QEMU side. Both
 * facts are decided here and cannot be inferred from the host.
 */
#define GLES_OP_LOG 0x1002
#define GLES_OP_BIND_SURFACE 0x1003
#define GLES_OP_NEW_SHAREGROUP 0x1004
#define GLES_OP_DELETE_SHAREGROUP 0x1005
#define GLES_OP_NEW_CONTEXT 0x1006
#define GLES_OP_DELETE_CONTEXT 0x1007

typedef struct { unsigned host, unpack_alignment; } GuestGC;
extern void *calloc(unsigned long, unsigned long);
extern void free(void *);
static long long qc(unsigned slot, void *gc, unsigned argc, const unsigned *args);
#define A(...) (const unsigned[]){ __VA_ARGS__ }
static void w(const char *s)
{
    unsigned n = slen(s);

    write(2, s, n);
    qc(GLES_OP_LOG, 0, 2, A((unsigned)(unsigned long)s, n));
}
static void wd(unsigned v)
{
    char b[12], *p = b + 11;
    *p = 0;
    if (!v) *--p = '0';
    while (v) { *--p = '0' + (v % 10); v /= 10; }
    w(p);
}

static void wx(unsigned long v)
{
    char b[11], *p = b + 10;
    *p = 0;
    if (!v) *--p = '0';
    while (v) { *--p = "0123456789abcdef"[v & 15]; v >>= 4; }
    w("0x"); w(p);
}

static long long qc(unsigned slot, void *gc, unsigned argc, const unsigned *args)
{
    volatile qemu_call_t q;
    unsigned i;
    unsigned spill[16]; /* separate storage for concurrently issuing GCs */

    for (i = 0; i < QC_GLES_INLINE_ARGS; i++) q.gles.args[i] = 0;

    q.call_number = QC_GLES;
    q.gles.slot = slot;
    q.gles.ctx = gc && ((GuestGC *)gc)->host ? ((GuestGC *)gc)->host : (unsigned)(unsigned long)gc;
    q.gles.argc = argc;
    q.gles.spill = 0;
    q.retval = 0;
    q.error = 0;

    if (argc <= QC_GLES_INLINE_ARGS) {
        for (i = 0; i < argc; i++) q.gles.args[i] = args[i];
    } else {
        for (i = 0; i < argc; i++) spill[i] = args[i];
        q.gles.spill = (unsigned)(unsigned long)spill;
    }

    __asm__ __volatile__("mcr p15, 3, %0, c15, c15, 0" : : "r"(&q) : "memory");
    return q.retval;
}

/*
 * Reported once per slot, then silent. An app that touches an unimplemented
 * entry point does so thousands of times a second, and a log line per call
 * would both drown the log and slow the guest enough to change what it does.
 */
static unsigned char unimpl_seen[GLES_N_SLOTS];

__attribute__((visibility("hidden"))) int gles_unimpl(unsigned slot)
{
    if (slot < GLES_N_SLOTS && !unimpl_seen[slot]) {
        unimpl_seen[slot] = 1;
        w("[mbxshim] unimplemented slot "); wd(slot); w("\n");
    }
    return 0;
}

/* Host debug reads cannot page in guest memory. Touch upload pages here,
 * where a normal ARM load lets the guest VM resolve zero-fill/file mappings. */
static int guest_fault_read(unsigned long base, unsigned bytes)
{
    if (!base || !bytes || bytes > 64u * 1024 * 1024 || base > ~0UL - bytes) return 0;
    volatile const unsigned char *p = (void *)base;
    for (unsigned x = 0; x < bytes; x += 4096) (void)p[x];
    (void)p[bytes - 1];
    return 1;
}

static unsigned texture_bytes(void *gc, unsigned width, unsigned height,
                              unsigned format, unsigned type)
{
    unsigned bpp = 0;
    if (type == 0x8363 && format == 0x1907) bpp = 2; /* RGB565 */
    else if ((type == 0x8033 || type == 0x8034) && format == 0x1908) bpp = 2;
    else if (type == 0x1401) {
        switch (format) {
        case 0x1908: case 0x80e1: bpp = 4; break;
        case 0x1907: bpp = 3; break;
        case 0x190a: bpp = 2; break;
        case 0x1906: case 0x1909: bpp = 1; break;
        }
    }
    if (!bpp || !width || !height || width > (64u << 20)) return 0;
    unsigned alignment = gc ? ((GuestGC *)gc)->unpack_alignment : 0;
    if (!alignment) alignment = 4;
    unsigned row = width * bpp, stride = (row + alignment - 1) & ~(alignment - 1);
    unsigned long long total = (unsigned long long)(height - 1) * stride + row;
    return total <= (64u << 20) ? (unsigned)total : 0;
}

/* ------------------------------------------------------- implemented slots --
 *
 * Arguments are declared `unsigned` even where GL says `float`. iOS armv6 uses
 * the soft-float variant of AAPCS, so float arguments arrive in the core
 * registers as raw bit patterns -- which is exactly what the host wants, since
 * it reinterprets them itself. Declaring them float here would round-trip them
 * through a VFP register for no reason and invite an ABI mismatch.
 */

static int s_clear(void *gc, unsigned mask)
    { return (int)qc(10, gc, 1, A(mask)); }
static int s_clearColor(void *gc, unsigned r, unsigned g, unsigned b, unsigned a)
    { return (int)qc(12, gc, 4, A(r, g, b, a)); }
static int s_color4f(void *gc, unsigned r, unsigned g, unsigned b, unsigned a)
    { return (int)qc(37, gc, 4, A(r, g, b, a)); }
static int s_disable(void *gc, unsigned cap)
    { return (int)qc(63, gc, 1, A(cap)); }
static int s_disableClientState(void *gc, unsigned arr)
    { return (int)qc(64, gc, 1, A(arr)); }
static int s_drawArrays(void *gc, unsigned mode, unsigned first, unsigned count)
    { return (int)qc(65, gc, 3, A(mode, first, count)); }
static int s_enable(void *gc, unsigned cap)
    { return (int)qc(72, gc, 1, A(cap)); }
static int s_enableClientState(void *gc, unsigned arr)
    { return (int)qc(73, gc, 1, A(arr)); }
static int s_finish(void *gc)
    { return (int)qc(89, gc, 0, A(0)); }
static int s_flush(void *gc)
    { return (int)qc(90, gc, 0, A(0)); }
static int s_genTextures(void *gc, unsigned n, unsigned ids)
    { return (int)qc(98, gc, 2, A(n, ids)); }
static int s_getError(void *gc)
    { return (int)qc(102, gc, 0, A(0)); }

/*
 * glGetString is answered HERE rather than on the host, because it returns a
 * pointer to a string the guest then reads. A host-side handler would have to
 * put those bytes somewhere in guest memory and hand back an address; static
 * storage in this bundle is already guest memory, so there is nothing to
 * marshal.
 *
 * It was previously unimplemented, which meant it returned 0 -- and an app that
 * does the ordinary
 *
 *     if (strstr((char *)glGetString(GL_EXTENSIONS), "GL_OES_...")) ...
 *
 * gets NULL passed to strstr. Both Temple Run builds do exactly this and no
 * other title tested imports glGetString at all, which is why they alone hung
 * on the splash screen while everything else ran.
 *
 * NEVER return 0 from here, for any argument: an unknown enum answers with an
 * empty string, which every caller survives.
 *
 * The extension list is deliberately short and honest. Advertising something
 * the host does not implement is the failure mode this tree already knows well
 * -- a driver that is told a feature exists will use it and go wrong further
 * away. Every entry below is genuinely handled in gles-host.c: real
 * framebuffer objects, the PVRTC decoder, the paletted decoder, and GL_FIXED
 * widening. The APPLE multisample and EXT discard extensions are NOT listed,
 * and must not be until they are real.
 */
static const char *s_getString(void *gc, unsigned name)
{
    (void)gc;
    switch (name) {
    case 0x1F00: return "Imagination Technologies";   /* GL_VENDOR   */
    case 0x1F01: return "PowerVR MBX";                /* GL_RENDERER */
    case 0x1F02: return "OpenGL ES-CM 1.1";           /* GL_VERSION  */
    case 0x1F03:                                      /* GL_EXTENSIONS */
        return "GL_OES_framebuffer_object "
               "GL_OES_compressed_paletted_texture "
               "GL_OES_fixed_point "
               "GL_IMG_texture_compression_pvrtc";
    default:     return "";
    }
}
static int s_copyTexImage2D(void *gc, unsigned target, unsigned level,
                            unsigned ifmt, unsigned x, unsigned y,
                            unsigned w, unsigned h, unsigned border)
    { return (int)qc(54, gc, 8, A(target, level, ifmt, x, y, w, h, border)); }
static int s_getFloatv(void *gc, unsigned pname, unsigned params)
    { return (int)qc(103, gc, 2, A(pname, params)); }
static int s_materialf(void *gc, unsigned face, unsigned pname, unsigned param)
    { return (int)qc(170, gc, 3, A(face, pname, param)); }
static int s_readPixels(void *gc, unsigned x, unsigned y, unsigned w,
                        unsigned h, unsigned fmt, unsigned type, unsigned px)
    { return (int)qc(237, gc, 7, A(x, y, w, h, fmt, type, px)); }
static int s_texEnvf(void *gc, unsigned target, unsigned pname, unsigned param)
    { return (int)qc(290, gc, 3, A(target, pname, param)); }
static int s_texEnvfv(void *gc, unsigned target, unsigned pname, unsigned params)
    { return (int)qc(291, gc, 3, A(target, pname, params)); }
static int s_texParameterx(void *gc, unsigned target, unsigned pname,
                           unsigned param)
    { return (int)qc(799, gc, 3, A(target, pname, param)); }
static int s_loadIdentity(void *gc)
    { return (int)qc(157, gc, 0, A(0)); }
static int s_matrixMode(void *gc, unsigned m)
    { return (int)qc(174, gc, 1, A(m)); }
static int s_texCoordPointer(void *gc, unsigned size, unsigned type,
                             unsigned stride, unsigned ptr)
    { return (int)qc(289, gc, 4, A(size, type, stride, ptr)); }
static int s_texImage2D(void *gc, unsigned target, unsigned level, unsigned ifmt,
                        unsigned wd_, unsigned ht, unsigned border,
                        unsigned fmt, unsigned type, unsigned pixels)
    { guest_fault_read(pixels, texture_bytes(gc, wd_, ht, fmt, type));
      return (int)qc(301, gc, 9,
                     A(target, level, ifmt, wd_, ht, border, fmt, type, pixels)); }
static int s_getLightfv(void *gc, unsigned target, unsigned pname, unsigned params)
    { return (int)qc(105, gc, 3, A(target, pname, params)); }
static int s_getMaterialfv(void *gc, unsigned target, unsigned pname, unsigned params)
    { return (int)qc(110, gc, 3, A(target, pname, params)); }
static int s_getTexEnvfv(void *gc, unsigned target, unsigned pname, unsigned params)
    { return (int)qc(118, gc, 3, A(target, pname, params)); }
static int s_getTexEnviv(void *gc, unsigned target, unsigned pname, unsigned params)
    { return (int)qc(119, gc, 3, A(target, pname, params)); }
static int s_getTexParameterfv(void *gc, unsigned target, unsigned pname, unsigned params)
    { return (int)qc(126, gc, 3, A(target, pname, params)); }
static int s_getTexParameteriv(void *gc, unsigned target, unsigned pname, unsigned params)
    { return (int)qc(127, gc, 3, A(target, pname, params)); }
static int s_texEnviv(void *gc, unsigned target, unsigned pname, unsigned params)
    { return (int)qc(293, gc, 3, A(target, pname, params)); }
static int s_texParameterf(void *gc, unsigned target, unsigned pname, unsigned params)
    { return (int)qc(302, gc, 3, A(target, pname, params)); }
static int s_texParameterfv(void *gc, unsigned target, unsigned pname, unsigned params)
    { return (int)qc(303, gc, 3, A(target, pname, params)); }
static int s_texParameteriv(void *gc, unsigned target, unsigned pname, unsigned params)
    { return (int)qc(305, gc, 3, A(target, pname, params)); }
static int s_texParameteri(void *gc, unsigned target, unsigned pname, unsigned p)
    { return (int)qc(304, gc, 3, A(target, pname, p)); }
static int s_vertexPointer(void *gc, unsigned size, unsigned type,
                           unsigned stride, unsigned ptr)
    { return (int)qc(334, gc, 4, A(size, type, stride, ptr)); }
static int s_viewport(void *gc, unsigned x, unsigned y, unsigned wv, unsigned h)
    { return (int)qc(335, gc, 4, A(x, y, wv, h)); }
static int s_orthof(void *gc, unsigned l, unsigned r, unsigned b,
                    unsigned t, unsigned n, unsigned f)
    { return (int)qc(791, gc, 6, A(l, r, b, t, n, f)); }
static int s_clearStencil(void *gc, unsigned a0)
    { return (int)qc(15, gc, 1, A(a0)); }
static int s_color4ub(void *gc, unsigned a0, unsigned a1, unsigned a2, unsigned a3)
    { return (int)qc(43, gc, 4, A(a0, a1, a2, a3)); }
static int s_cullFace(void *gc, unsigned a0)
    { return (int)qc(57, gc, 1, A(a0)); }
static int s_normal3f(void *gc, unsigned a0, unsigned a1, unsigned a2)
    { return (int)qc(182, gc, 3, A(a0, a1, a2)); }
static int s_pointSize(void *gc, unsigned a0)
    { return (int)qc(199, gc, 1, A(a0)); }
static int s_polygonOffset(void *gc, unsigned a0, unsigned a1)
    { return (int)qc(201, gc, 2, A(a0, a1)); }
static int s_stencilFunc(void *gc, unsigned a0, unsigned a1, unsigned a2)
    { return (int)qc(254, gc, 3, A(a0, a1, a2)); }
static int s_stencilOp(void *gc, unsigned a0, unsigned a1, unsigned a2)
    { return (int)qc(256, gc, 3, A(a0, a1, a2)); }
static int s_multiTexCoord4f(void *gc, unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned a4)
    { return (int)qc(369, gc, 5, A(a0, a1, a2, a3, a4)); }
static int s_sampleCoverage(void *gc, unsigned a0, unsigned a1)
    { return (int)qc(459, gc, 2, A(a0, a1)); }
static int s_alphaFuncx(void *gc, unsigned a0, unsigned a1)
    { return (int)qc(761, gc, 2, A(a0, a1)); }
static int s_clearColorx(void *gc, unsigned a0, unsigned a1, unsigned a2, unsigned a3)
    { return (int)qc(762, gc, 4, A(a0, a1, a2, a3)); }
static int s_clearDepthx(void *gc, unsigned a0)
    { return (int)qc(764, gc, 1, A(a0)); }
static int s_color4x(void *gc, unsigned a0, unsigned a1, unsigned a2, unsigned a3)
    { return (int)qc(767, gc, 4, A(a0, a1, a2, a3)); }
static int s_depthRangef(void *gc, unsigned a0, unsigned a1)
    { return (int)qc(768, gc, 2, A(a0, a1)); }
static int s_depthRangex(void *gc, unsigned a0, unsigned a1)
    { return (int)qc(769, gc, 2, A(a0, a1)); }
static int s_frustumx(void *gc, unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned a4, unsigned a5)
    { return (int)qc(773, gc, 6, A(a0, a1, a2, a3, a4, a5)); }
static int s_lineWidthx(void *gc, unsigned a0)
    { return (int)qc(785, gc, 1, A(a0)); }
static int s_normal3x(void *gc, unsigned a0, unsigned a1, unsigned a2)
    { return (int)qc(790, gc, 3, A(a0, a1, a2)); }
static int s_orthox(void *gc, unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned a4, unsigned a5)
    { return (int)qc(792, gc, 6, A(a0, a1, a2, a3, a4, a5)); }
static int s_pointSizex(void *gc, unsigned a0)
    { return (int)qc(793, gc, 1, A(a0)); }
static int s_polygonOffsetx(void *gc, unsigned a0, unsigned a1)
    { return (int)qc(794, gc, 2, A(a0, a1)); }
static int s_rotatex(void *gc, unsigned a0, unsigned a1, unsigned a2, unsigned a3)
    { return (int)qc(795, gc, 4, A(a0, a1, a2, a3)); }
static int s_translatex(void *gc, unsigned a0, unsigned a1, unsigned a2)
    { return (int)qc(801, gc, 3, A(a0, a1, a2)); }
static int s_multiTexCoord4x(void *gc, unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned a4)
    { return (int)qc(802, gc, 5, A(a0, a1, a2, a3, a4)); }
static int s_pointSizePointerOES(void *gc, unsigned type, unsigned stride, unsigned ptr)
    { return (int)qc(806, gc, 3, A(type, stride, ptr)); }
static int s_sampleCoveragex(void *gc, unsigned a0, unsigned a1)
    { return (int)qc(803, gc, 2, A(a0, a1)); }
static int s_getBooleanv(void *gc, unsigned a0, unsigned a1)
    { return (int)qc(99, gc, 2, A(a0, a1)); }
static int s_getPointerv(void *gc, unsigned a0, unsigned a1)
    { return (int)qc(115, gc, 2, A(a0, a1)); }
static int s_loadMatrixx(void *gc, unsigned a0)
    { if (!guest_fault_read(a0, 64)) return -1; return (int)qc(786, gc, 1, A(a0)); }
static int s_multMatrixx(void *gc, unsigned a0)
    { if (!guest_fault_read(a0, 64)) return -1; return (int)qc(789, gc, 1, A(a0)); }
static int s_colorMask(void *gc, unsigned r, unsigned g, unsigned b, unsigned a)
    { return (int)qc(49, gc, 4, A(r, g, b, a)); }
static int s_stencilMask(void *gc, unsigned mask)
    { return (int)qc(255, gc, 1, A(mask)); }
static int s_isRenderbuffer(void *gc, unsigned name)
    { return (int)qc(665, gc, 1, A(name)); }
static int s_isFramebuffer(void *gc, unsigned name)
    { return (int)qc(671, gc, 1, A(name)); }
static int s_generateMipmap(void *gc, unsigned target)
    { return (int)qc(681, gc, 1, A(target)); }
static int s_bindTexture(void *gc, unsigned target, unsigned tex)
    { return (int)qc(5, gc, 2, A(target, tex)); }

/* ---- the fixed-function set a real ES 1.1 game needs ----------------------
 *
 * These are exactly Cube Runner's imports: `nm -u` on the binary lists 41 gl*
 * symbols, the 21 above cover part of it and these 20 cover the rest. Slot
 * numbers come from the framework's own trampolines (see gles.h), not from a
 * table of ours.
 */
static int s_blendFunc(void *gc, unsigned s, unsigned d)
    { return (int)qc(7, gc, 2, A(s, d)); }
static int s_colorPointer(void *gc, unsigned size, unsigned type,
                          unsigned stride, unsigned ptr)
    { return (int)qc(51, gc, 4, A(size, type, stride, ptr)); }
static int s_depthMask(void *gc, unsigned flag)
    { return (int)qc(61, gc, 1, A(flag)); }
static int s_drawElements(void *gc, unsigned mode, unsigned count,
                          unsigned type, unsigned indices)
    { return (int)qc(67, gc, 4, A(mode, count, type, indices)); }
static int s_fogf(void *gc, unsigned pname, unsigned param)
    { return (int)qc(91, gc, 2, A(pname, param)); }
static int s_fogfv(void *gc, unsigned pname, unsigned params)
    { return (int)qc(92, gc, 2, A(pname, params)); }
static int s_hint(void *gc, unsigned target, unsigned mode)
    { return (int)qc(128, gc, 2, A(target, mode)); }
static int s_lightfv(void *gc, unsigned light, unsigned pname, unsigned params)
    { return (int)qc(151, gc, 3, A(light, pname, params)); }
static int s_lineWidth(void *gc, unsigned width)
    { return (int)qc(155, gc, 1, A(width)); }
static int s_materialfv(void *gc, unsigned face, unsigned pname, unsigned params)
    { return (int)qc(171, gc, 3, A(face, pname, params)); }
static int s_multMatrixf(void *gc, unsigned m)
    { return (int)qc(176, gc, 1, A(m)); }
static int s_normalPointer(void *gc, unsigned type, unsigned stride,
                           unsigned ptr)
    { return (int)qc(188, gc, 3, A(type, stride, ptr)); }
static int s_popMatrix(void *gc)
    { return (int)qc(205, gc, 0, A(0)); }
static int s_pushMatrix(void *gc)
    { return (int)qc(210, gc, 0, A(0)); }
static int s_rotatef(void *gc, unsigned an, unsigned x, unsigned y, unsigned z)
    { return (int)qc(248, gc, 4, A(an, x, y, z)); }
static int s_scalef(void *gc, unsigned x, unsigned y, unsigned z)
    { return (int)qc(250, gc, 3, A(x, y, z)); }
static int s_shadeModel(void *gc, unsigned mode)
    { return (int)qc(253, gc, 1, A(mode)); }
static int s_translatef(void *gc, unsigned x, unsigned y, unsigned z)
    { return (int)qc(309, gc, 3, A(x, y, z)); }
static int s_clearDepthf(void *gc, unsigned d)
    { return (int)qc(763, gc, 1, A(d)); }
static int s_frustumf(void *gc, unsigned l, unsigned r, unsigned b,
                      unsigned t, unsigned n, unsigned f)
    { return (int)qc(772, gc, 6, A(l, r, b, t, n, f)); }

/* The rest of Super Monkey Ball's import set -- its `nm -u` lists 49 gl*
 * symbols and these seven were the only unimplemented ones. Slot numbers read
 * out of the 3.1.3 SDK trampolines like all the others (see gles.h). */
static int s_alphaFunc(void *gc, unsigned func, unsigned ref)
    { return (int)qc(1, gc, 2, A(func, ref)); }
static int s_deleteTextures(void *gc, unsigned n, unsigned ids)
    { return (int)qc(59, gc, 2, A(n, ids)); }
static int s_getIntegerv(void *gc, unsigned pname, unsigned out)
    { return (int)qc(104, gc, 2, A(pname, out)); }
static int s_loadMatrixf(void *gc, unsigned m)
    { return (int)qc(159, gc, 1, A(m)); }
static int s_texSubImage2D(void *gc, unsigned target, unsigned level,
                           unsigned xoff, unsigned yoff, unsigned wd_,
                           unsigned ht, unsigned fmt, unsigned type,
                           unsigned pixels)
    { guest_fault_read(pixels, texture_bytes(gc, wd_, ht, fmt, type));
      return (int)qc(307, gc, 9,
                     A(target, level, xoff, yoff, wd_, ht, fmt, type, pixels)); }
static int s_bindBuffer(void *gc, unsigned target, unsigned buf)
    { return (int)qc(642, gc, 2, A(target, buf)); }
static int s_scalex(void *gc, unsigned x, unsigned y, unsigned z)
    { return (int)qc(796, gc, 3, A(x, y, z)); }

/* Cheap state setters that a survey of 20 shipping App Store apps found
 * imported but unimplemented. Slots read out of the 3.1.3 SDK trampolines and
 * cross-checked against slotmap.txt; see gles.h. */
static int s_pixelStorei(void *gc, unsigned pname, unsigned param)
    {
        if (gc && pname == 0x0cf5 && (param == 1 || param == 2 || param == 4 || param == 8))
            ((GuestGC *)gc)->unpack_alignment = param;
        return (int)qc(195, gc, 2, A(pname, param));
    }
static int s_scissor(void *gc, unsigned x, unsigned y, unsigned wd_, unsigned ht)
    { return (int)qc(251, gc, 4, A(x, y, wd_, ht)); }
static int s_texEnvi(void *gc, unsigned target, unsigned pname, unsigned param)
    { return (int)qc(292, gc, 3, A(target, pname, param)); }
static int s_activeTexture(void *gc, unsigned tex)
    { return (int)qc(342, gc, 1, A(tex)); }
static int s_clientActiveTexture(void *gc, unsigned tex)
    { return (int)qc(341, gc, 1, A(tex)); }
static int s_depthFunc(void *gc, unsigned func)
    { return (int)qc(60, gc, 1, A(func)); }
static int s_frontFace(void *gc, unsigned mode)
    { return (int)qc(95, gc, 1, A(mode)); }

/* OES framebuffer objects. EAGL calls these itself inside
 * -renderbufferStorage:fromDrawable:, so a CAEAGLLayer client needs them
 * before it can draw anything at all. */
static int s_genRenderbuffers(void *gc, unsigned n, unsigned ids)
    { return (int)qc(668, gc, 2, A(n, ids)); }
static int s_bindRenderbuffer(void *gc, unsigned target, unsigned rb)
    { return (int)qc(666, gc, 2, A(target, rb)); }
static int s_deleteRenderbuffers(void *gc, unsigned n, unsigned ids)
    { return (int)qc(667, gc, 2, A(n, ids)); }
static int s_renderbufferStorage(void *gc, unsigned t, unsigned f,
                                 unsigned wv, unsigned h)
    { return (int)qc(669, gc, 4, A(t, f, wv, h)); }
static int s_getRenderbufferParameteriv(void *gc, unsigned t, unsigned p,
                                        unsigned out)
    { return (int)qc(670, gc, 3, A(t, p, out)); }
static int s_genFramebuffers(void *gc, unsigned n, unsigned ids)
    { return (int)qc(674, gc, 2, A(n, ids)); }
static int s_bindFramebuffer(void *gc, unsigned target, unsigned fb)
    { return (int)qc(672, gc, 2, A(target, fb)); }
static int s_deleteFramebuffers(void *gc, unsigned n, unsigned ids)
    { return (int)qc(673, gc, 2, A(n, ids)); }
static int s_checkFramebufferStatus(void *gc, unsigned target)
    { return (int)qc(675, gc, 1, A(target)); }
static int s_framebufferRenderbuffer(void *gc, unsigned t, unsigned at,
                                     unsigned rbt, unsigned rb)
    { return (int)qc(679, gc, 4, A(t, at, rbt, rb)); }
static int s_framebufferTexture2D(void *gc, unsigned t, unsigned at,
                                  unsigned tt, unsigned tex, unsigned lvl)
    { return (int)qc(677, gc, 5, A(t, at, tt, tex, lvl)); }
static int s_getFramebufferAttachmentParameteriv(void *gc, unsigned t,
                                                 unsigned at, unsigned p,
                                                 unsigned out)
    { return (int)qc(680, gc, 4, A(t, at, p, out)); }

/* Compressed textures and buffer objects. glCompressedTexImage2D carries eight
 * scalars and glCompressedTexSubImage2D nine, so both spill exactly the way
 * s_texImage2D does. Slots read out of the 3.1.3 SDK trampolines; see gles.h. */
static int s_compressedTexImage2D(void *gc, unsigned target, unsigned level,
                                  unsigned ifmt, unsigned wd_, unsigned ht,
                                  unsigned border, unsigned imgsz,
                                  unsigned data)
    { guest_fault_read(data, imgsz);
      return (int)qc(380, gc, 8,
                     A(target, level, ifmt, wd_, ht, border, imgsz, data)); }
static int s_compressedTexSubImage2D(void *gc, unsigned target, unsigned level,
                                     unsigned xoff, unsigned yoff, unsigned wd_,
                                     unsigned ht, unsigned fmt, unsigned imgsz,
                                     unsigned data)
    { return (int)qc(383, gc, 9,
                     A(target, level, xoff, yoff, wd_, ht, fmt, imgsz, data)); }
static int s_genBuffers(void *gc, unsigned n, unsigned ids)
    { return (int)qc(644, gc, 2, A(n, ids)); }
static int s_deleteBuffers(void *gc, unsigned n, unsigned ids)
    { return (int)qc(643, gc, 2, A(n, ids)); }
static int s_bufferData(void *gc, unsigned target, unsigned size,
                        unsigned data, unsigned usage)
    { guest_fault_read(data, size);
      return (int)qc(646, gc, 4, A(target, size, data, usage)); }
static int s_bufferSubData(void *gc, unsigned target, unsigned offset,
                           unsigned size, unsigned data)
    { guest_fault_read(data, size);
      return (int)qc(647, gc, 4, A(target, offset, size, data)); }

/* ------------------------------------------------------------ EGL interface */

/* One GC per context. The framework only ever hands this back to us as arg0,
 * so its contents are ours; the host keys off the same pointer. */


/* Stock 7E18 engine 0xa0d4 stores through argument 0 and returns 1. */
static int GLESCreateSharegroup(void **out)
{
    if (!out) return 0;
    *out = 0;
    GuestGC *group = calloc(1, sizeof(*group));
    if (!group) return 0;
    long long host = qc(GLES_OP_NEW_SHAREGROUP, 0, 0, A(0));
    if (host < 0) { free(group); return 0; }
    group->host = (unsigned)host;
    *out = group;
    return 1;
}

static int GLESDestroySharegroup(void *sg)
{
    if (sg && ((GuestGC *)sg)->host) qc(GLES_OP_DELETE_SHAREGROUP, 0, 1, A(((GuestGC *)sg)->host));
    free(sg);
    return 0;
}

/*
 * The one that matters. See the header comment for how the contract was read
 * out of the real binary.
 */
static int GLESCreateGC(void *sharegroup, void **table, void *x_ce8,
                        void **gc_out)
{
    unsigned i;

    (void)x_ce8;
    if (!gc_out || !sharegroup) return 0;
    GuestGC *gc = calloc(1, sizeof(*gc));
    if (!gc) return 0;
    if (((GuestGC *)sharegroup)->host) {
        long long host = qc(GLES_OP_NEW_CONTEXT, 0, 1, A(((GuestGC *)sharegroup)->host));
        if (host <= 0) { free(gc); return 0; }
        gc->host = (unsigned)host;
    }
    w("[mbxshim] GLESCreateGC\n");

    if (table) {
        /* Fill every entry. The trampolines never null-check. */
        for (i = 0; i < GLES_N_SLOTS; i++) {
            table[i] = gles_default_table[i];
        }
        table[15] = (void *)s_clearStencil;
        table[43] = (void *)s_color4ub;
        table[57] = (void *)s_cullFace;
        table[182] = (void *)s_normal3f;
        table[199] = (void *)s_pointSize;
        table[201] = (void *)s_polygonOffset;
        table[254] = (void *)s_stencilFunc;
        table[256] = (void *)s_stencilOp;
        table[369] = (void *)s_multiTexCoord4f;
        table[459] = (void *)s_sampleCoverage;
        table[761] = (void *)s_alphaFuncx;
        table[762] = (void *)s_clearColorx;
        table[764] = (void *)s_clearDepthx;
        table[767] = (void *)s_color4x;
        table[768] = (void *)s_depthRangef;
        table[769] = (void *)s_depthRangex;
        table[773] = (void *)s_frustumx;
        table[785] = (void *)s_lineWidthx;
        table[790] = (void *)s_normal3x;
        table[792] = (void *)s_orthox;
        table[793] = (void *)s_pointSizex;
        table[794] = (void *)s_polygonOffsetx;
        table[795] = (void *)s_rotatex;
        table[801] = (void *)s_translatex;
        table[802] = (void *)s_multiTexCoord4x;
        table[803] = (void *)s_sampleCoveragex;
        table[806] = (void *)s_pointSizePointerOES;
        table[99] = (void *)s_getBooleanv;
        table[115] = (void *)s_getPointerv;
        table[786] = (void *)s_loadMatrixx;
        table[789] = (void *)s_multMatrixx;
        table[49]  = (void *)s_colorMask;
        table[255] = (void *)s_stencilMask;
        table[665] = (void *)s_isRenderbuffer;
        table[671] = (void *)s_isFramebuffer;
        table[681] = (void *)s_generateMipmap;
        table[5]   = (void *)s_bindTexture;
        table[10]  = (void *)s_clear;
        table[12]  = (void *)s_clearColor;
        table[37]  = (void *)s_color4f;
        table[63]  = (void *)s_disable;
        table[64]  = (void *)s_disableClientState;
        table[65]  = (void *)s_drawArrays;
        table[72]  = (void *)s_enable;
        table[73]  = (void *)s_enableClientState;
        table[89]  = (void *)s_finish;
        table[90]  = (void *)s_flush;
        table[98]  = (void *)s_genTextures;
        table[102] = (void *)s_getError;
        table[117] = (void *)s_getString;
        /* Forwarded rather than dropped. A call the shim does not forward is
         * invisible to the host's unhandled-slot warning too, so these were
         * silent on both sides -- glCopyTexImage2D among them, which is what
         * left Labyrinth's render target with no storage. */
        table[54]  = (void *)s_copyTexImage2D;
        table[103] = (void *)s_getFloatv;
        table[170] = (void *)s_materialf;
        table[237] = (void *)s_readPixels;
        table[290] = (void *)s_texEnvf;
        table[291] = (void *)s_texEnvfv;
        table[799] = (void *)s_texParameterx;
        table[157] = (void *)s_loadIdentity;
        table[174] = (void *)s_matrixMode;
        table[289] = (void *)s_texCoordPointer;
        table[301] = (void *)s_texImage2D;
        table[105] = (void *)s_getLightfv;
        table[110] = (void *)s_getMaterialfv;
        table[118] = (void *)s_getTexEnvfv;
        table[119] = (void *)s_getTexEnviv;
        table[126] = (void *)s_getTexParameterfv;
        table[127] = (void *)s_getTexParameteriv;
        table[293] = (void *)s_texEnviv;
        table[302] = (void *)s_texParameterf;
        table[303] = (void *)s_texParameterfv;
        table[305] = (void *)s_texParameteriv;
        table[304] = (void *)s_texParameteri;
        table[334] = (void *)s_vertexPointer;
        table[335] = (void *)s_viewport;
        table[791] = (void *)s_orthof;
        table[666] = (void *)s_bindRenderbuffer;
        table[667] = (void *)s_deleteRenderbuffers;
        table[668] = (void *)s_genRenderbuffers;
        table[669] = (void *)s_renderbufferStorage;
        table[670] = (void *)s_getRenderbufferParameteriv;
        table[672] = (void *)s_bindFramebuffer;
        table[673] = (void *)s_deleteFramebuffers;
        table[674] = (void *)s_genFramebuffers;
        table[675] = (void *)s_checkFramebufferStatus;
        table[677] = (void *)s_framebufferTexture2D;
        table[679] = (void *)s_framebufferRenderbuffer;
        table[680] = (void *)s_getFramebufferAttachmentParameteriv;

        table[7]   = (void *)s_blendFunc;
        table[51]  = (void *)s_colorPointer;
        table[61]  = (void *)s_depthMask;
        table[67]  = (void *)s_drawElements;
        table[91]  = (void *)s_fogf;
        table[92]  = (void *)s_fogfv;
        table[128] = (void *)s_hint;
        table[151] = (void *)s_lightfv;
        table[155] = (void *)s_lineWidth;
        table[171] = (void *)s_materialfv;
        table[176] = (void *)s_multMatrixf;
        table[188] = (void *)s_normalPointer;
        table[205] = (void *)s_popMatrix;
        table[210] = (void *)s_pushMatrix;
        table[248] = (void *)s_rotatef;
        table[250] = (void *)s_scalef;
        table[253] = (void *)s_shadeModel;
        table[309] = (void *)s_translatef;
        table[763] = (void *)s_clearDepthf;
        table[772] = (void *)s_frustumf;

        table[1]   = (void *)s_alphaFunc;
        table[59]  = (void *)s_deleteTextures;
        table[104] = (void *)s_getIntegerv;
        table[159] = (void *)s_loadMatrixf;
        table[307] = (void *)s_texSubImage2D;
        table[642] = (void *)s_bindBuffer;
        table[796] = (void *)s_scalex;

        table[380] = (void *)s_compressedTexImage2D;
        table[383] = (void *)s_compressedTexSubImage2D;
        table[643] = (void *)s_deleteBuffers;
        table[644] = (void *)s_genBuffers;
        table[646] = (void *)s_bufferData;
        table[647] = (void *)s_bufferSubData;

        table[60]  = (void *)s_depthFunc;
        table[95]  = (void *)s_frontFace;
        table[195] = (void *)s_pixelStorei;
        table[251] = (void *)s_scissor;
        table[292] = (void *)s_texEnvi;
        table[341] = (void *)s_clientActiveTexture;
        table[342] = (void *)s_activeTexture;
    }

    if (gc_out) {
        *gc_out = gc;
    }
    return 1;   /* 1 = success. See the header comment; 0 here yields a nil
                 * EAGLContext with no other symptom. */
}

static int GLESDestroyGC(void *gc);

/*
 * The surface CoreAnimation gave us, if any.
 *
 * The stock engine imports ten IOSurface functions and every one is read-side
 * (IOSurfaceGetBaseAddress/BytesPerRow/Width/Height/PixelFormat/AllocSize/ID/
 * PlaneCount, IOSurfaceLock, IOSurfaceUnlock). There is no IOSurfaceCreate: CA
 * owns the allocation and the engine only renders into it. So all we keep is
 * what the host needs to write pixels.
 */
/*
 * ONE OF THESE PER GC, not one for the whole process.
 *
 * Every field below used to be a single global, and an app with more than one
 * CAEAGLLayer therefore had all of its views sharing one drawable, one CA
 * block and one surface. Cube Runner has three GCs and binds two views, so its
 * game layer and its menu layer wrote into the same surface and whichever
 * presented last won -- which is why the panel showed a correct camera with no
 * obstacles.
 *
 * The block in particular CANNOT be shared: CA indexes it, keys its own
 * bookkeeping off it, and hands it back as createBuffer's arg0, so it is how we
 * tell one view's surfaces from another's. Sharing it is also the likeliest
 * reason CA refused the second bind outright -- the real engine allocates this
 * per GC at ctx+0x210.
 */
typedef struct {
    void *gc;                 /* identity; 0 means the slot is free */
    void *drawable;
    void *block[8];           /* CA indexes this -- must be per view */
    void *ref;
    unsigned base, stride, width, height, format;
    unsigned char need_buffer;
} ca_view_t;

#define CA_MAX_VIEWS 4
static ca_view_t ca_views[CA_MAX_VIEWS];

/* Look a view up by the GC that owns it, optionally allocating a slot. */
static ca_view_t *ca_view_for_gc(void *gc, int create)
{
    int i, free_slot = -1;

    for (i = 0; i < CA_MAX_VIEWS; i++) {
        if (ca_views[i].gc == gc) return &ca_views[i];
        if (!ca_views[i].gc && free_slot < 0) free_slot = i;
    }
    if (!create || free_slot < 0) return 0;
    ca_views[free_slot].gc = gc;
    return &ca_views[free_slot];
}

/* CA hands the block back as createBuffer's arg0; that is the view's identity
 * on the callback side. */
static ca_view_t *ca_view_for_block(void *blk)
{
    int i;

    for (i = 0; i < CA_MAX_VIEWS; i++) {
        if (ca_views[i].gc && (void *)ca_views[i].block == blk) {
            return &ca_views[i];
        }
    }
    return 0;
}

static void ca_detach_view(ca_view_t *v);

static int GLESDestroyGC(void *gc)
{
    ca_detach_view(ca_view_for_gc(gc, 0));
    if (gc && ((GuestGC *)gc)->host) qc(GLES_OP_DELETE_CONTEXT, gc, 0, A(0));
    free(gc);
    return 0;
}

static void *iosurf;    /* IOSurface.framework handle */
static void *(*p_IOSurfaceGetBaseAddress)(void *);
static unsigned (*p_IOSurfaceGetBytesPerRow)(void *);
static unsigned (*p_IOSurfaceGetWidth)(void *);
static unsigned (*p_IOSurfaceGetHeight)(void *);
static unsigned (*p_IOSurfaceGetPixelFormat)(void *);
static unsigned (*p_IOSurfaceGetPlaneCount)(void *);
static void *(*p_IOSurfaceGetBaseAddressOfPlane)(void *, unsigned);
static unsigned (*p_IOSurfaceGetBytesPerRowOfPlane)(void *, unsigned);
static int (*p_IOSurfaceLock)(void *, unsigned, unsigned *);
static int (*p_IOSurfaceUnlock)(void *, unsigned, unsigned *);
static unsigned long (*p_IOSurfaceGetTypeID)(void);
static unsigned long (*p_CFGetTypeID)(const void *);

extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
#define RTLD_NOW 2

/* Heap bounds checks, so the search below can never dereference a word that
 * merely looks like a pointer. malloc_zone_from_ptr answers "is this address
 * owned by a malloc zone" from the zone's own address ranges, without reading
 * anything at the address itself. */
extern void *malloc_zone_from_ptr(const void *);
extern unsigned long malloc_size(const void *);

static void iosurface_init(void)
{
    if (iosurf) return;
    iosurf = dlopen("/System/Library/PrivateFrameworks/IOSurface.framework/IOSurface",
                    RTLD_NOW);
    if (!iosurf) { w("[mbxshim] IOSurface.framework not available\n"); return; }
    p_IOSurfaceGetBaseAddress = dlsym(iosurf, "IOSurfaceGetBaseAddress");
    p_IOSurfaceGetBytesPerRow = dlsym(iosurf, "IOSurfaceGetBytesPerRow");
    p_IOSurfaceGetWidth       = dlsym(iosurf, "IOSurfaceGetWidth");
    p_IOSurfaceGetHeight      = dlsym(iosurf, "IOSurfaceGetHeight");
    p_IOSurfaceGetPixelFormat = dlsym(iosurf, "IOSurfaceGetPixelFormat");
    p_IOSurfaceGetPlaneCount = dlsym(iosurf, "IOSurfaceGetPlaneCount");
    p_IOSurfaceGetBaseAddressOfPlane = dlsym(iosurf, "IOSurfaceGetBaseAddressOfPlane");
    p_IOSurfaceGetBytesPerRowOfPlane = dlsym(iosurf, "IOSurfaceGetBytesPerRowOfPlane");
    p_IOSurfaceLock           = dlsym(iosurf, "IOSurfaceLock");
    p_IOSurfaceUnlock         = dlsym(iosurf, "IOSurfaceUnlock");
    p_IOSurfaceGetTypeID      = dlsym(iosurf, "IOSurfaceGetTypeID");
    {
        void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/"
                          "CoreFoundation", RTLD_NOW);
        if (cf) p_CFGetTypeID = dlsym(cf, "CFGetTypeID");
    }
}

/*
 * Try to read a surface's geometry, and REJECT it unless the numbers are
 * self-consistent.
 *
 * The validation is not defensive padding, it is load-bearing. Measured on
 * 3.1.3: the object GLESBindView is handed is NOT an IOSurfaceRef. Calling
 * IOSurfaceGetBaseAddress on it does not fail -- it happily reads whatever is
 * at that offset of some other object and returns garbage (observed:
 * base=3468311840, stride=3885969411, 3852415272x3851223040). Nothing about
 * that is distinguishable from a real surface at the call site.
 *
 * Accepting it would point the host's present at an arbitrary guest address
 * with a nonsense stride. So anything that fails a plausibility check is
 * dropped, the view's surface stays empty, and GLESPresentView falls back to the panel
 * blit rather than scribbling somewhere random.
 */
static int surface_capture(ca_view_t *v, void *s)
{
    unsigned base, stride, width, height, format, bpp;

    iosurface_init();
    if (!v || !s || !p_IOSurfaceGetBaseAddress) return 0;

    base   = (unsigned)(unsigned long)p_IOSurfaceGetBaseAddress(s);
    stride = p_IOSurfaceGetBytesPerRow ? p_IOSurfaceGetBytesPerRow(s) : 0;
    width  = p_IOSurfaceGetWidth  ? p_IOSurfaceGetWidth(s)  : 0;
    height = p_IOSurfaceGetHeight ? p_IOSurfaceGetHeight(s) : 0;
    format = p_IOSurfaceGetPixelFormat ? p_IOSurfaceGetPixelFormat(s) : 0;

    w("[mbxshim]   candidate surface base="); wd(base);
    w(" stride="); wd(stride);
    w(" "); wd(width); w("x"); wd(height);
    w(" fmt="); wd(format); w("\n");

    /* A real drawable on this device is at most a screenful and its stride has
     * to cover its width -- at ITS OWN pixel size. A CAEAGLLayer asking for
     * kEAGLColorFormatRGB565 gets a 2-byte surface (320 wide, stride 640), and
     * measuring that against a hardcoded 4 bytes rejected it as garbage: the
     * app then rendered every frame into a framebuffer with no colour
     * attachment, which looks like a healthy draw count and a white screen. */
    bpp = (format == CA_FOURCC_565L || format == CA_FOURCC_555L) ? 2 : 4;
    if (!base || width == 0 || height == 0 ||
        width > 2048 || height > 2048 || stride < width * bpp ||
        stride > width * bpp + 4096) {
        w("[mbxshim]   -> REJECTED (not a plausible IOSurface); "
          "keeping panel fallback\n");
        return 0;
    }

    v->ref    = s;
    v->base   = base;
    v->stride = stride;
    v->width  = width;
    v->height = height;
    v->format = format;
    w("[mbxshim]   -> accepted\n");
    return 1;
}

/* ------------------------------------------ the CoreAnimation drawable ----
 *
 * THE DRAWABLE IS A CALLBACK TABLE, NOT A BUFFER.
 *
 * This was read out of the stock engine (device copy, md5 92ddd55f, so the
 * addresses below are literal file addresses in it) and it settles what two
 * rounds of guessing could not. GLESBindView never dereferences its drawable as
 * data; it tail-calls through it:
 *
 *   0xd7ec  str  r5, [r4, #0x20c]     ; remember the drawable in the engine ctx
 *   0xd7f4  str  r3, [r4, #0x214]     ; r3 = &_GLESCreateBuffer   (0xda88)
 *   0xd804  str  r4, [r4, #0x210]     ; the engine's own context
 *   0xd808  str  r3, [r4, #0x218]     ; r3 = &_GLESDestroyBuffer  (0xd8dc)
 *   0xd80c  str  r8, [r4, #0x224]     ; the GC
 *   0xd814  mov  r1, r10              ; a pixel-format FourCC
 *   0xd818  add  r2, r4, #528         ; = ctx+0x210, i.e. the block just built
 *   0xd81c  ldr  pc, [r5, #0x4]       ; drawable->bind(drawable, fourcc, block)
 *
 * So the engine hands CoreAnimation a {context, functions} closure and CA calls
 * back into it. `_GLESCreateBuffer(ctx, surface)` at 0xda88 takes the
 * IOSurfaceRef as its SECOND ARGUMENT -- its first instructions are
 * IOSurfaceLock(r1) and _ValidateCoreSurface(r1). That is where the surface
 * comes from. It was never anywhere inside the drawable, which is why looking
 * for it there found nothing.
 *
 * THE FORMAT IS 'BGRA', and now for a reason rather than by assumption. r10
 * above is a FourCC selected from the internalformat argument:
 *
 *   GL_RGB565_OES (0x8d62)                        -> 'L565' (0x4c353635)
 *   GL_RGB8/RGBA4/RGB5_A1/RGBA8 (0x8051..0x8058)  -> 'BGRA' (0x42475241)
 *   0 (unspecified)                               -> 'BGRA'
 *   anything else                                 -> GL_INVALID_ENUM, no bind
 *
 * It is the format the engine ASKS CA for, so for an RGBA8 renderbuffer -- what
 * every CAEAGLLayer client gets by default -- the surface really is BGRA.
 *
 * THE REST OF THE TABLE, and the one that actually delivers a frame:
 *
 *   +0x08 unbind(drawable)          -- releases a binding on teardown/rebind,
 *                                      or if acquiring its first buffer fails.
 *                                      GLESBindView calls _DetachTexture first
 *                                      (7E18 armv6 0xd7dc); _DetachTexture
 *                                      calls unbind at 0x1bee4.
 *   +0x0c nextBuffer(drawable)      -- returns the IOSurfaceRef to render into
 *                                      for THIS frame.
 *   +0x10 present(drawable, 1)      -- the frame in that surface is finished.
 *
 * nextBuffer is the whole answer to "where does the address come from".
 * _ViewTextureBeginIfNeeded (0xd5ec) is the only caller:
 *
 *   0xd5f4  ldr  r3, [r1, #0x21c]   ; nothing to do unless a buffer is wanted
 *   0xd618  ldr  pc, [r3, #0xc]     ; surface = drawable->nextBuffer(drawable)
 *   0xd628  ldr  r2, [r4, #0x230]   ; then look it up in the buffer list that
 *   0xd634  ldr  r3, [r2, #0x18]    ;   _GLESCreateBuffer built
 *   0xd678  str  r0, [r4, #0x220]   ; and arm the present
 *
 * and GLESPresentView re-arms ctx+0x21c on the way out (0xd70c), so it is
 * exactly ONE nextBuffer per frame. That is also the answer to whether CA can
 * move the surface between frames: it does not merely have the right to, the
 * design hands you a different one each frame -- this is double buffering, and
 * caching the address from bind time would render into the buffer being
 * scanned out.
 *
 * bind, present and nextBuffer all report failure as zero (0xd820
 * `subs r5,r0,#0` then `beq` to SetError; 0xd624 likewise), and so does
 * _GLESCreateBuffer (0xdc50 `mov r0,#1` on the success path only).
 */

#define GL_RGB565_OES  0x8d62

typedef int (*ca_bind_fn)(void *drawable, unsigned fourcc, void **block);
typedef int (*ca_unbind_fn)(void *drawable);
typedef void *(*ca_next_fn)(void *drawable);
typedef int (*ca_present_fn)(void *drawable, unsigned n);


/* These fire every frame; one log line per process is enough to know the path
 * is live, and a line per frame would change the timing it is reporting on. */
static unsigned char present_logged;
static unsigned present_ok, present_fail;
static unsigned char surface_logged;
/* Mirrors the engine's ctx+0x21c: "a buffer is wanted for the next frame". */


static int ca_next_buffer(ca_view_t *v);

/*
 * The block CA is given. Its shape is the engine's ctx+0x210 verbatim, because
 * CA indexes it and we do not get to choose the layout: [0] is the context
 * passed back to us as arg0, [1] create, [2] destroy, [5] the GC.
 */


static int ca_create_buffer(void *ctx, void *surface)
{
    /* ctx is the block we handed CA at bind time, which names the view. */
    ca_view_t *v = ca_view_for_block(ctx);

    w("[mbxshim] CA createBuffer surface="); wx((unsigned long)surface); w("\n");
    /* Still validated: this is the frame's destination address, and a wrong one
     * is a write to an arbitrary guest page. */
    return surface_capture(v, surface) ? 1 : 0;
}

/*
 * Ask CA for this frame's surface. Mirrors _ViewTextureBeginIfNeeded: at most
 * one call per frame, gated on the same flag the engine keeps at ctx+0x21c.
 */
static int ca_next_buffer(ca_view_t *v)
{
    void **vt;
    void *s;

    if (!v || !v->drawable) return 0;
    vt = v->drawable;
    if (!v->need_buffer) return v->ref != 0;

    s = ((ca_next_fn)vt[3])(v->drawable);
    if (!s) {
        w("[mbxshim] drawable->nextBuffer returned nothing\n");
        return 0;
    }
    v->need_buffer = 0;
    if (s == v->ref) {
        return v->base != 0;
    }
    /* A different surface from last frame is the normal case, not an error:
     * CA rotates buffers. Re-read its geometry rather than assuming it matches
     * the one before. */
    if (!surface_logged) {
        surface_logged = 1;
        w("[mbxshim] drawable->nextBuffer -> "); wx((unsigned long)s); w("\n");
    }
    return surface_capture(v, s);
}

static int ca_destroy_buffer(void *ctx, void *surface)
{
    ca_view_t *v = ca_view_for_block(ctx);

    w("[mbxshim] CA destroyBuffer surface="); wx((unsigned long)surface); w("\n");
    /* Only the view that owns it, so one layer's teardown cannot blind
     * another -- the same rule as the bind failure path. */
    if (v && v->ref == surface) {
        /* Whatever replaces it will arrive through createBuffer. Presenting into
         * a destroyed surface writes into freed memory. */
        v->ref = 0;
        v->base = 0;
    }
    return 1;
}

/* Match 7E18 _DetachTexture: return an acquired buffer before unbinding.
 * CA refuses a second bind while the layer still belongs to the first one.
 * Keep the callback block registered until unbind finishes: CA can synchronously
 * destroy its surfaces through that block during teardown. */
static void ca_detach_view(ca_view_t *v)
{
    ca_view_t empty = {0};
    if (!v) return;
    if (v->drawable) {
        void **vt = v->drawable;
        if (!v->need_buffer && v->ref && vt[4]) {
            ((ca_present_fn)vt[4])(v->drawable, 1);
        }
        if (vt[2]) ((ca_unbind_fn)vt[2])(v->drawable);
    }
    *v = empty;
}

/* A mapped IOSurface can still contain demand-paged memory. The host's debug
 * memory reader cannot fault guest pages in as the real GPU's pinning does. */
static int surface_fault_read(unsigned long base, unsigned stride, unsigned rows,
                               unsigned bytes)
{
    unsigned row;
    if (!base || !rows || rows > 2048 || !bytes || stride < bytes || stride > 16384 ||
        base > ~0UL - ((unsigned long)(rows - 1) * stride + bytes))
        return 0;
    for (row = 0; row < rows; row++) {
        guest_fault_read(base + row * stride, bytes);
    }
    return 1;
}

/* 7E18's engine at 0xd918 takes (gc, GL target, IOSurface), not
 * (gc, IOSurface, ...). The target is 0x84f5 or 0x8d41; treating it as a
 * surface pointer crashes the compositor. Texture bindings never own a view. */
static int GLESBindCoreSurface(void *gc, unsigned target, void *surface)
{
    unsigned base, stride, width, height, format, uv = 0, uvstride = 0;
    int result;
    if (!surface) {
        return qc(GLES_OP_BIND_SURFACE, gc, 8, A(target,0,0,0,0,0,0,0)) == 0;
    }
    iosurface_init();
    if (!p_IOSurfaceLock || !p_IOSurfaceUnlock ||
        !p_IOSurfaceGetBaseAddress || !p_IOSurfaceGetBytesPerRow ||
        !p_IOSurfaceGetWidth || !p_IOSurfaceGetHeight || !p_IOSurfaceGetPixelFormat) {
        return 0;
    }
    if (p_IOSurfaceLock(surface, 1, 0)) return 0;
    base = (unsigned)p_IOSurfaceGetBaseAddress(surface);
    stride = p_IOSurfaceGetBytesPerRow(surface);
    width = p_IOSurfaceGetWidth(surface);
    height = p_IOSurfaceGetHeight(surface);
    format = p_IOSurfaceGetPixelFormat(surface);
    if (p_IOSurfaceGetPlaneCount && p_IOSurfaceGetPlaneCount(surface) == 2 &&
        p_IOSurfaceGetBaseAddressOfPlane && p_IOSurfaceGetBytesPerRowOfPlane) {
        base = (unsigned)p_IOSurfaceGetBaseAddressOfPlane(surface, 0);
        stride = p_IOSurfaceGetBytesPerRowOfPlane(surface, 0);
        uv = (unsigned)p_IOSurfaceGetBaseAddressOfPlane(surface, 1);
        uvstride = p_IOSurfaceGetBytesPerRowOfPlane(surface, 1);
    }
    unsigned rowbytes = (format == CA_FOURCC_565L || format == CA_FOURCC_555L) ? width * 2 : width * 4;
    int readable = width && width <= 2048 && height && height <= 2048;
    if (format == 0x34323076 || format == 0x34323066) {
        readable = readable && uv && !(width & 1) && !(height & 1) &&
            surface_fault_read(base, stride, height, width) &&
            surface_fault_read(uv, uvstride, height / 2, width);
    } else {
        readable = readable && !uv &&
            (format == CA_FOURCC_565L || format == CA_FOURCC_555L || format == CA_FOURCC_BGRA || format == 0x52474241) &&
            surface_fault_read(base, stride, height, rowbytes);
    }
    if (!readable) {
        static unsigned rejected;
        if (rejected++ < 8) {
            w("[mbxshim] rejected texture surface format="); wx(format);
            w(" size="); wd(width); w("x"); wd(height);
            w(" stride="); wd(stride); w(" base="); wx(base); w("\n");
        }
        p_IOSurfaceUnlock(surface, 1, 0);
        return 0;
    }
    result = qc(GLES_OP_BIND_SURFACE, gc, 8,
                A(target,base,stride,width,height,format,uv,uvstride)) == 0;
    p_IOSurfaceUnlock(surface, 1, 0);
    return result;
}

/*
 * GLESBindView is the drawable path -- the one a CAEAGLLayer actually takes.
 * See the contract above: we do not read a surface out of the drawable, we hand
 * CA a closure and CA calls us back with one.
 */
static int GLESBindView(void *gc, void *drawable, void *ifmt, void *flags)
{
    void **vt = drawable;
    ca_view_t *v;
    unsigned f = (unsigned)(unsigned long)ifmt;
    /*
     * ALWAYS ask CoreAnimation for a 32-bit surface, even when the app asked
     * for kEAGLColorFormatRGB565.
     *
     * A 565 drawable exists to halve the MBX's write bandwidth. We do not have
     * an MBX -- the frame is rendered by a host GPU and copied in by the host --
     * so the saving buys nothing here, and the app cannot tell: it never
     * touches this surface, it renders through GL and calls present.
     *
     * And a 565 surface is not merely pointless, it is INVISIBLE. Measured:
     * Angry Birds and Labyrinth both take this path, CA accepts the surface and
     * returns success for every present, the pixels are written to the address
     * CA gave us -- and nothing reaches the panel. Blitting the identical frame
     * straight to the LCD scanout (IT_GLES_PANEL_ALSO=1) shows it rendering
     * perfectly, so the loss is entirely in CA's compositing of a 16-bit
     * surface. Monkey Ball, which asks for RGBA8, composites correctly through
     * the very same code.
     */
    unsigned fourcc = CA_FOURCC_BGRA;
    int r;

    (void)flags;
    w("[mbxshim] GLESBindView drawable="); wx((unsigned long)drawable);
    w(" internalformat="); wx(f); w("\n");

    iosurface_init();
    v = ca_view_for_gc(gc, drawable != 0);
    if (v) ca_detach_view(v);
    /* A NULL drawable releases storage, as in the original engine. */
    if (!drawable) return 1;
    if (!v) {
        w("[mbxshim] GLESBindView: no free view slot\n");
        return 0;
    }
    v->gc = gc;
    v->block[0] = v->block;      /* handed back to us as createBuffer's arg0 */
    v->block[1] = (void *)ca_create_buffer;
    v->block[2] = (void *)ca_destroy_buffer;
    v->block[3] = 0;
    v->block[4] = 0;
    v->block[5] = gc;            /* +0x14 in the engine's block */
    v->block[6] = 0;
    v->block[7] = 0;

    r = ((ca_bind_fn)vt[1])(drawable, fourcc, v->block);
    w("[mbxshim]   drawable->bind(fourcc="); wx(fourcc); w(") -> "); wd((unsigned)r);
    w("\n");
    if (!r) {
        ca_view_t empty = {0};
        *v = empty;
        return 0;
    }
    v->drawable = drawable;

    /* The engine asks for the first buffer here, inside the bind, through
     * _ViewTextureBeginIfNeeded. Do the same: it is what makes CA announce its
     * surfaces, and until it happens there is nowhere to render. */
    v->need_buffer = 1;
    if (!ca_next_buffer(v)) {
        ca_detach_view(v);
        w("[mbxshim]   bind failed: no buffer from the drawable\n");
        return 0;
    }
    /* Storage follows the accepted CA layer, not the physical panel. Tell the
     * host before the app queries its renderbuffer size or draws into it.
     * Older hosts ignore this new operation and retain their legacy size. */
    if (qc(GLES_OP_DRAWABLE_STORAGE, gc, 2, A(v->width, v->height)) < 0) {
        ca_detach_view(v);
        return 0;
    }
    return 1;
}

/* 7E18 0x1d020 returns one after waiting for the bound texture. Returning
 * zero falsely reports a failure, including when no GPU work is pending. */
static int GLESFinishTexture(void *gc, unsigned target)
{
    if (target != 0x0de1 && target != 0x84f5) return 0;
    return qc(89, gc, 0, A(0)) == 0;
}

/*
 * Where a frame becomes visible.
 *
 * If CoreAnimation has handed us a surface, render into it and let CA composite
 * -- that is the real path, and the only one that survives CA repainting. The
 * blit straight to the panel is the fallback for clients that never bind one
 * (the direct-trap tests), and it is deliberately second: it bypasses CA
 * entirely, so anything CA draws next overwrites it, which makes a stale frame
 * look like a live one.
 */
static int GLESPresentView(void *gc, void *view)
{
    /* Whose frame this is. A GC that never bound a view has no slot, and falls
     * through to the panel blit below exactly as before. */
    ca_view_t *v = ca_view_for_gc(gc, 0);

    (void)view;

    /* This frame's target. CA hands out a different surface each frame, so the
     * one to render into is asked for now, not remembered from bind time. */
    ca_next_buffer(v);

    if (v && v->ref && v->base) {
        int rr;
        unsigned lockseed = 0;
        long long r;

        if (p_IOSurfaceLock) p_IOSurfaceLock(v->ref, 0, &lockseed);
        /* Re-read the base each frame: CA is entitled to move or reallocate a
         * surface between frames, and caching it would write into whatever now
         * owns the old address. */
        if (p_IOSurfaceGetBaseAddress) {
            v->base =
                (unsigned)(unsigned long)p_IOSurfaceGetBaseAddress(v->ref);
        }
        r = qc(GLES_OP_PRESENT_SURFACE, gc, 5,
               A(v->base, v->stride, v->width,
                 v->height, v->format));
        if (p_IOSurfaceUnlock) p_IOSurfaceUnlock(v->ref, 0, &lockseed);
        if (r != 0) {
            return 0;
        }

        /*
         * Tell CoreAnimation the surface now holds a finished frame. Until this
         * call CA has no reason to composite: the pixels are in its buffer but
         * nothing has said so. The stock engine does exactly this, and only
         * after its own flush -- GLESPresentView at 0xd6fc is
         * `ldr pc,[r3,#0x10]` with the drawable in r0 and 1 in r1, reached only
         * after _FlushHW has returned.
         */
        if (v->drawable) {
            void **vt = v->drawable;
            rr = ((ca_present_fn)vt[4])(v->drawable, 1);
            /* The engine re-arms its "wants a buffer" flag on the way out of
             * present (0xd70c), which is what makes it one nextBuffer per
             * frame rather than one for the whole context. */
            v->need_buffer = 1;
            /*
             * Count every one of these, not just the first.
             *
             * This used to log once per process, which said only that the FIRST
             * present succeeded -- and the first present succeeding is exactly
             * what a layer that CA later drops looks like. A running tally is
             * the difference between "we never signal" and "we signal and CA
             * refuses", and those have completely different fixes.
             */
            if (rr) present_ok++; else present_fail++;
            if (!present_logged || (present_ok + present_fail) % 300 == 0) {
                present_logged = 1;
                w("[mbxshim] present tally: ok="); wd(present_ok);
                w(" failed="); wd(present_fail);
                w(" (last -> "); wd((unsigned)rr); w(")\n");
            }
            return rr != 0;
        }
        return 1;
    }

    qc(GLES_OP_PRESENT, gc, 0, A(0));
    return 1;
}

/* EAGL passes the framebuffer's IOConnect port, transaction and layer.
 * Stock MBX queues these behind rendering; the software path signals selector
 * 20 directly (IOMobileFramebufferSwapSignal, 7E18 0x332e8e9c). */
static int GLESSwapNotification(void *gc, unsigned connection,
                                unsigned transaction, unsigned layer)
{
    static int (*signal_swap)(unsigned, unsigned, const unsigned long long *,
                              unsigned, unsigned long long *, unsigned *);
    if (!signal_swap) {
        void *io = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW);
        if (io) signal_swap = dlsym(io, "IOConnectCallScalarMethod");
    }
    if (!signal_swap || qc(89, gc, 0, A(0)) != 0) return 0;
    unsigned long long args[] = { transaction, layer };
    return signal_swap(connection, 20, args, 2, 0, 0) == 0;
}


/*
 * The table GLESGetEGLInterface hands back: nine function pointers, then
 * {extension string, bit} pairs. The framework never indexes past the ninth
 * pointer, so the extension list only has to be well-formed and terminated.
 */
static void *const gles_egl_interface[] = {
    /* +0x00 */ (void *)GLESCreateSharegroup,
    /* +0x04 */ (void *)GLESDestroySharegroup,
    /* +0x08 */ (void *)GLESCreateGC,
    /* +0x0c */ (void *)GLESDestroyGC,
    /* +0x10 */ (void *)GLESBindCoreSurface,
    /* +0x14 */ (void *)GLESBindView,
    /* +0x18 */ (void *)GLESFinishTexture,
    /* +0x1c */ (void *)GLESPresentView,
    /* +0x20 */ (void *)GLESSwapNotification,
    /* +0x24 onward: extension pairs. Empty list, null-terminated. */
    (void *)0, (void *)0,
};

void *GLESGetEGLInterface(void);
void *GLESGetEGLInterface(void)
{
    w("[mbxshim] GLESGetEGLInterface\n");
    return (void *)gles_egl_interface;
}
