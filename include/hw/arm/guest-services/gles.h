/*
 * OpenGL ES 1.1 high-level emulation -- guest/host request format.
 *
 * The guest side of this is a drop-in replacement for
 * /System/Library/Frameworks/OpenGLES.framework/MBXGLEngine.bundle. The stock
 * bundle talks to the AppleMBX kext through IOKit; ours talks to QEMU instead,
 * so the whole IOKit/MBX rendezvous is bypassed and the host renders with real
 * OpenGL.
 *
 * Every gl* entry point in OpenGLES.framework is the same six-instruction
 * trampoline: it loads the per-thread GC out of TSD key 30, indexes a
 * framework-owned dispatch table with a fixed slot, and tail-calls through it
 * with the GC as arg0. So a single request shape covers all 178 of them: the
 * slot says which call it was, and the arguments follow. Slot numbers are the
 * table's byte offset / 4 -- a numbering the framework already owns, so there
 * is no table of ours that has to be kept in sync with it.
 *
 * WHY THIS STRUCT IS EXACTLY 32 BYTES
 *
 * It sits in qemu_call_t's args union, and that union's size decides where
 * retval lands. contrib/it-kbd-agent hardcodes the layout as
 * "call_number(4) + args(32) + retval(8) + error(8)", and -- worse -- it is
 * *compiled into NAND images that already exist*. Widening the union would
 * shift retval out from under every agent binary already injected into every
 * image, with no build error anywhere to catch it. So this struct is capped at
 * 32 bytes, and general.h static-asserts the total.
 *
 * That cap is why arguments are split: four inline, and anything longer spilled
 * to a guest buffer. glTexImage2D and glCompressedTexSubImage2D take nine
 * scalars, which was never going to fit inline.
 *
 * Copyright (c) 2026 the qemu-ios contributors.
 */

#ifndef HW_ARM_GUEST_SERVICES_GLES_H
#define HW_ARM_GUEST_SERVICES_GLES_H

#include <stdint.h>

#define QC_GLES_INLINE_ARGS 4

typedef struct __attribute__((packed)) {
    /* Dispatch-table slot, i.e. the framework's table offset / 4. */
    uint32_t slot;
    /* The engine's GC handle -- arg0 of every trampoline. Opaque to the guest.
     * The host does NOT switch context on it: there is one host GL context and
     * its state is global, shared by every guest GC (see the note on the `gh`
     * global in gles-host.c). This is carried only so the host can attribute
     * per-GC statistics -- which GC drew what, and which one presented. */
    uint32_t ctx;
    /* How many 32-bit arguments this call carries, GC excluded. */
    uint32_t argc;
    /* Guest VA of the full argument array, used when argc > QC_GLES_INLINE_ARGS.
     * Zero when the inline array below carries everything. */
    uint32_t spill;
    /* Arguments widened to 32 bits. Floats travel as their bit patterns;
     * pointers are guest virtual addresses the host reads with
     * cpu_memory_rw_debug. */
    uint32_t args[QC_GLES_INLINE_ARGS];
} qc_gles_args_t;

/*
 * Dispatch slots.
 *
 * These are NOT ours to choose. Each is the byte offset of an entry point in
 * OpenGLES.framework's own dispatch table, divided by four -- the framework's
 * trampolines index that table with constants baked into their code, so the
 * numbering is fixed by Apple and the guest shim must fill the same indices.
 * The block offset a trampoline uses is 0x10 + slot*4, because the table starts
 * at X+0x10 within the 6592-byte context block that GLESCreateGC populates.
 *
 * Recovered from MBXGLEngine and cross-checked against eight framework call
 * sites; the full 171-slot map is in GATE1_slotmap.txt. Only the slots this
 * implementation actually handles are named here -- see SCOPE in gles-host.c.
 */
