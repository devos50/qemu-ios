#!/usr/bin/env python3
"""Verify a running guest's Bluetooth clients without enabling UART/DMA traces.

Build bluetooth-stack-probe.c using contrib/armv6-toolchain/armv6.sh cc6/link6
and ldid -S. Run against an isolated device, after boot:
  python3 tests/ipod/check-bluetooth-stack.py --qmp 127.0.0.1:45981 /tmp/btprobe
The guest agent must be installed. This checks stack responsiveness; launch
animation still requires inspecting intermediate guest LCD frames.
"""
import argparse
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "imgtools"))
from itqmp import QMP, agent, agent_alive

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("probe", type=Path)
parser.add_argument("--qmp", default="127.0.0.1:45981")
parser.add_argument("--startup-grace", type=float, default=10)
args = parser.parse_args()
q = QMP(args.qmp, timeout=60)
try:
    deadline = time.monotonic() + 60
    while not agent_alive(q):
        if time.monotonic() >= deadline:
            raise SystemExit("FAIL: guest agent did not become ready")
        time.sleep(0.25)
    status, output = agent(q, "put", "/tmp/bluetooth-stack-probe 755", args.probe.read_bytes())
    if status:
        raise SystemExit(f"FAIL: probe upload: {status}: {output!r}")
    time.sleep(max(0, args.startup_grace))
    for trial in range(3):
        status, output = agent(q, "exec", "/tmp/bluetooth-stack-probe")
        print(output.decode(errors="replace"), end="", flush=True)
        if status or b"sharedInstance=" not in output:
            raise SystemExit(f"FAIL: Bluetooth stack unavailable or stalled (probe status {status})")
        if trial < 2:
            time.sleep(1)
    print("PASS: BluetoothManager attaches and responds within 500 ms on three calls")
finally:
    q.close()
