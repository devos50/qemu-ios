/*
 * glapp -- a real iOS application, with a real CAEAGLLayer, drawing through
 * the GLES HLE path and composited by CoreAnimation.
 *
 * gles_fw already proved that OpenGLES.framework dispatches into our
 * MBXGLEngine replacement. What it could not prove is the last inch: a
 * command-line process can create a UIWindow, but SpringBoard never composites
 * it, so -[EAGLContext renderbufferStorage:fromDrawable:] has nothing
 * meaningful behind it and the frame can only reach the panel through the debug
 * blit that bypasses CoreAnimation entirely. This is an app bundle launched by
 * SpringBoard, so the layer is real and CA owns the surface.
 *
 * WHY IT IS PLAIN C AND NOT OBJECTIVE-C
 *
 * The armv6 toolchain (contrib/armv6-toolchain) links against the 3.1.3 SDK,
 * and `ld -lobjc` against that SDK is fatal -- the modern linker reads the 2009
 * dylib's platform as 'unknown', which it tolerates for libSystem alone. So
 * there is no way to link the ObjC runtime, and an .m file's class and category
 * data would have nothing to attach to.
 *
 * The runtime is therefore dlopen'd and the two classes an app needs -- an
 * application delegate and a UIView whose +layerClass is CAEAGLLayer -- are
 * built at runtime with objc_allocateClassPair. UIApplicationMain then finds
 * "GLTestDelegate" by name exactly as it would a compiled one.
 *
 * Every objc_msgSend call goes through a prototype that names its real argument
 * types rather than through the variadic declaration. On ARM the variadic ABI
 * is not the same as the normal one (floats promote to double), and CGRect and
 * NSTimeInterval both travel through here.
 *
 * WHAT IT DRAWS, AND WHY THOSE COLOURS
 *
 * The GL view is deliberately NOT fullscreen: it is inset in a red window. So
 * the three ways this can end are told apart by pixels alone, with no colour
 * shared between them:
 *
 *   - CA composited our surface -> magenta right half and cyan left half
 *     INSIDE the inset, red around it, status bar still on top.
 *   - the shim fell back to GLES_OP_PRESENT (the debug blit straight to the
 *     scanout) -> the same magenta/cyan covering the WHOLE panel, red and
 *     status bar gone.
 *   - nothing rendered -> red everywhere.
 *
 * A fullscreen layer would make the first two identical, and "it looked right"
 * would not have distinguished the path that is the entire point of this test
 * from the one that was already known to work.
 *
 * Build with contrib/it-gles/build.sh, which assembles GLTest.app around it.
 */

/* ------------------------------------------------------------------ libc --- */

extern long write(int, const void *, unsigned long);
extern int open(const char *, int, ...);
extern int dup2(int, int);
extern void _exit(int);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern const char *dlerror(void);

#define RTLD_NOW  2
#define O_WRONLY  0x0001
#define O_CREAT   0x0200
#define O_TRUNC   0x0400
#define O_APPEND  0x0008

static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void w(const char *s) { write(2, s, slen(s)); }
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

/* -------------------------------------------------------------- ObjC glue --- */

typedef void *id_;
typedef void *SEL_;
typedef void *Class_;

/* CGRect, spelled out. Passing it through a prototype that says exactly this
 * lets the compiler apply AAPCS to it; hand-splitting it into words would have
 * to get the register/stack boundary right by hand. */
typedef struct { float x, y; } Point_;
typedef struct { float w, h; } Size_;
typedef struct { Point_ origin; Size_ size; } Rect_;

static void *(*p_objc_getClass)(const char *);
static void *(*p_sel_registerName)(const char *);
static void *(*p_objc_msgSend)(void *, void *, ...);
static void *(*p_objc_allocateClassPair)(void *, const char *, unsigned long);
static void (*p_objc_registerClassPair)(void *);
static int (*p_class_addMethod)(void *, void *, void *, const char *);
static void *(*p_object_getClass)(void *);