#define GLES_SLOT_CLEAR_STENCIL             15
#define GLES_SLOT_COLOR4UB                  43
#define GLES_SLOT_CULL_FACE                 57
#define GLES_SLOT_NORMAL3F                  182
#define GLES_SLOT_POINT_SIZE                199
#define GLES_SLOT_POLYGON_OFFSET            201
#define GLES_SLOT_STENCIL_FUNC              254
#define GLES_SLOT_STENCIL_OP                256
#define GLES_SLOT_MULTI_TEX_COORD4F         369
#define GLES_SLOT_SAMPLE_COVERAGE           459
#define GLES_SLOT_ALPHA_FUNCX               761
#define GLES_SLOT_CLEAR_COLORX              762
#define GLES_SLOT_CLEAR_DEPTHX              764
#define GLES_SLOT_COLOR4X                   767
#define GLES_SLOT_DEPTH_RANGEF              768
#define GLES_SLOT_DEPTH_RANGEX              769
#define GLES_SLOT_FRUSTUMX                  773
#define GLES_SLOT_LINE_WIDTHX               785
#define GLES_SLOT_NORMAL3X                  790
#define GLES_SLOT_ORTHOX                    792
#define GLES_SLOT_POINT_SIZEX               793
#define GLES_SLOT_POLYGON_OFFSETX           794
#define GLES_SLOT_ROTATEX                   795
#define GLES_SLOT_TRANSLATEX                801
#define GLES_SLOT_MULTI_TEX_COORD4X         802
#define GLES_SLOT_POINT_SIZE_POINTER_OES    806
#define GLES_SLOT_SAMPLE_COVERAGEX          803
#define GLES_SLOT_GET_BOOLEANV              99
#define GLES_SLOT_GET_POINTERV              115
#define GLES_SLOT_LOAD_MATRIXX              786
#define GLES_SLOT_MULT_MATRIXX              789
#define GLES_SLOT_COLOR_MASK            49
#define GLES_SLOT_STENCIL_MASK          255
#define GLES_SLOT_IS_RENDERBUFFER       665
#define GLES_SLOT_IS_FRAMEBUFFER        671
#define GLES_SLOT_GENERATE_MIPMAP        681
#define GLES_SLOT_BIND_TEXTURE          5    /* 0x0024 */
#define GLES_SLOT_CLEAR                 10   /* 0x0038 */
#define GLES_SLOT_CLEAR_COLOR           12   /* 0x0040 */
#define GLES_SLOT_COLOR4F               37   /* 0x00a4 */
#define GLES_SLOT_DISABLE               63   /* 0x010c */
#define GLES_SLOT_DISABLE_CLIENT_STATE  64   /* 0x0110 */
#define GLES_SLOT_DRAW_ARRAYS           65   /* 0x0114 */
#define GLES_SLOT_ENABLE                72   /* 0x0130 */
#define GLES_SLOT_ENABLE_CLIENT_STATE   73   /* 0x0134 */
#define GLES_SLOT_FINISH                89   /* 0x0174 */
#define GLES_SLOT_FLUSH                 90   /* 0x0178 */
#define GLES_SLOT_GEN_TEXTURES          98   /* 0x0198 */
#define GLES_SLOT_GET_ERROR             102  /* 0x01a8 */
#define GLES_SLOT_LOAD_IDENTITY         157  /* 0x0284 */
#define GLES_SLOT_MATRIX_MODE           174  /* 0x02c8 */
#define GLES_SLOT_TEXCOORD_POINTER      289  /* 0x0494 */
#define GLES_SLOT_TEX_IMAGE_2D          301  /* 0x04c4 */
#define GLES_SLOT_GET_LIGHTFV             105
#define GLES_SLOT_GET_MATERIALFV          110
#define GLES_SLOT_GET_TEX_ENVFV           118
#define GLES_SLOT_GET_TEX_ENVIV           119
#define GLES_SLOT_GET_TEX_PARAMETERFV     126
#define GLES_SLOT_GET_TEX_PARAMETERIV     127
#define GLES_SLOT_TEX_ENVIV               293
#define GLES_SLOT_TEX_PARAMETERF          302
#define GLES_SLOT_TEX_PARAMETERFV         303
#define GLES_SLOT_TEX_PARAMETERIV         305
#define GLES_SLOT_TEX_PARAMETERI        304  /* 0x04d0 */
#define GLES_SLOT_VERTEX_POINTER        334  /* 0x0548 */
#define GLES_SLOT_VIEWPORT              335  /* 0x054c */
#define GLES_SLOT_ORTHOF                791  /* 0x0c6c */

