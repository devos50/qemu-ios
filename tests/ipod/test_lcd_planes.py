#!/usr/bin/env python3
"""Exercise the actual LCD plane compositor with bounded DMA under sanitizers."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
s = (root / 'hw/arm/ipod_touch_lcd.c').read_text()
s = s[s.index('static bool lcd_plane_range('):s.index('/*\n * Blit the guest\'s portrait framebuffer')]
header = r'''
#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#define LCD_FB_WIDTH 320
#define LCD_FB_HEIGHT 480
#define BASE 0x0f600000
static uint8_t ram[1024*1024];
static void cpu_physical_memory_read(uint64_t a,void *p,size_t n)
{ assert(a>=BASE && a+n<=BASE+sizeof(ram));memcpy(p,ram+(a-BASE),n); }
'''
check = r'''
int main(void)
{
    /* Program a linear kernel through the actual packed half-phase banks.
     * Check both mirrored phases and edge extension independently of scanout. */
    uint32_t kernel[36]={0};
    for (unsigned p=0;p<=8;p++) {
        kernel[p*4+1]=128-p*8;
        kernel[p*4+2]=(p*8)<<16;
    }
    uint8_t ramp[]={0,64,128,192}, resized[16];
    uint32_t vertical[18]={0};
    for (unsigned p=0;p<=8;p++) {
        vertical[p*2]=128-p*8;
        vertical[p*2+1]=(p*8)<<16;
    }
    assert(lcd_filter_sample(ramp,4,1,0xf000,kernel,8)==60);
    assert(lcd_filter_sample(ramp,4,1,0x3f000,kernel,8)==192);
    assert(lcd_filter_tap((uint32_t[]){0x0fff0000},8,0,0)==-1);
    lcd_filter_plane(ramp,4,1,4,1,resized,8,2,0x8000,0x8000,0,0,kernel,8,vertical);
    uint8_t expected[]={0,32,64,96,128,160,192,192};
    assert(!memcmp(resized,expected,8) && !memcmp(resized+8,expected,8));
    uint32_t r[0x300/4]={0};uint8_t out[320*480*4];
    r[0x2c0/4]=r[0x2c4/4]=0x10000;
    r[1]=0x108;r[0x134/4]=(4<<16)|2;r[0x13c/4]=(4<<16)|2;
    r[0x2e0/4]=r[0x2e4/4]=6<<17;r[0x11c/4]=BASE;r[0x120/4]=BASE+256;
    unsigned matrix[]={1192,0,1634,1192,0x1191,0x1340,1192,2066,0};
    memcpy(r+0x70/4,matrix,sizeof(matrix));
    memset(ram,0xa5,sizeof(ram));
    uint8_t y[]={16,235,81,81};memcpy(ram,y,4);memcpy(ram+6,y,4);
    uint8_t uv[]={128,128,90,240};memcpy(ram+256,uv,4);
    assert(lcd_compose_planes(r,out));
    assert(!memcmp(out,"\0\0\0\xff\xff\xff\xff\xff\0\0\xfe\xff",12));
    r[0x118/4]=3;r[0x13c/4]=(2<<16)|4;
    assert(lcd_compose_planes(r,out));
    assert(!memcmp(out,"\0\0\0\xff",4));
    assert(!memcmp(out+3*320*4,"\0\0\xfe\xff",4));
    /* The controls surface is premultiplied BGRA over the video plane. */
    r[1]|=0x20;r[0x40/4]=0x310700;r[0x44/4]=BASE+512;
    r[0x48/4]=2;r[0x50/4]=(2<<16)|4;
    memset(ram+512,0,32);ram[512+3*8]=128;ram[515+3*8]=128;
    assert(lcd_compose_planes(r,out));assert(!memcmp(out+3*320*4,"\x80\0\x7f\xff",4));
    /* Full-range luma and a destination clipped by the panel edges. */
    r[1]=8;r[0x118/4]=0;r[0x13c/4]=(4<<16)|2;
    r[0x138/4]=(319<<16)|479;
    memset(r+0x70/4,0,9*4);r[0x70/4]=r[0x7c/4]=r[0x88/4]=1024;
    ram[0]=173;assert(lcd_compose_planes(r,out));
    assert(!memcmp(out+(479*320+319)*4,"\xad\xad\xad\xff",4));
    /* 2x scaled, rotated full-range luma through all four filter banks. */
    r[0x138/4]=0;r[0x118/4]=3;r[0x13c/4]=(4<<16)|8;
    r[0x2c0/4]=r[0x2c4/4]=0x8000;
    memcpy(r+0x140/4,kernel,sizeof(kernel));
    memcpy(r+0x1d0/4,vertical,sizeof(vertical));
    memcpy(r+0x220/4,vertical,sizeof(vertical));
    memcpy(r+0x270/4,vertical,sizeof(vertical));
    memcpy(ram,ramp,4);memcpy(ram+6,ramp,4);
    assert(lcd_compose_planes(r,out));
    for (unsigned row=0;row<8;row++)
        for (unsigned col=0;col<4;col++)
            assert(out[(row*320+col)*4]==expected[row]);
    /* Crop on an odd luma row: chroma begins half a source row later. */
    r[0x130/4]=1;r[0x134/4]=(4<<16)|3;r[0x13c/4]=(6<<16)|8;
    memset(r+0x70/4,0,9*4);r[0x74/4]=r[0x80/4]=r[0x8c/4]=1024;
    memset(ram+256,128,4);memset(ram+262,192,4);
    assert(lcd_compose_planes(r,out));
    assert(out[0]==64 && out[5*4]==32); /* reversed vertical rotation */
    r[0x130/4]=(1<<16)|1;r[0x134/4]=(3<<16)|3;
    r[0x118/4]=0;r[0x13c/4]=(6<<16)|6;
    uint8_t chromaRamp[]={128,128,192,128};
    memcpy(ram+256,chromaRamp,4);memcpy(ram+262,chromaRamp,4);
    assert(lcd_compose_planes(r,out));
    assert(out[0]==32 && out[5*4]==64);
    r[0x130/4]=0xffff0000;assert(!lcd_compose_planes(r,out));
    r[0x130/4]=0;r[0x134/4]=(4<<16)|2;
    r[0x2c0/4]=r[0x2c4/4]=0x10000;
    memset(out,0,sizeof(out));
    assert(!out[0]);r[0x138/4]=0;r[0x118/4]=3;r[0x13c/4]=(2<<16)|4;r[1]=0x128;
    /* Reject malformed source ranges and unsupported resizing before DMA. */
    r[0x11c/4]=0xfffffff0;assert(!lcd_compose_planes(r,out));
    r[0x11c/4]=BASE;r[0x134/4]=(4<<16);assert(!lcd_compose_planes(r,out));
    r[0x134/4]=(4<<16)|2;r[0x13c/4]=(3<<16)|4;assert(!lcd_compose_planes(r,out));
    r[0x13c/4]=(2<<16)|4;r[0x48/4]=0x40000002;assert(!lcd_compose_planes(r,out));
    /* Exact RGB0 geometry observed on Diner's accepted landscape drawable:
     * 426x320, 432-pixel stride, mode 3, at (0,27). No second plane is enabled.
     * Unique X/Y channels detect row skew, transpose, mirroring and cropping. */
    memset(r,0,sizeof(r));r[1]=0x10;r[0x20/4]=0x00e00700;
    r[0x24/4]=BASE;r[0x28/4]=432;r[0x30/4]=(426<<16)|320;r[0x34/4]=27;
    memset(ram,0xa5,sizeof(ram));
    for(unsigned y=0;y<320;y++) for(unsigned x=0;x<426;x++) {
        uint8_t *p=ram+(y*432+x)*4;
        p[0]=x&255;p[1]=y&255;p[2]=x>>8;p[3]=255;
    }
    assert(lcd_needs_plane_composition(r));assert(lcd_compose_planes(r,out));
    for(unsigned y=0;y<480;y++) for(unsigned x=0;x<320;x++) {
        uint8_t expected[4]={0};
        if(y>=27 && y<453) {
            unsigned sx=y-27,sy=319-x;
            expected[0]=sx&255;expected[1]=sy&255;expected[2]=sx>>8;expected[3]=255;
        }
        assert(!memcmp(out+(y*320+x)*4,expected,4));
    }
    /* Preserve the common dirty-tracked portrait path; route padded or
     * positioned RGB0 through composition even without an overlay. */
    r[0x20/4]=0x00200700;r[0x28/4]=320;r[0x30/4]=(320<<16)|480;r[0x34/4]=0;
    assert(!lcd_needs_plane_composition(r));
    r[0x28/4]=321;assert(lcd_needs_plane_composition(r));
    r[0x28/4]=320;r[0x34/4]=1;assert(lcd_needs_plane_composition(r));
    r[0x34/4]=0;r[1]=0;assert(!lcd_needs_plane_composition(r));
    r[1]=0x10;r[0x20/4]=0x00600700;assert(!lcd_compose_planes(r,out));
    r[0x20/4]=0x00e00f00;assert(!lcd_compose_planes(r,out));
    r[0x20/4]=0x00e00700;r[0x24/4]=0x0fffffc0;assert(!lcd_compose_planes(r,out));
    r[0x24/4]=BASE;r[0x28/4]=0x40000140;assert(!lcd_compose_planes(r,out));
    puts("PASS: LCD RGB0 padded landscape scanout, NV12 rotation, overlays, bounded DMA");
}
'''
with tempfile.TemporaryDirectory(prefix='it-lcd-planes-') as tmp:
    c=Path(tmp)/'check.c';exe=Path(tmp)/'check';c.write_text(header+s+check)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'],text=True).split()
    subprocess.run(['clang','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe),*flags],check=True)
    subprocess.run([str(exe)],check=True)