/* objc_msgSend under the shapes we actually send. */
static id_ (*m0)(id_, SEL_);
static id_ (*m1)(id_, SEL_, id_);
static id_ (*m1u)(id_, SEL_, unsigned);
static id_ (*m1s)(id_, SEL_, const char *);
static int (*m2ru)(id_, SEL_, unsigned, id_);
static int (*m1uI)(id_, SEL_, unsigned);
static id_ (*mrect)(id_, SEL_, Rect_);
static id_ (*mtimer)(id_, SEL_, double, id_, SEL_, id_, int);

#define S(n) (p_sel_registerName(n))
#define C(n) (p_objc_getClass(n))

static id_ nsstr(const char *s)
{
    return m1s(C("NSString"), S("stringWithUTF8String:"), s);
}

/* NSLog, so the interesting lines also reach idevicesyslog. It is variadic,
 * but with no arguments after the format the two ABIs agree. */
static void (*p_NSLog)(id_);
static void nslog(const char *s) { if (p_NSLog) p_NSLog(nsstr(s)); }

/* ------------------------------------------------------------------- GL --- */

#define GL_TRIANGLE_STRIP        0x0005
#define GL_FLOAT                 0x1406
#define GL_COLOR_BUFFER_BIT      0x4000
#define GL_VERTEX_ARRAY          0x8074
#define GL_MODELVIEW             0x1700
#define GL_PROJECTION            0x1701
#define GL_RENDERBUFFER_OES      0x8D41
#define GL_FRAMEBUFFER_OES       0x8D40
#define GL_COLOR_ATTACHMENT0_OES 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE_OES 0x8CD5
#define GL_RENDERBUFFER_WIDTH_OES   0x8D42
#define GL_RENDERBUFFER_HEIGHT_OES  0x8D43