/*
 * The fixed-function set a real ES 1.1 game needs on top of the bring-up subset
 * above: matrix stack, lighting, fog, blending, depth, and the remaining client
 * arrays. Every one of these was recovered the same way as the slots above --
 * by disassembling the armv6 slice of the 3.1.3 SDK's OpenGLES and reading the
 * `ldr pc, [ctx, #off]` out of each trampoline -- and the extractor was
 * validated by reproducing all 23 of the already-known slots above exactly
 * before any new number was trusted.
 *
 * The set is not a guess at what games use: it is `nm -u` on Cube Runner,
 * which imports exactly 41 gl* symbols and no others.
 */
#define GLES_SLOT_BLEND_FUNC            7    /* 0x002c */
#define GLES_SLOT_COLOR_POINTER         51   /* 0x00dc */
#define GLES_SLOT_DEPTH_MASK            61   /* 0x0104 */
#define GLES_SLOT_DRAW_ELEMENTS         67   /* 0x011c */
#define GLES_SLOT_FOGF                  91   /* 0x017c */
#define GLES_SLOT_FOGFV                 92   /* 0x0180 */
#define GLES_SLOT_HINT                  128  /* 0x0210 */
#define GLES_SLOT_LIGHTFV               151  /* 0x026c */
#define GLES_SLOT_LINE_WIDTH            155  /* 0x027c */
#define GLES_SLOT_MATERIALFV            171  /* 0x02bc */
#define GLES_SLOT_MATERIALF             170  /* 0x02b8 */

/*
 * Calls the shim used to drop on the floor. Each one was silently unimplemented
 * on BOTH sides, which is worse than it sounds: the host's unhandled-slot
 * warning cannot fire for a call the shim never forwards, so these were
 * invisible from the QEMU log no matter how verbose it was set.
 *
 * glCopyTexImage2D is the one that mattered. Labyrinth builds its render target
 * by copying the framebuffer into a texture, so dropping it left that texture
 * with NO storage -- and once framebuffer objects became real, an FBO with a
 * storage-less texture attached reports INCOMPLETE, which the app treats as
 * fatal. It is the reason Labyrinth went from a white level to a crash on Play.
 */
#define GLES_SLOT_COPY_TEX_IMAGE_2D      54  /* 0x00d8 */
#define GLES_SLOT_GET_FLOATV            103  /* 0x019c */
#define GLES_SLOT_READ_PIXELS           237  /* 0x03b4 */
#define GLES_SLOT_TEX_ENVF              290  /* 0x0498 */
#define GLES_SLOT_TEX_ENVFV             291  /* 0x049c */
#define GLES_SLOT_TEX_PARAMETERX        799  /* 0x0c7c */
#define GLES_SLOT_MULT_MATRIXF          176  /* 0x02d0 */
#define GLES_SLOT_NORMAL_POINTER        188  /* 0x0300 */
#define GLES_SLOT_POP_MATRIX            205  /* 0x0344 */
#define GLES_SLOT_PUSH_MATRIX           210  /* 0x0358 */
#define GLES_SLOT_ROTATEF               248  /* 0x03f0 */
#define GLES_SLOT_SCALEF                250  /* 0x03f8 */
#define GLES_SLOT_SHADE_MODEL           253  /* 0x0404 */
#define GLES_SLOT_TRANSLATEF            309  /* 0x04e4 */
#define GLES_SLOT_CLEAR_DEPTHF          763  /* 0x0bfc */
#define GLES_SLOT_FRUSTUMF              772  /* 0x0c20 */

/*
 * The remainder of Super Monkey Ball's import set (`nm -u` lists 49 gl*
 * symbols; these seven were the only ones missing). Same provenance as above:
 * read out of the 3.1.3 SDK trampolines, extractor re-validated against the
 * known slots first. glLoadMatrixf and glTexSubImage2D are the load-bearing
 * two -- a console-engine port sets matrices wholesale and streams textures
 * into pre-allocated storage, and with both silently dropped every draw lands
 * off-screen or samples an incomplete (white) texture.
 */
#define GLES_SLOT_ALPHA_FUNC            1    /* 0x0014 */
#define GLES_SLOT_DELETE_TEXTURES       59   /* 0x00fc */
#define GLES_SLOT_GET_INTEGERV          104  /* 0x01b0 */
#define GLES_SLOT_LOAD_MATRIXF          159  /* 0x028c */
#define GLES_SLOT_TEX_SUB_IMAGE_2D      307  /* 0x04dc */
#define GLES_SLOT_BIND_BUFFER           642  /* 0x0a18 */
#define GLES_SLOT_SCALEX                796  /* 0x0c80 */

