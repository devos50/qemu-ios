#!/usr/bin/env python3
"""Native guest probes for retired/unknown services; owns a disposable device."""
from pathlib import Path
from types import SimpleNamespace
import os
import subprocess
import tempfile
import time
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import regress as r

ROOT = Path(__file__).resolve().parents[2]
r.START = time.time()
class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        body=b'native-sdio-network'
        self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
    def log_message(self,*args): pass
server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
threading.Thread(target=server.serve_forever,daemon=True).start()
with tempfile.TemporaryDirectory(prefix='it-service-errors-') as temporary:
    out = Path(temporary)
    (out/'probe.c').write_text(r'''
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <string.h>
typedef struct __attribute__((packed)) {
 unsigned call; unsigned char args[32]; long long retval, error;
} Request;
_Static_assert(sizeof(Request)==52,"guest ABI");
static void invoke(volatile Request *request) {
 __asm__ __volatile__("mcr p15, 3, %0, c15, c15, 0" : : "r"(request) : "memory");
}
int main(void) {
 const unsigned calls[]={0x100,0x101,0x110,0x111,0x112,0x113,0x114,0x115,0x116,0x117,0x118,0x119,0xffffffff};
 for(unsigned i=0;i<sizeof(calls)/sizeof(calls[0]);i++) {
  volatile Request request={0};request.call=calls[i];
  for(unsigned n=0;n<32;n++)request.args[n]=0xff;
  request.retval=123;request.error=456;invoke(&request);
  if(request.retval!=-1 || request.error!=ENOSYS)_exit(1);
  request.call=0x141;invoke(&request);
  if(request.retval!=0x6a17c0deLL || request.error!=0)_exit(2);
 }
 /* The invalid continuation must discard all staging, not publish a prefix
  * or uninitialized host bytes. A fresh item must still work afterwards. */
 volatile Request pb={0};
 unsigned *args=(unsigned *)(void *)pb.args;
 const char text[]="service-check";
 for(unsigned i=0;i<2;i++) {
  pb.call=0x153;args[0]=(unsigned)text;args[1]=0;args[2]=sizeof(text)-1;
  invoke(&pb);if(pb.retval!=sizeof(text)-1 || pb.error)_exit(3);
  pb.call=0x154;invoke(&pb);if(pb.retval || pb.error)_exit(4);
  pb.call=0x153;args[1]=0;invoke(&pb);if(pb.retval<0)_exit(5);
  args[0]=i ? 0xfffffffe : 0;args[1]=sizeof(text)-1;args[2]=4;
  invoke(&pb);if(pb.retval!=-1 || pb.error!=EFAULT)_exit(6);
  pb.call=0x154;invoke(&pb);if(pb.retval!=-1 || pb.error!=EINVAL)_exit(7);
 }
 /* Native guest networking still uses the emulated SDIO interface. */
 int net=-1;
 for(unsigned attempt=0;attempt<30;attempt++) {
  net=socket(AF_INET,SOCK_STREAM,0);if(net<0)_exit(8);
  struct timeval timeout={2,0};setsockopt(net,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
  setsockopt(net,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
  struct sockaddr_in address={0};address.sin_len=sizeof(address);address.sin_family=AF_INET;
  address.sin_port=htons(@PORT@);address.sin_addr.s_addr=htonl(0x0a000202);
  if(!connect(net,(struct sockaddr *)&address,sizeof(address)))break;
  close(net);net=-1;sleep(1);
 }
 if(net<0)_exit(9);
 const char request[]="GET / HTTP/1.0\r\nHost: fixture\r\n\r\n";
 unsigned sent=0;while(sent<sizeof(request)-1) {
  int count=send(net,request+sent,sizeof(request)-1-sent,0);if(count<=0)_exit(10);sent+=count;
 }
 char response[1024];unsigned total=0;
 for(;;) {
  int count=recv(net,response+total,sizeof(response)-1-total,0);
  if(count<0)_exit(11);if(!count)break;total+=count;if(total==sizeof(response)-1)_exit(12);
 }
 close(net);response[total]=0;
 if(!strstr(response,"200 OK") || !strstr(response,"native-sdio-network"))_exit(13);
 const char message[]="PASS: retired FD/socket/file calls and unknown opcode return ENOSYS; next successful call clears error; native SDIO HTTP succeeds\n";
 write(1,message,sizeof(message)-1);
 _exit(0);
}
'''.replace('@PORT@',str(server.server_port)))
    env = os.environ.copy()
    env.setdefault('ARMV6_SDK',str(ROOT.parent/'ipod2g-re/OldSDK/iPhoneOS3.1.3.sdk'))
    subprocess.run(['bash','-c','''
set -eu
source "$1/contrib/armv6-toolchain/armv6.sh"
cc6 "$2/probe.c" "$2/probe.o"
link6 -execute "$2/probe" "$2/probe.o"
"${LDID:-ldid}" -S"$1/contrib/it-gles/sblaunch-entitlements.xml" "$2/probe"
''','service-test',str(ROOT),str(out)],env=env,check=True)
    files=str(ROOT.parent/'qemu-ios-files')
    cfg=SimpleNamespace(out=str(out),files=files,base_nand=files+'/nand-current',
        nor=files+'/ios3/nor_7E18.bin',overlay=str(out/'overlay'),
        qemu=str(ROOT/'build-native14/qemu-build/qemu-system-arm'),usbmuxd_ok=False,
        usb_port=r.free_port(1520,1539),qmp_port=r.free_port(28200,28219),
        wifi=True,cpu=None,mem='128M',kernel_console=True)
    procs=r.Procs();device=r.Device(cfg,procs,'device')
    try:
        device.start();ok,detail,_=device.wait_for_home(180);assert ok,detail
        q=device.qmp;q.cmd('qom-set',path='/machine',property='usb-attached',value=False)
        deadline=time.monotonic()+90
        while not r.itqmp.agent_alive(q):
            assert device.alive() and time.monotonic()<deadline
            time.sleep(1)
        assert r.itqmp.agent(q,'put','/tmp/service-probe 755',(out/'probe').read_bytes())==(0,b'')
        status,output=r.itqmp.agent(q,'exec','/tmp/service-probe')
        assert status==0,(status,output)
        assert q.cmd('qom-get',path='/machine',property='guest-pasteboard')=='service-check'
        print('PASS: unmapped/wrapping clipboard buffers discard staging, preserve clipboard and allow recovery',flush=True)
        print(output.decode(),end='',flush=True)
        assert r.itqmp.agent(q,'ping')==(0,b'it_agent v1\n')
        assert device.powerdown()
    finally:
        if device.qmp:device.qmp.close()
        procs.stop_all()
        server.shutdown();server.server_close()
