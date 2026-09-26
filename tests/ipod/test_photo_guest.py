#!/usr/bin/env python3
"""Native Saved Photos registration, thumbnails, receipt recovery and persistence."""
import argparse
import os
import tempfile
import time
from pathlib import Path
from types import SimpleNamespace
from PIL import Image, ImageDraw
import regress as r

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--files',default=str(ROOT.parent/'qemu-ios-files'))
parser.add_argument('--base-nand')
args = parser.parse_args()
out = Path(tempfile.mkdtemp(prefix='it-photo-guest-'))
cfg = SimpleNamespace(out=str(out),files=args.files,
    base_nand=args.base_nand or args.files+'/nand-current',
    nor=args.files+'/ios3/nor_7E18.bin',overlay=str(out/'overlay'),
    qemu=str(ROOT/'build-native14/qemu-build/qemu-system-arm'),usbmuxd_ok=False,
    usb_port=r.free_port(1520,1539),qmp_port=r.free_port(28200,28219),
    wifi=False,cpu=None,mem='128M',kernel_console=True,h264_decode="on",scaler_decode="on",mpvd_decode="on",amc_mode="decode",lcd_planes="on")
image = Image.new('RGB',(640,480),'white')
draw = ImageDraw.Draw(image)
for x,color in [(0,(220,30,30)),(213,(30,210,30)),(426,(30,30,220))]:
    draw.rectangle((x,0,min(x+212,639),479),fill=color)
draw.ellipse((200,120,440,360),fill='white')
image.save(out/'source.jpg',quality=90)
p = r.Procs()
d = r.Device(cfg,p,'first')
r.START = time.time()
print('OUTPUT',out,flush=True)

def rpc(op,argument='',data=b''):
    status, result = r.itqmp.agent(d.qmp,op,argument,data)
    assert status == 0,(op,argument,status,result)
    return result

def ready():
    ok,detail,_ = d.wait_for_home(240)
    assert ok,detail
    deadline = time.monotonic()+90
    while not r.itqmp.agent_alive(d.qmp):
        assert time.monotonic()<deadline
        time.sleep(1)
    rpc('put','/tmp/itphoto 755',(ROOT/'contrib/it-media/itphoto').read_bytes())

def check_files():
    listing = rpc('exec','find /var/mobile/Media/DCIM -type f').decode().splitlines()
    originals = [path for path in listing if path.endswith('.JPG')]
    assert originals == ['/var/mobile/Media/DCIM/100APPLE/IMG_0001.JPG'],originals
    for suffix in ['BTH','THM']:
        assert '/var/mobile/Media/DCIM/100APPLE/.MISC/IMG_0001.'+suffix in listing
    data = rpc('get',originals[0])
    (out/'saved.jpg').write_bytes(data)
    with Image.open(out/'saved.jpg') as saved:
        assert saved.size == (640,480),saved.size
        for point,expected in [((80,80),(220,30,30)),((320,80),(30,210,30)),((560,80),(30,30,220))]:
            actual = saved.convert('RGB').getpixel(point)
            assert all(abs(a-b)<15 for a,b in zip(actual,expected)),(point,actual)
    assert rpc('get','/var/mobile/Media/LightTouch/photo/.photo-receipt') == b'done\n/var/mobile/Media/DCIM/100APPLE/IMG_0001.JPG\n'

try:
    d.start()
    ready()
    rpc('exec','mkdir -p /var/mobile/Media/LightTouch/photo /var/mobile/Media/LightTouch/bad '
               '/var/mobile/Media/LightTouch/pending && chown -R 501:501 /var/mobile/Media/LightTouch')
    rpc('put','/var/mobile/Media/LightTouch/photo/image.jpg 644',(out/'source.jpg').read_bytes())
    assert rpc('exec','/tmp/itphoto photo') == b'imported\n'
    rpc('put','/var/mobile/Media/LightTouch/photo/image.jpg 644',(out/'source.jpg').read_bytes())
    assert rpc('exec','/tmp/itphoto photo') == b'already-imported\n'
    rpc('exec','test ! -e /var/mobile/Media/LightTouch/photo/image.jpg')
    rpc('put','/var/mobile/Media/LightTouch/bad/image.jpg 644',b'not a JPEG')
    rpc('put','/var/mobile/Media/LightTouch/pending/.photo-receipt 644',b'pending\n')
    for identifier in ['bad','pending','../escape']:
        status,result = r.itqmp.agent(d.qmp,'exec','/tmp/itphoto '+identifier)
        assert status != 0 and b'itphoto:' in result,(identifier,status,result)
    rpc('exec','test ! -e /var/mobile/Media/LightTouch/bad/.photo-receipt')
    check_files()
    control = r.prepare_app_control(cfg,p,d,r.Result('photo control'))
    ok,detail = r.unlock(cfg,control,d)
    assert ok,detail
    rpc('launch','com.apple.mobileslideshow')
    deadline = time.monotonic()+45
    while not rpc('frontmost').startswith(b'com.apple.mobileslideshow'):
        assert time.monotonic()<deadline
        time.sleep(1)
    time.sleep(2)
    r.to_png(d.qmp.shot(str(out/'albums.ppm')),str(out/'albums.png'))
    d.qmp.tap(140,91)
    time.sleep(5)
    r.to_png(d.qmp.shot(str(out/'grid.ppm')),str(out/'grid.png'))
    with Image.open(out/'grid.png') as grid:
        for point,channel in [((12,78),0),((40,78),1),((70,78),2)]:
            pixel = grid.convert('RGB').getpixel(point)
            assert pixel[channel] > 150 and all(pixel[c] < 90 for c in range(3) if c != channel),(point,pixel)
    d.qmp.tap(40,105)
    time.sleep(2)
    r.to_png(d.qmp.shot(str(out/'photo.ppm')),str(out/'photo.png'))
    assert d.powerdown(), 'guest shutdown not confirmed'
    d = r.Device(cfg,p,'reboot')
    d.start()
    ready()
    check_files()
    assert rpc('exec','/tmp/itphoto photo') == b'already-imported\n'
    assert d.powerdown(), 'reboot shutdown not confirmed'
    print('PASS: Saved Photos, native thumbnails, image colors, duplicate/uncertain receipts, invalid input and cold persistence',flush=True)
finally:
    if d.qmp:
        d.qmp.close()
    p.stop_all()