/*
 * Cheap state setters a survey of 20 real App Store apps found imported but
 * unimplemented. Same provenance as everything above -- read out of the 3.1.3
 * SDK trampolines, with the extractor re-validated against seven already-known
 * slots (7, 1, 61, 253, 301, 307, 335) before any new number was trusted.
 *
 * glPixelStorei is the one with teeth. ES 1.1's GL_UNPACK_ALIGNMENT defaults to
 * 4, so a guest uploading GL_RGB, GL_LUMINANCE, GL_ALPHA or GL_LUMINANCE_ALPHA
 * at a width whose row is not a multiple of four bytes pads every row -- and
 * the upload path here used to size the fetch as w*h*bpp, with no padding at
 * all. That under-reads, and each row after the first is sheared by a
 * progressively larger offset. Honouring the alignment is not about supporting
 * the call; it is a correctness fix to uploads that were already wrong.
 */
#define GLES_SLOT_DEPTH_FUNC            60   /* 0x0100 */
#define GLES_SLOT_FRONT_FACE            95   /* 0x018c */
#define GLES_SLOT_PIXEL_STOREI          195  /* 0x031c */
#define GLES_SLOT_SCISSOR               251  /* 0x03fc */
#define GLES_SLOT_TEX_ENVI              292  /* 0x04a0 */
#define GLES_SLOT_CLIENT_ACTIVE_TEXTURE 341  /* 0x0558 */
#define GLES_SLOT_ACTIVE_TEXTURE        342  /* 0x055c */

/*
 * Compressed textures and buffer objects.
 *
 * Read out of the 3.1.3 SDK trampolines like everything above, and
 * cross-checked against contrib/it-gles/slotmap.txt; the extractor was
 * re-validated here by re-deriving GLES_SLOT_BIND_BUFFER (642, `ldr r12,
 * [r3, #0xa18]`) before any neighbouring number was trusted.
 *
 * glCompressedTexImage2D is what a PowerVR-era title uploads its art with --
 * PVRTC was THE texture format on the MBX -- and it takes eight scalars, so it
 * spills exactly like glTexImage2D. The host has no PVRTC, so it decodes to
 * RGBA8 on the CPU; see gles-host.c.
 *
 * The buffer-object four close the asymmetry glBindBuffer was left in: the bind
 * was accepted with no way to ever put data behind it, so any engine keeping
 * geometry in a VBO drew from memory the host never received.
 */
#define GLES_SLOT_COMPRESSED_TEX_IMAGE_2D     380  /* 0x0600 */
#define GLES_SLOT_COMPRESSED_TEX_SUB_IMAGE_2D 383  /* 0x060c */
#define GLES_SLOT_DELETE_BUFFERS              643  /* 0x0a1c */
#define GLES_SLOT_GEN_BUFFERS                 644  /* 0x0a20 */
#define GLES_SLOT_BUFFER_DATA                 646  /* 0x0a28 */
#define GLES_SLOT_BUFFER_SUB_DATA             647  /* 0x0a2c */

/* OES framebuffer-object entry points. EAGL uses these itself inside
 * -renderbufferStorage:fromDrawable:, so a real CAEAGLLayer client cannot get
 * off the ground without them -- they are not optional coverage. The non-OES
 * and OES spellings share a slot (the framework aliases them). */
#define GLES_SLOT_BIND_RENDERBUFFER     666  /* 0x0a78 */
#define GLES_SLOT_DELETE_RENDERBUFFERS  667  /* 0x0a7c */
#define GLES_SLOT_GEN_RENDERBUFFERS     668  /* 0x0a80 */
#define GLES_SLOT_RENDERBUFFER_STORAGE  669  /* 0x0a84 */
#define GLES_SLOT_GET_RB_PARAMETERIV    670  /* 0x0a88 */
#define GLES_SLOT_BIND_FRAMEBUFFER      672  /* 0x0a90 */
#define GLES_SLOT_DELETE_FRAMEBUFFERS   673  /* 0x0a94 */
#define GLES_SLOT_GEN_FRAMEBUFFERS      674  /* 0x0a98 */
#define GLES_SLOT_CHECK_FB_STATUS       675  /* 0x0a9c */
#define GLES_SLOT_FB_TEXTURE_2D         677  /* 0x0aa4 */
#define GLES_SLOT_FB_RENDERBUFFER       679  /* 0x0aac */
#define GLES_SLOT_GET_FB_ATTACH_PARAM   680  /* 0x0ab0 */

