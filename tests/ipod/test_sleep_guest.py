#!/usr/bin/env python3
"""Native untethered lock/Home wake without brightness or watchdog overrides."""
import argparse,os,sys,tempfile,time
from PIL import Image
from pathlib import Path
from types import SimpleNamespace
sys.path.insert(0,str(Path(__file__).resolve().parent))
import regress as r
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--automatic',action='store_true',help='set Auto-Lock to one minute in Settings and wake with Power')
args=parser.parse_args()
root=Path(__file__).resolve().parents[2];files=str(root.parent/'qemu-ios-files')
out=tempfile.mkdtemp(prefix='it-idle-wake-')
cfg=SimpleNamespace(out=out,files=files,base_nand=files+'/nand-current',nor=files+'/ios3/nor_7E18.bin',overlay=out+'/overlay',qemu=str(root/'build-native14/qemu-build/qemu-system-arm'),usbmuxd=str(root/"build-native14/build/usbmuxd/src/usbmuxd"),usbmuxd_ok=True,mux_port=r.free_port(27400,27419),usb_port=r.free_port(1520,1539),qmp_port=r.free_port(28200,28219),wifi=False,cpu=None,mem='128M',kernel_console=True)
class Procs(r.Procs):
 def spawn(self,argv,logpath,env=None):
  if argv[0]==cfg.qemu:
   env=dict(env)
   for name in ('IT_LCD_BRIGHT','IT_WDT_NORESET'):env.pop(name,None)
   env.update(IT_LCD_PLANES='1',IT_SCALER_DECODE='1',IT_AMC_DECODE='1',IT_MPVD_DECODE='1',IT_H264_DECODE='1')
  return super().spawn(argv,logpath,env)
p=Procs();d=r.Device(cfg,p,'device');r.START=time.time()
print('OUTPUT',out,flush=True)
def rpc(op,arg=''):
 result=r.itqmp.agent(d.qmp,op,arg);assert result[0]==0,result;return result[1]
def capture(label):
 print(label,d.qmp.cmd('human-monitor-command',**{'command-line':'info registers'}),flush=True)
 r.to_png(d.qmp.shot(out+'/'+label+'.ppm'),out+'/'+label+'.png')
 with Image.open(out+'/'+label+'.png') as image:
  brightest=max(high for low,high in image.convert('RGB').getextrema())
  if label in ('awake','wake'): assert brightest>100,(label,brightest)
  elif not (args.automatic and label=='idle60'): assert brightest==0,(label,brightest)
try:
 d.start()
 deadline=time.monotonic()+90
 while not r.itqmp.agent_alive(d.qmp):
  assert time.monotonic()<deadline;time.sleep(1)
 r.itqmp.button(d.qmp,'home',120)
 time.sleep(2)
 control=r.prepare_app_control(cfg,p,d,r.Result('control'));ok,detail=r.unlock(cfg,control,d);assert ok,detail
 print('BEFORE',rpc('ping'),flush=True)
 resets=d.qmp.reset_count
 if args.automatic:
  rpc('launch','com.apple.Preferences');time.sleep(3)
  assert b'text: General' in rpc('uidump')
  d.qmp.tap(150,225);time.sleep(2)
  assert b'text: Auto-Lock' in rpc('uidump')
  d.qmp.tap(150,423);time.sleep(2)
  assert b'text: 1 Minute' in rpc('uidump')
  d.qmp.tap(150,96);time.sleep(2)
  d.qmp.home();time.sleep(2)
 d.qmp.cmd('qom-set',path='/machine',property='usb-attached',value=False)
 if not args.automatic:r.itqmp.button(d.qmp,'power',120)
 time.sleep(5);capture('awake' if args.automatic else 'locked')
 time.sleep(55);capture('idle60')
 time.sleep(35);capture('idle95')
 r.itqmp.button(d.qmp,'power' if args.automatic else 'home',120)
 time.sleep(5);capture('wake')
 print('AFTER',rpc('ping'),flush=True)
 assert d.qmp.reset_count==resets,'wake reset the guest'
 d.qmp.cmd('qom-set',path='/machine',property='usb-attached',value=True)
 assert d.powerdown()
 assert b'unexpected CLCD interrupt' not in Path(d.serial).read_bytes()
 print('PASS:', 'Settings Auto-Lock and Power wake' if args.automatic else 'Lock and Home wake', 'through 95 seconds untethered, agent recovery, no reset or unexpected CLCD interrupts and native shutdown',flush=True)
finally:
 if d.qmp:d.qmp.close()
 p.stop_all()
