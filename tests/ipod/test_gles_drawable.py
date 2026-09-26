#!/usr/bin/env python3
"""Exercise the guest CA drawable lifecycle with real shim functions."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'contrib/it-gles/mbxshim.c').read_text()
start = source.rfind('typedef struct {', 0, source.index('} ca_view_t;'))
views = source[start:source.index('static void *iosurf;')]
callbacks = source[source.index('typedef int (*ca_bind_fn)'):source.index('/* A mapped IOSurface')]
bind = source[source.index('static int GLESBindView('):source.index('/* 7E18 0x1d020')]
preamble = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#define GLES_OP_DELETE_CONTEXT 0x1007
#define GLES_OP_DRAWABLE_STORAGE 0x1008
#define CA_FOURCC_BGRA 0x42475241
#define A(...) (const unsigned[]){__VA_ARGS__}
typedef struct { unsigned host; } GuestGC;
static unsigned deleted;
static long long qc(unsigned op,void *gc,unsigned argc,const unsigned *args)
{ if(op==GLES_OP_DRAWABLE_STORAGE) { assert(argc==2 && args[0] && args[1]);return 0; }
  assert(op==GLES_OP_DELETE_CONTEXT);deleted++;return 0; }
static void iosurface_init(void) {}
static void w(const char *s) {}
static void wx(unsigned long x) {}
static void wd(unsigned x) {}
'''
mock_surface = r'''
static int surface_capture(ca_view_t *v, void *surface)
{
    unsigned *geometry=surface;
    assert(v && v->gc);
    v->ref=surface;v->base=0x1000;v->stride=geometry[0]*4;
    v->width=geometry[0];v->height=geometry[1];return 1;
}
'''
check = r'''
typedef struct {
    void *vt[5];
    void **block;
    unsigned geometry[2];
    unsigned binds,unbinds,presents,created;
    int refuse,empty;
} Drawable;
static int bind_drawable(void *p,unsigned format,void **block)
{
    Drawable *d=p;d->binds++;
    assert(format==CA_FOURCC_BGRA);
    /* This is CA's observed refusal when the previous binding is retained. */
    if(d->block || d->refuse)return 0;
    assert(ca_view_for_block(block));
    d->block=block;d->created=0;return 1;
}
static int unbind_drawable(void *p)
{
    Drawable *d=p;assert(d->block);d->unbinds++;
    if(d->created) {
        ca_view_t *v=ca_view_for_block(d->block);
        assert(v && v->ref==d->geometry);
        assert(((int (*)(void *,void *))d->block[2])(d->block,d->geometry));
        assert(!v->ref && !v->base);
    }
    d->block=NULL;return 1;
}
static void *next_drawable(void *p)
{
    Drawable *d=p;assert(d->block);
    if(d->empty)return NULL;
    if(!d->created) {
        assert(((int (*)(void *,void *))d->block[1])(d->block,d->geometry));
        d->created=1;
    }
    return d->geometry;
}
static int present_drawable(void *p,unsigned n)
{ Drawable *d=p;assert(d->block && n==1);d->presents++;return 1; }
static Drawable drawable(unsigned width,unsigned height)
{
    Drawable d={.vt={NULL,bind_drawable,unbind_drawable,next_drawable,present_drawable},
                .geometry={width,height}};
    return d;
}
int main(void)
{
    GuestGC *one=calloc(1,sizeof(*one)), *two=calloc(1,sizeof(*two));
    one->host=1;two->host=2;
    Drawable portrait=drawable(320,480), other=drawable(320,480), refused=drawable(480,320);
    assert(GLESBindView(one,&portrait,(void *)0x8058,NULL));
    assert(GLESBindView(two,&other,(void *)0x8058,NULL));
    ca_view_t *a=ca_view_for_gc(one,0),*b=ca_view_for_gc(two,0);
    assert(a!=b && a->width==320 && a->height==480);
    /* Diner's layoutSubviews destroys/recreates storage with new dimensions. */
    portrait.geometry[0]=480;portrait.geometry[1]=320;
    assert(GLESBindView(one,&portrait,(void *)0x8058,NULL));
    assert(portrait.binds==2 && portrait.unbinds==1 && portrait.presents==1);
    assert(a->width==480 && a->height==320 && a->stride==1920);
    assert(b->drawable==&other && other.binds==1 && !other.unbinds);
    /* No extra present when the previous frame was already submitted. */
    a->need_buffer=1;
    assert(GLESBindView(one,&portrait,(void *)0x8058,NULL));
    assert(portrait.presents==1 && portrait.unbinds==2);
    /* A rejected replacement must not keep freed surface metadata. */
    refused.refuse=1;
    assert(!GLESBindView(one,&refused,(void *)0x8058,NULL));
    assert(!ca_view_for_gc(one,0) && !a->base && !a->ref);
    assert(!refused.unbinds && !other.unbinds);
    /* Accepted binding with no usable surface must be released. */
    refused.refuse=0;refused.empty=1;
    assert(!GLESBindView(one,&refused,(void *)0x8058,NULL));
    assert(refused.unbinds==1 && !ca_view_for_gc(one,0));
    assert(GLESBindView(one,NULL,NULL,NULL));
    assert(GLESBindView(two,NULL,NULL,NULL));
    assert(other.unbinds==1 && !ca_view_for_gc(two,0));
    assert(GLESBindView(one,&portrait,(void *)0x8058,NULL));
    GLESDestroyGC(one);GLESDestroyGC(two);
    assert(!portrait.block && deleted==2);
    for(unsigned i=0;i<CA_MAX_VIEWS;i++)assert(!ca_views[i].gc);
    puts("CA drawable lifecycle: passed");
    return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    code=Path(tmp)/'drawable.c';exe=Path(tmp)/'drawable'
    code.write_text(preamble+views+mock_surface+callbacks+bind+check)
    subprocess.run(['cc','-Wall','-Wextra','-Wno-unused-parameter','-Wno-unused-variable',str(code),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