/* The framework's table tops out at slot 820. Engine-level operations that are
 * not dispatch entries at all live above it, well clear of anything Apple could
 * be indexing. */
#define GLES_OP_BASE                    0x1000
/* Debug present: blit straight to where the LCD scans out. Bypasses
 * CoreAnimation, so whatever CA draws next overwrites it. */
#define GLES_OP_PRESENT                 (GLES_OP_BASE + 0)
/*
 * Real present: write the frame into a caller-supplied CPU-addressable buffer.
 *
 * args: base (guest VA), stride in bytes, width, height, format
 *
 * This is the shape CoreAnimation's IOSurface has. The engine is a pure
 * consumer of that surface -- all ten of the stock bundle's IOSurface imports
 * are read-side (Get*, Lock/Unlock; there is no IOSurfaceCreate anywhere in
 * it) -- so the host never needs to know what an IOSurface is. It is handed an
 * address, a stride and a format, and it writes pixels.
 */
#define GLES_OP_PRESENT_SURFACE         (GLES_OP_BASE + 1)

/*
 * The shim's own log, forwarded to the host.
 *
 * args: guest pointer to bytes, length
 *
 * The shim writes to fd 2, which for an app launched by SpringBoard goes
 * nowhere anybody can read. That invisibility has cost real debugging time more
 * than once: whether CoreAnimation ACCEPTED a surface, and whether it accepted
 * the frames presented into it, are both decided guest-side and were only ever
 * reported there. The host cannot infer either.
 */
#define GLES_OP_LOG                     (GLES_OP_BASE + 2)
/* target, base, stride, width, height, FourCC, UV base, UV stride.
 * A zero base detaches the currently bound texture from guest memory. */
#define GLES_OP_BIND_SURFACE            (GLES_OP_BASE + 3)
/* Native context lifecycle. New operations return opaque positive handles;
 * a zero sharegroup handle means the backend only supports legacy contexts. */
#define GLES_OP_NEW_SHAREGROUP          (GLES_OP_BASE + 4)
#define GLES_OP_DELETE_SHAREGROUP       (GLES_OP_BASE + 5)
#define GLES_OP_NEW_CONTEXT             (GLES_OP_BASE + 6)
#define GLES_OP_DELETE_CONTEXT          (GLES_OP_BASE + 7)
/* Accepted CA drawable width and height, before GL size queries/draws.
 * Old shims retain the panel-sized default. The request ABI is unchanged. */
#define GLES_OP_DRAWABLE_STORAGE        (GLES_OP_BASE + 8)

/* Surface pixel formats, as IOSurfaceGetPixelFormat reports them (FourCC). */
#define GLES_SURFACE_BGRA32             0x42475241  /* 'BGRA' */
#define GLES_SURFACE_RGBA32             0x52474241  /* 'RGBA' */
/* 16 bits per pixel, not 32. A CAEAGLLayer that asked for
 * kEAGLColorFormatRGB565 gets this, and every size in the present path has to
 * follow the format rather than assume four bytes. */
#define GLES_SURFACE_RGB555 0x4c353535 /* L555: opaque little-endian RGB555 */
#define GLES_SURFACE_RGB565             0x4c353635  /* 'L565' */

#ifndef OUT_OF_TREE_BUILD
int64_t qc_handle_gles(CPUState *cpu, qc_gles_args_t *a);
void qc_gles_dump_stats(void);
int64_t gles_host_call(CPUState *cpu, uint32_t slot, uint32_t ctx,
                       uint32_t argc, const uint32_t *args);
void gles_host_stats(uint64_t *draws, uint64_t *presents);
void gles_host_reset(void);
int gles_host_context_count(void);
#endif

#endif /* HW_ARM_GUEST_SERVICES_GLES_H */
