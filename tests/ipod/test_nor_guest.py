#!/usr/bin/env python3
"""Opt-in NOR persistence check across a native guest reboot, using disposable NAND."""
from pathlib import Path
from types import SimpleNamespace
import argparse
import os
import subprocess
import shutil
import tempfile
import time
import regress as r

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--files', type=Path, default=ROOT.parent / 'qemu-ios-files')
parser.add_argument('--qemu', type=Path, default=ROOT / 'build-native14/qemu-build/qemu-system-arm')
parser.add_argument('--persistent', action='store_true')
args = parser.parse_args()
r.START = time.time()
with tempfile.TemporaryDirectory(prefix='it-nor-guest-') as temporary:
    out = Path(temporary)
    # reboot2 is the same native syscall used by the agent's halt operation.
    # Issue it directly: agent exec deliberately kills background descendants.
    (out / 'reboot.c').write_text('extern int reboot2(int,const char*); int main(void){return reboot2(0,0);}\n')
    env = os.environ.copy()
    env.setdefault('ARMV6_SDK', str(ROOT.parent / 'ipod2g-re/OldSDK/iPhoneOS3.1.3.sdk'))
    subprocess.run(['bash', '-c', '''
set -eu
source "$1/contrib/armv6-toolchain/armv6.sh"
cc6 "$2/reboot.c" "$2/reboot.o"
link6 -execute "$2/reboot" "$2/reboot.o"
"${LDID:-ldid}" -S"$1/contrib/it-gles/sblaunch-entitlements.xml" "$2/reboot"
''', 'nor-test', str(ROOT), str(out)], env=env, check=True)
    files = str(args.files)
    cfg = SimpleNamespace(out=str(out), files=files, base_nand=files+'/nand-current',
        nor=files+'/ios3/nor_7E18.bin', overlay=str(out/'overlay'), qemu=str(args.qemu),
        usbmuxd_ok=False, usb_port=r.free_port(1520,1539), qmp_port=r.free_port(28200,28219),
        wifi=False, cpu=None, mem='128M', kernel_console=True)
    if args.persistent:
        shutil.copyfile(cfg.nor, out/'nor.bin')
    class Procs(r.Procs):
        def spawn(self, argv, *rest, **kwargs):
            if args.persistent and argv[0] == cfg.qemu:
                argv = list(argv)
                argv[argv.index('-M')+1] += ',nor-rw='+str(out/'nor.bin')
            return super().spawn(argv, *rest, **kwargs)
    procs = Procs()
    device = r.Device(cfg, procs, 'device')
    try:
        device.start()
        ok, detail, _ = device.wait_for_home(180)
        assert ok, detail
        q = device.qmp
        q.cmd('qom-set', path='/machine', property='usb-attached', value=False)
        deadline = time.monotonic()+90
        while not r.itqmp.agent_alive(q):
            assert device.alive() and time.monotonic()<deadline, 'agent unavailable'
            time.sleep(1)
        assert r.itqmp.agent(q, 'put', '/tmp/nor-reboot 755', (out/'reboot').read_bytes()) == (0, b'')
        assert r.itqmp.agent(q, 'exec', 'nvram ltm-nor-test=persisted') == (0, b'')
        resets = q.reset_count
        q.cmd('qom-set', path='/machine', property='agent-request',
              value='nor-reboot exec /tmp/nor-reboot\n')
        deadline = time.monotonic()+90
        while q.reset_count == resets:
            q.cmd('query-status')
            assert device.alive() and time.monotonic()<deadline, 'native reboot did not reset'
            time.sleep(.5)
        time.sleep(3)
        deadline = time.monotonic()+120
        while not r.itqmp.agent_alive(q):
            assert device.alive() and time.monotonic()<deadline, 'agent did not return'
            time.sleep(1)
        result = r.itqmp.agent(q, 'exec', 'nvram ltm-nor-test')
        assert result == (0, b'ltm-nor-test\tpersisted\n'), result
        assert device.powerdown(), 'guest shutdown failed'
        print('PASS: native NOR program/erase, reboot persistence and confirmed shutdown', flush=True)
        if args.persistent:
            device = r.Device(cfg, procs, 'fresh-process')
            device.start()
            ok, detail, _ = device.wait_for_home(180)
            assert ok, detail
            q = device.qmp
            q.cmd('qom-set', path='/machine', property='usb-attached', value=False)
            deadline = time.monotonic()+90
            while not r.itqmp.agent_alive(q):
                assert device.alive() and time.monotonic()<deadline
                time.sleep(1)
            result = r.itqmp.agent(q, 'exec', 'nvram ltm-nor-test')
            assert result == (0, b'ltm-nor-test\tpersisted\n'), result
            assert device.powerdown()
            print('PASS: native NVRAM survives a fresh process', flush=True)
    finally:
        if device.qmp:
            device.qmp.close()
        procs.stop_all()