static void (*p_glClearColor)(float, float, float, float);
static void (*p_glClear)(unsigned);
static void (*p_glViewport)(int, int, int, int);
static void (*p_glColor4f)(float, float, float, float);
static void (*p_glEnableClientState)(unsigned);
static void (*p_glVertexPointer)(int, unsigned, int, const void *);
static void (*p_glDrawArrays)(unsigned, int, int);
static unsigned (*p_glGetError)(void);
static void (*p_glMatrixMode)(unsigned);
static void (*p_glLoadIdentity)(void);
static void (*p_glOrthof)(float, float, float, float, float, float);
static void (*p_glGenRenderbuffersOES)(int, unsigned *);
static void (*p_glBindRenderbufferOES)(unsigned, unsigned);
static void (*p_glGenFramebuffersOES)(int, unsigned *);
static void (*p_glBindFramebufferOES)(unsigned, unsigned);
static void (*p_glFramebufferRenderbufferOES)(unsigned, unsigned, unsigned, unsigned);
static unsigned (*p_glCheckFramebufferStatusOES)(unsigned);
static void (*p_glGetRenderbufferParameterivOES)(unsigned, unsigned, int *);
static void (*p_glGenTextures)(int, unsigned *);
static void (*p_glBindTexture)(unsigned, unsigned);
static void (*p_glTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
static void (*p_glTexParameteri)(unsigned, unsigned, int);
static void (*p_glTexParameteriv)(unsigned, unsigned, const int *);
static void (*p_glEnable)(unsigned);
static void (*p_glDisable)(unsigned);
static void (*p_glDrawTexfOES)(float, float, float, float, float);
#define GL_TEXTURE_2D            0x0DE1
#define GL_RGBA                  0x1908
#define GL_UNSIGNED_BYTE         0x1401
#define GL_NEAREST               0x2600
#define GL_TEXTURE_MIN_FILTER    0x2801
#define GL_TEXTURE_MAG_FILTER    0x2800
#define GL_TEXTURE_CROP_RECT_OES 0x8B9D
static unsigned g_tex;

/* ---------------------------------------------------------------- state --- */

/* The view is inset so that CA compositing and the debug blit cannot produce
 * the same picture. See the header comment. */
#define VIEW_X 40
#define VIEW_Y 60
#define VIEW_W 240
#define VIEW_H 360

static id_ g_window, g_view, g_ctx;
static unsigned g_rb, g_fb;
static unsigned g_frames;
static int g_gl_ready;

/* ------------------------------------------------------------- delegate --- */

static void draw_frame(void)
{
    /* A quad over the left half, in a magenta field. Both colours are ones no
     * part of the iOS UI produces, so a screenshot cannot be read two ways. */
    static const float quad[8] = {
        0.0f,           0.0f,
        VIEW_W / 2.0f,  0.0f,
        0.0f,           (float)VIEW_H,
        VIEW_W / 2.0f,  (float)VIEW_H,
    };

    p_glBindFramebufferOES(GL_FRAMEBUFFER_OES, g_fb);
    p_glViewport(0, 0, VIEW_W, VIEW_H);
    p_glMatrixMode(GL_PROJECTION);
    p_glLoadIdentity();
    p_glOrthof(0.0f, (float)VIEW_W, 0.0f, (float)VIEW_H, -1.0f, 1.0f);
    p_glMatrixMode(GL_MODELVIEW);
    p_glLoadIdentity();

    p_glClearColor(1.0f, 0.0f, 1.0f, 1.0f);     /* magenta */
    p_glClear(GL_COLOR_BUFFER_BIT);

    p_glColor4f(0.0f, 1.0f, 1.0f, 1.0f);        /* cyan */
    p_glEnableClientState(GL_VERTEX_ARRAY);
    p_glVertexPointer(2, GL_FLOAT, 0, quad);
    p_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    /* A yellow texture over the lower right quarter, via OES_draw_texture --
     * the third colour the regression check counts. A yellow field with no
     * DrawTex support reads as plain magenta there. */
    if (p_glDrawTexfOES && g_tex) {
        static const int crop[4] = { 0, 0, 4, 4 };
        p_glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        p_glEnable(GL_TEXTURE_2D);
        p_glBindTexture(GL_TEXTURE_2D, g_tex);
        p_glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_CROP_RECT_OES, crop);
        p_glDrawTexfOES(VIEW_W / 2.0f, 0.0f, 0.0f, VIEW_W / 2.0f, VIEW_H / 2.0f);
        p_glDisable(GL_TEXTURE_2D);
    }
}

static void present_frame(void)
{
    p_glBindRenderbufferOES(GL_RENDERBUFFER_OES, g_rb);
    m1uI(g_ctx, S("presentRenderbuffer:"), GL_RENDERBUFFER_OES);
}

static void app_tick(id_ self, SEL_ cmd, id_ timer)
{
    (void)self; (void)cmd; (void)timer;

    if (!g_gl_ready) {
        return;
    }
    draw_frame();
    present_frame();
    g_frames++;
    if (g_frames == 1 || g_frames == 20) {
        w("[glapp] frames="); wd(g_frames);
        w(" glGetError="); wx(p_glGetError ? p_glGetError() : 0);
        w("\n");
        nslog(g_frames == 1 ? "glapp: first frame presented"
                            : "glapp: 20 frames presented");
    }
}

/*
 * The CAEAGLLayer is the whole point: a UIView reports its layer class as a
 * class method, and UIView's -initWithFrame: asks for it while building the
 * layer, so this has to be on the metaclass and it has to be there before the
 * first instance exists.
 */
static Class_ view_layer_class(id_ self, SEL_ cmd)
{
    (void)self; (void)cmd;
    return C("CAEAGLLayer");
}

