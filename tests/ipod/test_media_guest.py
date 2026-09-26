#!/usr/bin/env python3
"""Opt-in 7E18 song/movie import, duplicate recovery, library query and music playback.

Build contrib/it-media and contrib/it-harness first. Uses an isolated overlay;
no user's live library is accessed. Requires the native USB tools and NumPy.
"""
import argparse
import os
from pathlib import Path
import plistlib
import sqlite3
import tempfile
import time
from types import SimpleNamespace
import regress as r

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--files', default=str(ROOT.parent/'qemu-ios-files'))
parser.add_argument('--base-nand')
args = parser.parse_args()
os.environ['PATH'] = str(ROOT.parent/'qemu-ios-deps12/bin') + ':' + os.environ['PATH']
out = Path(tempfile.mkdtemp(prefix='it-media-guest-'))
cfg = SimpleNamespace(out=str(out), files=args.files,
    base_nand=args.base_nand or args.files+'/nand-current',
    nor=args.files+'/ios3/nor_7E18.bin', overlay=str(out/'overlay'),
    qemu=str(ROOT/'build-native14/qemu-build/qemu-system-arm'),
    usbmuxd=str(ROOT/'build-native14/build/usbmuxd/src/usbmuxd'), usbmuxd_ok=True,
    usb_port=r.free_port(1520,1539), mux_port=r.free_port(27400,27419),
    qmp_port=r.free_port(28200,28219), wifi=False, cpu=None, mem='128M',
    kernel_console=True, amc_mode="decode", install_timeout=420, proxy_lo=28460, proxy_hi=28479)
p = r.Procs()
d = r.Device(cfg,p,'first')
r.START = time.time()
print('OUTPUT',out,flush=True)

def rpc(op, argument='', data=b''):
    status, result = r.itqmp.agent(d.qmp,op,argument,data)
    assert status == 0, (op,argument,status,result)
    return result

def ready():
    ok, detail, _ = d.wait_for_home(240)
    assert ok, detail
    deadline = time.monotonic()+90
    while not r.itqmp.agent_alive(d.qmp):
        assert time.monotonic()<deadline, 'agent unavailable'
        time.sleep(1)
    control = r.prepare_app_control(cfg,p,d,r.Result('media control'))
    ok, detail = r.unlock(cfg,control,d)
    assert ok, detail

def import_command(staging):
    return '/tmp/itmedia /tmp/'+staging+'.plist '+staging

def launch(bundle):
    rpc('launch',bundle)
    deadline = time.monotonic()+45
    while True:
        front = rpc('frontmost').splitlines()
        if front and front[0].decode().startswith(bundle):
            break
        assert time.monotonic()<deadline, ('app did not become foreground',bundle,front)
        time.sleep(1)
    time.sleep(2)

def verify_database(label):
    data = rpc('get','/var/mobile/Media/iTunes_Control/iTunes/iTunes Library.itlp/Library.itdb')
    path = out/(label+'.itdb')
    path.write_bytes(data)
    with sqlite3.connect(path) as db:
        rows = db.execute('SELECT title,artist,album,total_time_ms FROM item WHERE is_song=1 ORDER BY title').fetchall()
    assert rows == [('Harness AAC','Light Touch','Fixture Album',6000.0),
                    ('Harness MP3','Light Touch','Fixture Album',6000.0)],rows
    with sqlite3.connect(path) as db:
        movie = db.execute("SELECT title,is_song,media_kind FROM item WHERE title='Harness Movie'").fetchall()
    assert movie == [('Harness Movie',0,2)],movie

