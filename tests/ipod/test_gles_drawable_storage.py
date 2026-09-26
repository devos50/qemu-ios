#!/usr/bin/env python3
"""Production CA drawable resizing, dimensions and full writeback on native CGL."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
s = (root / 'hw/arm/gles-host.c').read_text()
def function(name):
    start = s.index('static ', s.index(name) - 50)
    start = s.rfind('\nstatic ', 0, s.index(name)) + 1
    return s[start:s.index('\n}', s.index(name)) + 2]
query = s[s.index('    case GLES_SLOT_GET_RB_PARAMETERIV:'):s.index('    case GLES_SLOT_GET_FB_ATTACH_PARAM:')]
code = r'''
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define GLES_SLOT_GET_RB_PARAMETERIV 670
#define GLES_SURFACE_BGRA32 0x42475241
#define GLES_SURFACE_RGBA32 0x52474241
#define GLES_SURFACE_RGB565 0x4c353635
#define GLES_BGRA_READ_TYPE GL_UNSIGNED_INT_8_8_8_8_REV
#define GLES_FB_WIDTH 320
#define GLES_FB_HEIGHT 480
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
typedef struct { int unused; } CPUState;
typedef uint64_t hwaddr;
static struct {
 GLuint fbo,tex,depth;
 uint32_t drawable_width,drawable_height,bound_framebuffer,bound_renderbuffer;
 uint8_t *readback;
 GHashTable *rb_sized;
 GLenum error;
 bool iosurface,fb_dirty,depth_cleared_this_frame,drawable_announced;
 uint64_t presents;
} gh;
#define RAM_BASE 0x10000000
static uint8_t ram[8*1024*1024];
static int cpu_memory_rw_debug(CPUState *cpu,uint64_t address,uint8_t *p,size_t n,int write)
{
 if(address<RAM_BASE || address+n>RAM_BASE+sizeof(ram))return -1;
 if(write)memcpy(ram+(address-RAM_BASE),p,n);else memcpy(p,ram+(address-RAM_BASE),n);
 return 0;
}
static int64_t gles_reject(GLenum error) {if(!gh.error)gh.error=error;return -1;}
static GLuint gles_host_fbo(uint32_t name) {return name?name:gh.fbo;}
static int gles_swizzle;
static const uint8_t *gles_frame_lock(size_t *stride) {return NULL;}
static void gles_platform_frame_unlock(void) {}
static void gles_dump_frame(const uint8_t *p,size_t stride,unsigned w,unsigned h,bool bgra) {}
static void gles_frame_end(void) {}
static void gles_note_scene(void) {}
static void gles_note_frame_gap(void) {}
static void gles_report_progress(void) {}
''' + function('gles_texture_begin(') + function('gles_drawable_storage(') + function('gles_present_to_surface(') + r'''
static GLint dimension(GLenum pname)
{
 uint32_t a[]={GL_RENDERBUFFER_EXT,pname,RAM_BASE};
 CPUState *cpu=NULL;
 switch(GLES_SLOT_GET_RB_PARAMETERIV) {
''' + query.replace('        return 0;','        return *(GLint *)ram;') + r'''
 }
 return -1;
}
static void check_pixels(unsigned width,unsigned height,unsigned format)
{
 unsigned bpp=format==GLES_SURFACE_RGB565?2:4;
 unsigned stride=width*bpp+12;
 memset(ram,0xa5,sizeof(ram));
 glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,gh.fbo);
 glDisable(GL_SCISSOR_TEST);glClearColor(1,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
 /* Distinct pixels at the far right and the top catch both crop and Y inversion. */
 glEnable(GL_SCISSOR_TEST);glScissor(width-1,0,1,height);
 glClearColor(0,1,0,1);glClear(GL_COLOR_BUFFER_BIT);
 glScissor(0,height-1,width,1);glClearColor(0,0,1,1);glClear(GL_COLOR_BUFFER_BIT);
 glDisable(GL_SCISSOR_TEST);
 glPixelStorei(GL_PACK_ALIGNMENT,8);
 assert(!gles_present_to_surface(NULL,RAM_BASE,stride,width,height,format));
 GLint alignment;glGetIntegerv(GL_PACK_ALIGNMENT,&alignment);assert(alignment==8);
 for(unsigned y=0;y<height;y++) {
  for(unsigned x=0;x<width;x++) {
   if(bpp==4) {
    uint8_t expected[4]={0,0,0,255};
    unsigned channel=y==0?2:x==width-1?1:0;
    if(format==GLES_SURFACE_BGRA32)channel=2-channel;
    expected[channel]=255;
    assert(!memcmp(ram+y*stride+x*bpp,expected,4));
   } else {
    uint16_t expected=y==0?0x001f:x==width-1?0x07e0:0xf800;
    assert(!memcmp(ram+y*stride+x*bpp,&expected,2));
   }
  }
  for(unsigned p=width*bpp;p<stride;p++)assert(ram[y*stride+p]==0xa5);
 }
 assert(ram[height*stride]==0xa5);
 assert(glGetError()==GL_NO_ERROR);
}
int main(void)
{
 CGLPixelFormatAttribute attrs[]={kCGLPFAAccelerated,0};
 CGLPixelFormatObj pf;GLint count;CGLContextObj context;
 assert(CGLChoosePixelFormat(attrs,&pf,&count)==kCGLNoError);
 assert(CGLCreateContext(pf,NULL,&context)==kCGLNoError);CGLDestroyPixelFormat(pf);
 assert(CGLSetCurrentContext(context)==kCGLNoError);
 gh.rb_sized=g_hash_table_new(g_direct_hash,g_direct_equal);
 GLuint texture,renderbuffer,offscreen;
 glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
 glGenRenderbuffersEXT(1,&renderbuffer);glBindRenderbufferEXT(GL_RENDERBUFFER_EXT,renderbuffer);
 glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT,GL_RGBA8,17,23);
 gh.bound_renderbuffer=renderbuffer;
 glGenFramebuffersEXT(1,&offscreen);glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,offscreen);
 gh.bound_framebuffer=offscreen;glViewport(3,5,71,93);
 unsigned sizes[][2]={{320,480},{480,320},{321,201},{320,480}};
 for(unsigned n=0;n<ARRAY_SIZE(sizes);n++) {
  unsigned width=sizes[n][0],height=sizes[n][1];
  assert(!gles_drawable_storage(width,height));
  GLint value,viewport[4];
  glGetIntegerv(GL_TEXTURE_BINDING_2D,&value);assert(value==texture);
  glGetIntegerv(GL_RENDERBUFFER_BINDING_EXT,&value);assert(value==renderbuffer);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT,&value);assert(value==offscreen);
  glGetIntegerv(GL_VIEWPORT,viewport);assert(viewport[0]==3 && viewport[1]==5 && viewport[2]==71 && viewport[3]==93);
  assert(dimension(GL_RENDERBUFFER_WIDTH_EXT)==width);
  assert(dimension(GL_RENDERBUFFER_HEIGHT_EXT)==height);
  g_hash_table_add(gh.rb_sized,GUINT_TO_POINTER(renderbuffer));
  assert(dimension(GL_RENDERBUFFER_WIDTH_EXT)==17 && dimension(GL_RENDERBUFFER_HEIGHT_EXT)==23);
  g_hash_table_remove(gh.rb_sized,GUINT_TO_POINTER(renderbuffer));
  check_pixels(width,height,GLES_SURFACE_BGRA32);
  check_pixels(width,height,GLES_SURFACE_RGBA32);
  check_pixels(width,height,GLES_SURFACE_RGB565);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT,&value);assert(value==offscreen);
  GLuint fbo=gh.fbo;
  assert(!gles_drawable_storage(width,height) && gh.fbo==fbo);
  assert(gles_drawable_storage(0,height)<0 && gh.fbo==fbo);
  assert(gles_drawable_storage(UINT32_MAX,height)<0 && gh.fbo==fbo);
  assert(gh.error==GL_INVALID_VALUE);gh.error=0;
 }
 /* A shim that never announces storage keeps the legacy panel-sized crop. */
 gh.drawable_announced=false;
 assert(!gles_present_to_surface(NULL,RAM_BASE,240*4,240,360,GLES_SURFACE_BGRA32));
 gh.drawable_announced=true;
 /* A resize while the actual drawable is bound rebinds its replacement. */
 glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,gh.fbo);gh.bound_framebuffer=0;
 assert(!gles_drawable_storage(480,320));
 GLint value;glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT,&value);assert(value==gh.fbo);
 assert(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT)==GL_FRAMEBUFFER_COMPLETE_EXT);
 assert(gles_present_to_surface(NULL,RAM_BASE,320*4,320,480,GLES_SURFACE_BGRA32)<0);
 glDeleteTextures(1,&gh.tex);glDeleteRenderbuffersEXT(1,&gh.depth);glDeleteFramebuffersEXT(1,&gh.fbo);
 glDeleteTextures(1,&texture);glDeleteRenderbuffersEXT(1,&renderbuffer);glDeleteFramebuffersEXT(1,&offscreen);
 g_free(gh.readback);g_hash_table_destroy(gh.rb_sized);
 CGLSetCurrentContext(NULL);CGLDestroyContext(context);
 puts("PASS: CA portrait/landscape/odd-sized storage, renderbuffer sizes, full writeback and GL state preservation");
}
'''
with tempfile.TemporaryDirectory(prefix='it-gles-drawable-storage-') as tmp:
    path=Path(tmp)/'check.c';exe=Path(tmp)/'check';path.write_text(code)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'],text=True).split()
    subprocess.run(['clang','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(path),'-o',str(exe),*flags,'-framework','OpenGL'],check=True)
    subprocess.run([str(exe)],check=True)
