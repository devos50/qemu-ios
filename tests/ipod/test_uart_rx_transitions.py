#!/usr/bin/env python3
"""Exercise the production IRQ mapping, timeout, and DMA mode transitions."""
from pathlib import Path
import re,subprocess,tempfile
s=(Path(__file__).resolve().parents[2] / 'hw/char/exynos4210_uart.c').read_text()
def fn(name):
 start=s.index('static ',s.index(name)-35); a=s.index('{',s.index(name)); n=1;b=a+1
 while n:
  n+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[start:b]
names=['exynos4210_uart_update_dmabusy','exynos4210_uart_timeout_int','exynos4210_uart_rx_timeout_set','exynos4210_uart_write','exynos4210_uart_receive']
fns=[fn(x) for x in names]
irq_fns=[fn(x) for x in ['exynos4210_uart_FIFO_trigger_level','exynos4210_uart_Tx_FIFO_trigger_level','exynos4210_uart_Rx_FIFO_trigger_level','exynos4210_uart_update_irq']]
defs='\n'.join(x for x in s.splitlines() if x.startswith('#define ') and '\\' not in x)
traces='\n'.join('#define '+x+'(...) ((void)0)' for x in set(re.findall(r'trace_\w+', '\n'.join(fns+irq_fns))))
code='''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
typedef uint64_t hwaddr;
typedef struct {unsigned size,sp,rp;} FIFO;
typedef struct {uint32_t reg[32]; FIFO rx,tx; unsigned channel; bool s5l8720_irq,rx_since_timeout; int dmairq,rxdmareq,irq,chr; int *fifo_timeout_timer; uint32_t wordtime;} Exynos4210UartState;
static unsigned levels[4],pulses;
#define QEMU_CLOCK_VIRTUAL 0
static int64_t qemu_clock_get_ns(int c){return 100;}
static void timer_del(int *t){*t=0;}
static void timer_mod(int *t,int64_t deadline){*t=1;}
static void qemu_set_irq(int irq,int level){levels[irq]=level;}
static void qemu_irq_raise(int irq){levels[irq]=1;}
static void qemu_irq_lower(int irq){levels[irq]=0;}
static void qemu_irq_pulse(int irq){pulses++;}
static unsigned fifo_elements_number(FIFO *f){return(f->sp+f->size-f->rp)%f->size;}
static unsigned fifo_empty_elements_number(FIFO *f){return f->size-fifo_elements_number(f);}
static void fifo_store(FIFO *f,uint8_t b){f->sp=(f->sp+1)%f->size;}
static void fifo_reset(FIFO *f){f->sp=f->rp=0;}
#define bt_record(...) ((void)0)
#define qemu_chr_fe_backend_connected(...) 0
#define qemu_chr_fe_write_all(...) ((void)0)
#define exynos4210_uart_update_parameters(...) ((void)0)
static void exynos4210_uart_update_irq(Exynos4210UartState *s);
'''+defs+'\n'+traces+'\n'+fns[0]+'\n'+'\n'.join(irq_fns)+'\n'+fns[1]+'\n'+fns[2]+'\n'+fns[3]+'\n'+fns[4]+'''
int main(void){
 int timer=1;
 Exynos4210UartState s={.rx={256,7,0},.tx={256,0,0},.channel=1,.s5l8720_irq=true,.rx_since_timeout=true,.dmairq=0,.rxdmareq=1,.irq=3,.fifo_timeout_timer=&timer};
 s.reg[I_(UFCON)]=1;s.reg[I_(UCON)]=0x115c80;s.reg[I_(UTRSTAT)]=7;
 exynos4210_uart_write(&s,UCON,0x115c8f,4);
 assert(levels[1]==1);
 exynos4210_uart_update_dmabusy(&s);
 exynos4210_uart_write(&s,UCON,0x115c80,4);
 assert(levels[1]==0 && timer==0);
 s.reg[I_(UCON)]=0x115c8f; timer=1;
 exynos4210_uart_write(&s,UFCON,3,4);
 assert(!fifo_elements_number(&s.rx) && !levels[1]);
 assert(!(s.reg[I_(UTRSTAT)] & (UTRSTAT_Rx_BUFFER_DATA_READY|UTRSTAT_Rx_TIMEOUT)));
 assert(!s.rx_since_timeout && !timer);
 exynos4210_uart_timeout_int(&s);
 assert(!pulses);
 s.reg[I_(UCON)]=0x115c80;
 uint8_t reply[7]={4,14,4,1,24,252,0};
 exynos4210_uart_receive(&s,reply,sizeof(reply));
 assert(!timer && s.rx_since_timeout && fifo_elements_number(&s.rx)==7);
 assert(!levels[1]);
 exynos4210_uart_write(&s,UCON,0x115c8f,4);
 assert(timer && levels[1] && s.rx_since_timeout);
 exynos4210_uart_timeout_int(&s);
 assert(pulses==0 && !s.rx_since_timeout);
 /* An Rx timeout must survive update_irq and reach the timeout handler (8),
  * never masquerade as transmit (32) or automatic baud measurement (256). */
 assert(s.reg[I_(UTRSTAT)] & 8);
 assert(!(s.reg[I_(UTRSTAT)] & 0x100));
 assert(levels[3]);
 s.rx.rp=s.rx.sp; /* DMA drained the seven bytes; descriptor is still active. */
 s.reg[I_(UTRSTAT)] &= ~1;
 exynos4210_uart_write(&s,UTRSTAT,0x18,4);
 assert(!(s.reg[I_(UTRSTAT)] & 0x18) && !levels[3]);
 exynos4210_uart_timeout_int(&s);
 assert(!pulses);
 exynos4210_uart_write(&s,UTRSTAT,0x18,4);
 /* Pending receive threshold is 0x10, enabled by UCON bit 12. */
 s.reg[I_(UINTSP)]=UINTSP_RXD;
 exynos4210_uart_write(&s,UCON,0x1005,4);
 assert(levels[3] && (s.reg[I_(UTRSTAT)] & 0x10));
 assert(!(s.reg[I_(UTRSTAT)] & (0x100|0x20|8)));
 exynos4210_uart_write(&s,UCON,5,4);
 assert(!levels[3] && (s.reg[I_(UTRSTAT)] & 0x10));
 exynos4210_uart_write(&s,UTRSTAT,0x10,4);
 /* A transmitted byte cannot masquerade as a receive timeout. */
 s.reg[I_(UINTSP)]=UINTSP_TXD;
 exynos4210_uart_write(&s,UCON,0x2005,4);
 assert(levels[3] && (s.reg[I_(UTRSTAT)] & 0x20));
 assert(!(s.reg[I_(UTRSTAT)] & (0x100|0x10|8)));
 exynos4210_uart_write(&s,UTRSTAT,0x20,4);
 assert(!levels[3]);
 /* Error and timeout have separate enables; ack must drop the line. */
 s.reg[I_(UINTSP)]=UINTSP_ERROR;
 exynos4210_uart_write(&s,UCON,5,4);assert(!levels[3]);
 exynos4210_uart_write(&s,UCON,0x4005,4);assert(levels[3]);
 exynos4210_uart_write(&s,UTRSTAT,0x40,4);assert(!levels[3]);
 s.reg[I_(UTRSTAT)] |= 8;
 exynos4210_uart_write(&s,UCON,5,4);assert(!levels[3]);
 exynos4210_uart_write(&s,UCON,0x805,4);assert(levels[3]);
 exynos4210_uart_write(&s,UTRSTAT,8,4);assert(!levels[3]);
 /* Standard Exynos UINTP remains independent of the S5L UCON mask. */
 s.s5l8720_irq=false;s.reg[I_(UFCON)]=0;s.reg[I_(UCON)]=5;
 s.reg[I_(UINTSP)]=UINTSP_RXD;exynos4210_uart_update_irq(&s);
 assert(levels[3]);
 exynos4210_uart_write(&s,UINTP,UINTSP_RXD,4);assert(!levels[3]);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'check.c';p.write_text(code)
 subprocess.run(['clang','-fsanitize=address,undefined',str(p),'-o',d+'/check'],check=True)
 subprocess.run([d+'/check'],check=True)

print("PASS: S5L receive/timeout/transmit/error mapping, enables, acknowledgments, and DMA transitions")