static void gl_setup(void)
{
    id_ layer = m0(g_view, S("layer"));
    int rbw = 0, rbh = 0;
    int ok;

    w("[glapp] layer="); wx((unsigned long)layer); w("\n");

    g_ctx = m1u(m0(C("EAGLContext"), S("alloc")), S("initWithAPI:"), 1);
    w("[glapp] EAGLContext="); wx((unsigned long)g_ctx); w("\n");
    if (!g_ctx) {
        /* The classic cause is GLESCreateGC handing back 0 -- the framework
         * then returns nil and every other log line still looks healthy. */
        nslog("glapp: EAGLContext is nil");
        return;
    }
    if (!m1(C("EAGLContext"), S("setCurrentContext:"), g_ctx)) {
        w("[glapp] setCurrentContext FAILED\n");
        nslog("glapp: setCurrentContext failed");
        return;
    }

    /* The OES renderbuffer entry points are dispatch slots like any other, and
     * the shim does not implement them yet -- the stub returns 0 without
     * writing the name back. Seed the names so the framework has something
     * consistent to bind either way, and log what actually came back. */
    g_rb = 1;
    g_fb = 1;
    if (p_glGenRenderbuffersOES) p_glGenRenderbuffersOES(1, &g_rb);
    p_glBindRenderbufferOES(GL_RENDERBUFFER_OES, g_rb);

    ok = m2ru(g_ctx, S("renderbufferStorage:fromDrawable:"),
              GL_RENDERBUFFER_OES, layer);
    w("[glapp] renderbufferStorage:fromDrawable: -> "); wd((unsigned)ok); w("\n");

    if (p_glGetRenderbufferParameterivOES) {
        p_glGetRenderbufferParameterivOES(GL_RENDERBUFFER_OES,
                                          GL_RENDERBUFFER_WIDTH_OES, &rbw);
        p_glGetRenderbufferParameterivOES(GL_RENDERBUFFER_OES,
                                          GL_RENDERBUFFER_HEIGHT_OES, &rbh);
    }
    w("[glapp] renderbuffer "); wd((unsigned)rbw); w("x"); wd((unsigned)rbh); w("\n");

    if (p_glGenFramebuffersOES) p_glGenFramebuffersOES(1, &g_fb);
    p_glBindFramebufferOES(GL_FRAMEBUFFER_OES, g_fb);
    p_glFramebufferRenderbufferOES(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES,
                                   GL_RENDERBUFFER_OES, g_rb);
    if (p_glCheckFramebufferStatusOES) {
        w("[glapp] fbo status="); wx(p_glCheckFramebufferStatusOES(GL_FRAMEBUFFER_OES));
        w("\n");
    }

    if (p_glGenTextures && p_glTexImage2D) {
        static unsigned char yellow[4 * 4 * 4];
        unsigned i;
        for (i = 0; i < sizeof yellow; i += 4) {
            yellow[i] = 255; yellow[i + 1] = 255; yellow[i + 2] = 0; yellow[i + 3] = 255;
        }
        p_glGenTextures(1, &g_tex);
        p_glBindTexture(GL_TEXTURE_2D, g_tex);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, yellow);
        w("[glapp] texture="); wd(g_tex); w("\n");
    }
    g_gl_ready = 1;
    nslog("glapp: GL is set up");
}

static void app_did_finish(id_ self, SEL_ cmd, id_ app)
{
    Rect_ full = { { 0.0f, 0.0f }, { 320.0f, 480.0f } };
    Rect_ inset = { { (float)VIEW_X, (float)VIEW_Y },
                    { (float)VIEW_W, (float)VIEW_H } };

    (void)cmd; (void)app;
    w("[glapp] applicationDidFinishLaunching\n");
    nslog("glapp: applicationDidFinishLaunching");

    g_window = mrect(m0(C("UIWindow"), S("alloc")), S("initWithFrame:"), full);
    m1(g_window, S("setBackgroundColor:"), m0(C("UIColor"), S("redColor")));

    g_view = mrect(m0(C("GLTestView"), S("alloc")), S("initWithFrame:"), inset);
    w("[glapp] view="); wx((unsigned long)g_view); w("\n");
    m1u(g_view, S("setOpaque:"), 1);
    m1(g_window, S("addSubview:"), g_view);
    m0(g_window, S("makeKeyAndVisible"));

    gl_setup();

    /* Draw on a timer rather than once. A single frame drawn before CA's first
     * composite is indistinguishable from one that was never drawn. */
    mtimer(C("NSTimer"),
           S("scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:"),
           0.05, self, S("tick:"), (id_)0, 1);
}

