#!/usr/bin/env python3
"""Native battery calibration, virtual-time drain, pause and charging freeze."""
import os,tempfile,time,plistlib
from pathlib import Path
from types import SimpleNamespace
import regress as r
root=Path(__file__).resolve().parents[2]
files=str(root.parent/'qemu-ios-files')
out=Path(tempfile.mkdtemp(prefix='it-battery-guest-'))
cfg=SimpleNamespace(out=str(out),files=files,base_nand=files+'/nand-current',
    nor=files+'/ios3/nor_7E18.bin',overlay=str(out/'overlay'),
    qemu=str(root/'build-native14/qemu-build/qemu-system-arm'),
    usbmuxd=str(root/'build-native14/build/usbmuxd/src/usbmuxd'),usbmuxd_ok=True,
    mux_port=r.free_port(27400,27419),usb_port=r.free_port(1520,1539),
    qmp_port=r.free_port(28200,28219),wifi=False,cpu=None,mem='128M',kernel_console=True)
class Procs(r.Procs):
    def spawn(self,argv,logpath,env=None):
        if argv[0]==cfg.qemu:
            argv=list(argv)
            argv[argv.index('-M')+1]+=',battery-level=60,battery-charging=off'
        return super().spawn(argv,logpath,env)
p=Procs();d=r.Device(cfg,p,'device');r.START=time.time()
print('OUTPUT',out,flush=True)
def rpc(op,arg='',data=b''):
    status,result=r.itqmp.agent(d.qmp,op,arg,data)
    assert status==0,(op,status,result)
    return result
def get(name):return d.qmp.cmd('qom-get',path='/machine',property=name)
def setprop(name,value):d.qmp.cmd('qom-set',path='/machine',property=name,value=value)
def report():
    info=plistlib.loads(rpc('exec','/tmp/itbattery'))
    print('BATTERY',{k:info.get(k) for k in ('CurrentCapacity','Voltage','IsCharging','ExternalConnected')},flush=True)
    return info
try:
    d.start();ok,detail,_=d.wait_for_home(180);assert ok,detail
    deadline=time.monotonic()+90
    while not r.itqmp.agent_alive(d.qmp):
        assert time.monotonic()<deadline;time.sleep(1)
    rpc('put','/tmp/itbattery 755',(root/'contrib/it-halt/itbattery').read_bytes())
    initial=report()
    assert abs(initial['CurrentCapacity']-60)<=2,initial
    assert get('battery-charging')=='off'
    # Exercise normal discharge: USB-powered, noncharging 7E18 can defer
    # voltage measurements indefinitely, even though the target changes.
    setprop('usb-attached',False)
    for bad in (-1,101):
        try:setprop('battery-drain',bad)
        except RuntimeError:pass
        else:raise AssertionError('invalid drain accepted')
    setprop('battery-drain',50)
    time.sleep(30)
    target=get('battery-level')
    # The calibrated curve has a 32–36 percent voltage plateau.
    assert 30<=target<=37,target
    d.qmp.cmd('stop')
    frozen=get('battery-level');time.sleep(5)
    assert get('battery-level')==frozen,'paused VM drained'
    d.qmp.cmd('cont');setprop('battery-drain',0)
    assert get('battery-drain')==0
    deadline=time.monotonic()+240
    while True:
        current=report()
        if current['Voltage']<initial['Voltage']:break
        assert time.monotonic()<deadline,'guest ADC did not observe lower battery voltage'
        time.sleep(5)
    setprop('usb-attached',True)
    setprop('battery-charging','auto');setprop('battery-drain',100)
    charging_level=get('battery-level');time.sleep(5)
    assert get('battery-level')==charging_level,'USB charging drained'
    setprop('battery-drain',0)
    assert d.powerdown(),'guest shutdown not confirmed'
    print('PASS: native 60% calibration, sampled voltage drop, drain bounds, pause freeze, USB charging freeze and shutdown',flush=True)
finally:
    if d.qmp:d.qmp.close()
    p.stop_all()