try:
    helper = (ROOT/'contrib/it-media/itmedia').read_bytes()
    ipa = ROOT/'contrib/it-harness/build/Harness.ipa'
    assert ipa.is_file(), 'build Harness first'
    d.start(audio_wav=str(out/'music.wav'))
    ready()
    rpc('put','/tmp/itmedia 755',helper)
    # No Music launch before this: the helper must initialize a fresh library.
    rpc('exec','mkdir -p /var/mobile/Media/LightTouch/aac /var/mobile/Media/LightTouch/mp3 && '
               'chown -R 501:501 /var/mobile/Media/LightTouch')
    for staging, name, title in [('aac','aac.m4a','Harness AAC'),('mp3','tone.mp3','Harness MP3')]:
        rpc('put','/var/mobile/Media/LightTouch/'+staging+'/'+name+' 644',
            (ROOT/'contrib/it-harness/build/Payload/Harness.app'/name).read_bytes())
        metadata = dict(filename=name,title=title,artist='Light Touch',album='Fixture Album',duration_ms=6000)
        rpc('put','/tmp/'+staging+'.plist 644',plistlib.dumps(metadata))
        result = rpc('exec',import_command(staging))
        assert b'imported\n' in result,result
        repeated = rpc('exec',import_command(staging))
        assert repeated.splitlines()[-1:] == [b'already-imported'], repeated
    rpc('exec','mkdir -p /var/mobile/Media/LightTouch/movie && '
               'chown 501:501 /var/mobile/Media/LightTouch/movie')
    rpc('put','/var/mobile/Media/LightTouch/movie/video.mp4 644',
        (ROOT/'contrib/it-harness/build/Payload/Harness.app/h264.mp4').read_bytes())
    rpc('put','/tmp/movie.plist 644',plistlib.dumps(dict(filename='video.mp4',
        title='Harness Movie',kind='feature-movie',duration_ms=6000)))
    assert rpc('exec',import_command('movie')).splitlines()[-1:] == [b'imported']
    assert rpc('exec',import_command('movie')).splitlines()[-1:] == [b'already-imported']
    # Invalid metadata and paths must not create another song.
    for metadata, staging in [({'filename':'../aac.m4a'},'aac'),
                              ({'filename':'aac.m4a','title':42,'duration_ms':6000},'aac'),
                              ({'filename':'aac.m4a','title':'Invalid','duration_ms':-1},'aac'),
                              ({'filename':'aac.m4a','title':'Invalid','duration_ms':6000},'../aac')]:
        rpc('put','/tmp/bad.plist 644',plistlib.dumps(metadata))
        status, result = r.itqmp.agent(d.qmp,'exec','/tmp/itmedia /tmp/bad.plist '+staging)
        assert status != 0 and b'itmedia:' in result,(status,result)
    verify_database('imported')
    print('PASS: two songs and one movie, duplicate recovery and invalid metadata rejection',flush=True)
    # 7E18 serves third-party MPMediaQuery requests from MobileMusicPlayer's
    # MIG service. Start Music before asking Harness to connect to that service.
    launch('com.apple.mobileipod')
    udid, detail = r.wait_for_device(cfg,timeout=120)
    assert udid,detail
    installed = r.run(['ideviceinstaller','install',str(ipa)],cfg,180)
    assert installed.returncode == 0,installed
    launch('com.qemuios.harness')
    for _ in range(6):
        r.itqmp.move(d.qmp,160,290)
        d.qmp.cmd('input-send-event',events=[{'type':'btn','data':{'down':True,'button':'left'}}])
        for step in range(1,27):
            r.itqmp.move(d.qmp,160,290-step*5)
            time.sleep(.05)
        time.sleep(.5)
        d.qmp.cmd('input-send-event',events=[{'type':'btn','data':{'down':False,'button':'left'}}])
        time.sleep(.5)
    d.qmp.tap(160,292)
    time.sleep(1)
    tree = rpc('uidump')
    (out/'harness-ui.txt').write_bytes(tree)
    assert b'MEDIA songs=2 first=Harness AAC' in tree,tree[-3000:]
    print('PASS: MediaPlayer reports two songs',flush=True)
    launch('com.apple.mobileipod')
    d.qmp.tap(160,455)  # Songs tab
    time.sleep(2)
    r.to_png(d.qmp.shot(str(out/'songs.ppm')),str(out/'songs.png'))
    for _ in range(16):
        r.itqmp.button(d.qmp,'volup',hold_ms=100)
    d.qmp.tap(130,137)  # First song, below Shuffle; AAC then MP3 in queue order.
    time.sleep(2)
    r.to_png(d.qmp.shot(str(out/'playing.ppm')),str(out/'playing.png'))
    time.sleep(13)
    assert d.powerdown(), 'guest shutdown not confirmed'
    audio = r.Result('media audio')
    assert r.verify_audio(str(out/'music.wav'),audio),audio.detail
    import wave
    import numpy as np
    with wave.open(str(out/'music.wav')) as recording:
        samples = np.frombuffer(recording.readframes(recording.getnframes()),dtype='<i2').reshape(-1,2)
        active = np.max(np.abs(samples.astype(np.int32)),axis=1)>100
        seconds = np.count_nonzero(active)/recording.getframerate()
    assert seconds>10,('expected both six-second tracks',seconds)
    # A cold boot must retain the songs/movie and reconcile the same song again.
    cfg.usbmuxd_ok = False
    d = r.Device(cfg,p,'reboot')
    d.start()
    ready()
    verify_database('reboot')
    rpc('put','/tmp/itmedia 755',helper)
    rpc('put','/tmp/aac.plist 644',plistlib.dumps(dict(filename='aac.m4a',title='Harness AAC',duration_ms=6000)))
    repeated = rpc('exec',import_command('aac'))
    assert repeated.splitlines()[-1:] == [b'already-imported'], repeated
    assert d.powerdown(), 'reboot shutdown not confirmed'
    print('PASS: AAC/MP3 and movie imports, duplicate recovery, invalid inputs, MediaPlayer song count, Music playback and cold persistence',flush=True)
finally:
    if d.qmp:
        d.qmp.close()
    p.stop_all()