/* --------------------------------------------------------------- startup --- */

static int resolve_objc(void)
{
    void *o = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW);
    if (!o) { w("[glapp] no libobjc\n"); return 0; }

    p_objc_getClass          = dlsym(o, "objc_getClass");
    p_sel_registerName       = dlsym(o, "sel_registerName");
    p_objc_msgSend           = dlsym(o, "objc_msgSend");
    p_objc_allocateClassPair = dlsym(o, "objc_allocateClassPair");
    p_objc_registerClassPair = dlsym(o, "objc_registerClassPair");
    p_class_addMethod        = dlsym(o, "class_addMethod");
    p_object_getClass        = dlsym(o, "object_getClass");

    if (!p_objc_getClass || !p_objc_msgSend || !p_objc_allocateClassPair ||
        !p_class_addMethod || !p_object_getClass) {
        w("[glapp] missing objc symbols\n");
        return 0;
    }

    m0    = (id_ (*)(id_, SEL_))p_objc_msgSend;
    m1    = (id_ (*)(id_, SEL_, id_))p_objc_msgSend;
    m1u   = (id_ (*)(id_, SEL_, unsigned))p_objc_msgSend;
    m1s   = (id_ (*)(id_, SEL_, const char *))p_objc_msgSend;
    m2ru  = (int (*)(id_, SEL_, unsigned, id_))p_objc_msgSend;
    m1uI  = (int (*)(id_, SEL_, unsigned))p_objc_msgSend;
    mrect = (id_ (*)(id_, SEL_, Rect_))p_objc_msgSend;
    mtimer = (id_ (*)(id_, SEL_, double, id_, SEL_, id_, int))p_objc_msgSend;
    return 1;
}

static void *resolve_gl(void)
{
    void *h = dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES",
                     RTLD_NOW);
    if (!h) {
        const char *e = dlerror();
        w("[glapp] dlopen OpenGLES failed ["); w(e ? e : "(null)"); w("]\n");
        return 0;
    }
    p_glClearColor        = dlsym(h, "glClearColor");
    p_glClear             = dlsym(h, "glClear");
    p_glViewport          = dlsym(h, "glViewport");
    p_glColor4f           = dlsym(h, "glColor4f");
    p_glEnableClientState = dlsym(h, "glEnableClientState");
    p_glVertexPointer     = dlsym(h, "glVertexPointer");
    p_glDrawArrays        = dlsym(h, "glDrawArrays");
    p_glGetError          = dlsym(h, "glGetError");
    p_glMatrixMode        = dlsym(h, "glMatrixMode");
    p_glLoadIdentity      = dlsym(h, "glLoadIdentity");
    p_glOrthof            = dlsym(h, "glOrthof");
    p_glGenRenderbuffersOES = dlsym(h, "glGenRenderbuffersOES");
    p_glBindRenderbufferOES = dlsym(h, "glBindRenderbufferOES");
    p_glGenFramebuffersOES  = dlsym(h, "glGenFramebuffersOES");
    p_glBindFramebufferOES  = dlsym(h, "glBindFramebufferOES");
    p_glFramebufferRenderbufferOES = dlsym(h, "glFramebufferRenderbufferOES");
    p_glCheckFramebufferStatusOES  = dlsym(h, "glCheckFramebufferStatusOES");
    p_glGetRenderbufferParameterivOES =
        dlsym(h, "glGetRenderbufferParameterivOES");
    p_glGenTextures       = dlsym(h, "glGenTextures");
    p_glBindTexture       = dlsym(h, "glBindTexture");
    p_glTexImage2D        = dlsym(h, "glTexImage2D");
    p_glTexParameteri     = dlsym(h, "glTexParameteri");
    p_glTexParameteriv    = dlsym(h, "glTexParameteriv");
    p_glEnable            = dlsym(h, "glEnable");
    p_glDisable           = dlsym(h, "glDisable");
    p_glDrawTexfOES       = dlsym(h, "glDrawTexfOES");

    if (!p_glClear || !p_glDrawArrays || !p_glBindFramebufferOES ||
        !p_glBindRenderbufferOES) {
        w("[glapp] missing GL symbols\n");
        return 0;
    }
    return h;
}

