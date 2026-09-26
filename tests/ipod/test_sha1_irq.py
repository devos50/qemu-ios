#!/usr/bin/env python3
"""SHA completion interrupts follow config bit 2, not SHA_INTENABLE alone."""
from pathlib import Path
import re, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'hw/arm/ipod_touch_sha1.c').read_text()
header=(root/'include/hw/arm/ipod_touch_sha1.h').read_text()
assert 'sha1_run(s, value & 0x4);' in source
state=re.search(r'typedef struct IPodTouchSHA1State.*?} IPodTouchSHA1State;',header,re.S)[0]
functions=[re.search(r'^#define ROTL.*?\n',source,re.M)[0]]
for name in ['sha1_trace','sha1_compress','sha1_publish','sha1_clear_irq','sha1_run']:
 functions.append(re.search(r'^(?:static )?(?:bool|void) '+name+r'\([^)]*\)\s*\{.*?^}',source,re.M|re.S)[0])
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int SysBusDevice, MemoryRegion, qemu_irq;
static int line;
static void qemu_irq_raise(qemu_irq irq) {line=1;}
static void qemu_irq_lower(qemu_irq irq) {line=0;}
static void cpu_physical_memory_read(uint64_t a,void *p,size_t n) {memset(p,0,n);}
#define g_malloc malloc
#define g_free free
'''+state+'\n'+'\n'.join(functions)+r'''
int main(void) {
 IPodTouchSHA1State s={0};
 s.irq=1;s.int_enable=1;
 /* A polled start (0x2/0xa) left over from an interrupt-driven job. */
 sha1_run(&s,false);assert(!s.int_status && !line);
 /* An interrupt-driven start (0x6/0xe). */
 sha1_run(&s,true);assert(s.int_status && line);
 sha1_clear_irq(&s);assert(!s.int_status && !line);
 s.int_enable=0;sha1_run(&s,true);assert(!s.int_status && !line);
 puts("PASS: SHA completion interrupt only for bit-2 starts with SHA_INTENABLE set");
}
'''
with tempfile.TemporaryDirectory(prefix='it-sha-irq-') as tmp:
 tmp=Path(tmp);(tmp/'check.c').write_text(code)
 subprocess.run(['cc','-fsanitize=address,undefined',str(tmp/'check.c'),'-o',str(tmp/'check')],check=True)
 subprocess.run([str(tmp/'check')],check=True)