static int make_classes(void)
{
    Class_ del, view;

    del = p_objc_allocateClassPair(C("NSObject"), "GLTestDelegate", 0);
    if (!del) { w("[glapp] allocateClassPair(delegate) failed\n"); return 0; }
    p_class_addMethod(del, S("applicationDidFinishLaunching:"),
                      (void *)app_did_finish, "v@:@");
    p_class_addMethod(del, S("tick:"), (void *)app_tick, "v@:@");
    p_objc_registerClassPair(del);

    view = p_objc_allocateClassPair(C("UIView"), "GLTestView", 0);
    if (!view) { w("[glapp] allocateClassPair(view) failed\n"); return 0; }
    /* +layerClass is a CLASS method: it goes on the metaclass. */
    p_class_addMethod(p_object_getClass(view), S("layerClass"),
                      (void *)view_layer_class, "#@:");
    p_objc_registerClassPair(view);
    return 1;
}

int main(void)
{
    static char arg0[] = "/Applications/GLTest.app/GLTest";
    static char *argv[2];
    int (*p_UIApplicationMain)(int, char **, id_, id_);
    void *uikit;
    int fd;

    /*
     * The shim writes its own trace with write(2) to fd 2, and an app launched
     * by SpringBoard has nowhere for that to go. Point fd 1 and 2 at a file so
     * the mbxshim lines -- which say which bind entry point fired and what
     * surface geometry CA handed over -- survive the launch.
     */
    fd = open("/tmp/glapp.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        dup2(fd, 1);
        dup2(fd, 2);
    }
    w("[glapp] start\n");

    if (!resolve_objc()) _exit(1);
    if (!dlopen("/System/Library/Frameworks/Foundation.framework/Foundation",
                RTLD_NOW)) { w("[glapp] no Foundation\n"); _exit(1); }
    {
        void *f = dlopen("/System/Library/Frameworks/Foundation.framework/"
                         "Foundation", RTLD_NOW);
        p_NSLog = dlsym(f, "NSLog");
    }
    uikit = dlopen("/System/Library/Frameworks/UIKit.framework/UIKit", RTLD_NOW);
    if (!uikit) { w("[glapp] no UIKit\n"); _exit(1); }
    if (!dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore",
                RTLD_NOW)) { w("[glapp] no QuartzCore\n"); _exit(1); }
    if (!resolve_gl()) _exit(1);
    if (!C("CAEAGLLayer")) { w("[glapp] no CAEAGLLayer class\n"); _exit(1); }
    if (!make_classes()) _exit(1);

    p_UIApplicationMain = dlsym(uikit, "UIApplicationMain");
    if (!p_UIApplicationMain) { w("[glapp] no UIApplicationMain\n"); _exit(1); }

    /* LC_UNIXTHREAD means no crt1, so argc/argv were never set up for us. */
    argv[0] = arg0;
    argv[1] = 0;

    w("[glapp] entering UIApplicationMain\n");
    p_UIApplicationMain(1, argv, (id_)0, nsstr("GLTestDelegate"));

    /* main must not return: with LC_UNIXTHREAD there is no caller to return
     * to, lr is whatever the kernel left behind. */
    w("[glapp] UIApplicationMain returned\n");
    _exit(0);
    return 0;
}
