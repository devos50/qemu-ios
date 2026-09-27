#!/usr/bin/env python3
"""Regression harness for the emulated iPod touch 2G.

Every check here corresponds to a bug that actually shipped in this tree, so
none of them is a smoke test: "did it boot?" would have missed all of them.

  boot        SpringBoard reaches the home screen, judged from the *framebuffer*
              and not from a serial banner. A machine that prints the whole boot
              log and then shows a black screen is a regression the log cannot
              see. The panel's samples are very dark, so the frame is counted by
              lit sub-pixels rather than looked at: the boot logo is around 10k
              lit and the home screen around 290k, so the two are impossible to
              confuse.
  afc         Push and pull files over AFC and compare SHA-256, at sizes that
              are deliberately *not* multiples of 512. A residue bug in the USB
              bulk-IN path corrupted exactly the trailing partial packet, and
              round, 512-aligned test files never saw it.
  usbtcp      TCP-over-USB forwarding: iproxy to lockdownd on 62078 and a real
              lockdown QueryType round trip. This exercises the mux's stream
              layer rather than a one-shot service request.
  wifi        The BCM4325 model associates ("AirPort: Link Up on en0") *and* the
              guest takes a DHCP lease - checked on the wire, from the netdev
              capture, because a carrier with no lease looks identical in the log.
  appinstall  ideviceinstaller installs a decrypted App Store app and it appears
              in the installed list.
  applaunch   Launch the installed IPA's bundle ID through SpringBoard, then
              verify that exact foreground app and a lit screen. Requires
              appinstall, guest SSH, and built contrib/it-gles/sblaunch.
  persist     A file written over AFC survives a *clean* shutdown and a reboot on
              the same overlay, byte-identical. HFS+ holds catalog updates in
              memory, so killing QEMU loses the directory entry while keeping the
              data blocks: the file silently ceases to exist. Only a
              system_powerdown unmounts the volume.
  serial-console
              Enable early serial boot arguments and require XNU driver output
              and the BSD root mount in serial.log (explicit --checks only).
  fsck        The base image composed with the overlay fscks clean, i.e. the
              persisted pages landed on the physical pages the FTL mapping says
              they should.

Usage:
    tests/ipod/regress.py                    # default tier: boot, fsck, persist, appinstall, applaunch, gles, agent, audio
    tests/ipod/regress.py --with-apps         # + afc, usbtcp, wifi, appinstall, applaunch
    tests/ipod/regress.py --quick            # boot + afc only, one boot
    tests/ipod/regress.py --checks boot,wifi # explicit selection, any tier
    tests/ipod/regress.py --check-prereqs    # report missing artefacts, run nothing

The default tier needs a built qemu-system-arm and a NAND containing it_agent.
USB-side checks need the usbmuxd fork (--usbmuxd); missing host tools cause an
individual SKIP. A present image whose agent fails to start is a failure.
App checks use the bundled Harness.ipa by default; --ipa selects another app.

Exits non-zero if any selected check FAILs (SKIP and XFAIL do not count).

Known, reproducible interaction: appinstall run after afc in the same boot
fails - installd resets the mux connection mid-install
("Install: ExtractingPackage (15%)"), no app is registered, and
`ideviceinstaller` hangs in idevice_wait_for_command_to_complete for a
completion status that never comes, because it has no timeout of its own.
An app installs and launches perfectly on a device that has done nothing
else first. --install-timeout bounds the hang so the rest of the suite still
runs. This historical failure is still a FAIL: running AFC first is not
proof that a later installation failure has the same cause.
"""

import argparse
import hashlib
import json
import os
import plistlib
import re
import shlex
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zipfile
from xml.parsers.expat import ExpatError

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

sys.path.insert(0, os.path.join(ROOT, "imgtools"))
import itqmp  # noqa: E402  (needs the path above)

W, H = 320, 480
FRAME_BYTES = W * H * 3

# Lit sub-pixels. Measured on this tree: boot logo ~9.8k, a half-composited home
# screen (logo still drawn over icons that have already landed) ~121k, a settled
# home screen ~214k. The gate has to clear the *half-drawn* frame, not just the
# logo - the first version of this harness passed at 121k and called a boot
# finished while SpringBoard was still painting.
HOME_LIT_MIN = 180000
# ...and it has to be a picture rather than a solid fill. iBoot paints the panel
# its default colour - white on the pinot panel - and leaves it there when the
# boot fails, so a device sitting in recovery mode lights EVERY sub-pixel and
# sails through a lit floor at 460800. That is not hypothetical: it is what made
# this harness report PASS at t+13s against a NOR whose device tree iBoot could
# not load, and then spend 420s waiting for a mux device that was never going to
# arrive. A home screen has dark pixels in it; a fill does not.
SOLID_LIT_MAX = int(FRAME_BYTES * 0.98)
# And the frame has to stay there: one confirming sample this many seconds later
# must also be above the gate, so a transient cannot be mistaken for a boot.
HOME_CONFIRM_S = 15
LIT_THRESHOLD = 8

# Default tier: self-contained on a clean checkout, needs only a built qemu
# and a base NAND image containing it_agent.
DEFAULT_CHECKS = ["boot", "fsck", "persist", "appinstall", "applaunch", "gles", "agent", "audio"]
# Additional transport and restart checks remain opt-in.
OPT_IN_CHECKS = ["afc", "usbtcp", "wifi", "respring", "restart"]
ALL_CHECKS = DEFAULT_CHECKS + OPT_IN_CHECKS + ["serial-console", "webproxy"]
QUICK_CHECKS = ["boot", "afc"]

# Checks that talk to the device over usbmux and so need the usbmuxd fork.
# persist is in the default tier but still needs this - see the plan's
# "current state" claim that it doesn't; that turned out not to hold, so it
# SKIPs individually rather than being promised as an unconditional PASS.
USB_DEPENDENT_CHECKS = {"afc", "usbtcp", "appinstall", "applaunch", "persist",
                        "gles", "audio", "respring", "restart", "webproxy"}
# Checks that need a real .ipa.
IPA_DEPENDENT_CHECKS = {"appinstall", "applaunch"}

GLES_DIR = os.path.join(ROOT, "contrib", "it-gles")
GLES_BUNDLE_ID = "com.qemuios.gltest"
# Fraction of the frame that must be magenta, and cyan, for the render to
# count. GLTest clears magenta and draws a cyan quad over the left half of its
# view precisely because no part of the iOS UI produces either colour, so this
# is a far sharper assertion than a lit-pixel floor - measured on a real 3.1.3
# run, GLTest gives 0.281/0.281 and the home screen 0.0003/0.0082. A lit floor
# alone could not tell them apart at all (the home screen lights *more*
# sub-pixels than the GL frame does).
GLES_QUAD_MIN = 0.05
UNLOCK_TRIES = 4
# Seconds after launch before the first sample: the app has to get through
# SpringBoard's launch animation and its first present.
GLES_SETTLE_S = 25
# ...and the frame has to still be there this many seconds later. GLTest's
# scene is static by design, so "the pixels changed between two samples" is not
# available as a liveness test; what is available is that a renderer which
# wedged after its first present drops the app back to SpringBoard or to black,
# and the second sample catches that.
GLES_HOLD_S = 6
# Entry points GLTest is known to touch that the host does not implement, and
# whose absence does not change what it draws. Empty today: anything the test
# app calls, it needs. Add a slot here only with a note saying why it is inert.
GLES_ALLOWED_SLOTS = set()

HARNESS_IPA = os.path.join(ROOT, "contrib", "it-harness", "build", "Harness.ipa")
APP_IPA_DEFAULT = HARNESS_IPA


# --------------------------------------------------------------------------
# small utilities
# --------------------------------------------------------------------------

def log(msg):
    sys.stdout.write("[%7.1fs] %s\n" % (time.time() - START, msg))
    sys.stdout.flush()


def free_port(lo, hi):
    """Claim a free TCP port inside a range, so concurrent runs never collide."""
    for p in range(lo, hi + 1):
        s = socket.socket()
        try:
            s.bind(("127.0.0.1", p))
            return p
        except OSError:
            continue
        finally:
            s.close()
    raise RuntimeError("no free port in %d-%d" % (lo, hi))


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


read_ppm = itqmp.read_ppm


def lit_count(path):
    """(max sample, number of lit sub-pixels). Screendumps are extremely dark."""
    try:
        _w, _h, pix = read_ppm(path)
    except Exception:
        return 0, 0
    if not pix:
        return 0, 0
    return max(pix), sum(1 for v in pix if v > LIT_THRESHOLD)


def to_png(ppm, png):
    """Brighten and convert, so a human can look at a failure afterwards."""
    try:
        w, h, pix = read_ppm(ppm)
        hi = max(pix) if pix else 0
        if 0 < hi < 255:
            scale = 255.0 / hi
            pix = bytearray(min(255, int(v * scale)) for v in pix)
        tmp = ppm + ".norm.ppm"
        with open(tmp, "wb") as f:
            f.write(b"P6\n%d %d\n255\n" % (w, h) + bytes(pix))
        subprocess.run(["sips", "-s", "format", "png", tmp, "--out", png],
                       capture_output=True, timeout=60)
        os.remove(tmp)
    except Exception:
        pass


# --------------------------------------------------------------------------
# QMP
# --------------------------------------------------------------------------

# One shared client: QMP services only one connection at a time.
QMP = itqmp.QMP


class Procs:
    def __init__(self):
        self.procs = []

    def spawn(self, argv, logpath, env=None):
        f = open(logpath, "wb")
        p = subprocess.Popen(argv, stdout=f, stderr=subprocess.STDOUT,
                             env=env, start_new_session=True)
        p._logfile = f
        self.procs.append(p)
        return p

    def stop(self, p, grace=8):
        if p is None or p.poll() is not None:
            return
        p.terminate()
        deadline = time.time() + grace
        while time.time() < deadline and p.poll() is None:
            time.sleep(0.2)
        if p.poll() is None:
            p.kill()
        try:
            p.wait(timeout=5)
        except Exception:
            pass

    def stop_all(self):
        for p in reversed(self.procs):
            self.stop(p)
        self.procs = []


# --------------------------------------------------------------------------
# the emulated device, plus its host-side usbmux bridge
# --------------------------------------------------------------------------

def boot_env(cfg):
    """Environment for the QEMU child, with the vars 3.1.3 needs to boot.

    These are not tuning knobs. Without IT_DIRECT_IBOOT (and the
    TV-out gates) a 3.1.3 image hangs on the Apple logo forever, and without
    IT_LCD_BRIGHT=255 the backlight scales every pixel so a perfectly good
    frame reads as nearly black -- which is indistinguishable, to a lit-pixel
    threshold, from a device that never came up.

    Measured 2026-08-05: running this harness against nand-appsync3 from a
    shell that had none of these sat at lit=13456 for 573s and failed; the same
    check with them set reached lit=460800 and PASSed in 27s. The harness owns
    these now instead of silently depending on the caller's shell.

    Anything already exported by the caller wins, so a run can still override
    or bisect against them.
    """
    env = dict(os.environ)
    iboot = os.path.join(cfg.files, "ios3", "iBoot.bin")
    defaults = {
        "IT_LCD_BRIGHT": "255",
        "IT_TVOUT_READY": "1",
        "IT_BOOT_ARGS": "amfi_allow_any_signature=1 cs_enforcement_disable=1",
        "IT_BOOT_ARGS_DELAY_MS": "1500",
        "IT_BOOT_ARGS_REPEAT": "200",
        "IT_BOOT_ARGS_INTERVAL_MS": "250",
    }
    if os.path.exists(iboot):
        defaults["IT_DIRECT_IBOOT"] = iboot
    for k, v in defaults.items():
        env.setdefault(k, v)
    return env


class Device:
    def __init__(self, cfg, procs, tag):
        self.cfg = cfg
        self.procs = procs
        self.tag = tag
        self.dir = os.path.join(cfg.out, tag)
        os.makedirs(self.dir, exist_ok=True)
        self.serial = os.path.join(self.dir, "serial.log")
        self.pcap = os.path.join(self.dir, "wifi.pcap")
        self.qmp = None
        self.qemu = None
        self.mux = None

    # -- lifecycle ---------------------------------------------------------

    def start(self, audio_wav=None):
        cfg = self.cfg
        self.audio_wav = audio_wav
        # usbmuxd is only needed by USB-side checks; on a run where all of
        # those are SKIPped (no usbmuxd binary), don't try to spawn one.
        if getattr(cfg, "usbmuxd_ok", True):
            muxcfg = os.path.join(cfg.out, "muxcfg")
            os.makedirs(muxcfg, exist_ok=True)
            env = dict(os.environ)
            env["USBMUXD_QEMU_ADDR"] = "127.0.0.1:%d" % cfg.usb_port
            env["USBMUXD_QEMU_DELAY"] = "12"
            self.mux = self.procs.spawn(
                [cfg.usbmuxd, "-f", "-v", "-v", "-v",
                 "-S", "127.0.0.1:%d" % cfg.mux_port, "-P", "NONE", "-C",
                 muxcfg],
                os.path.join(self.dir, "usbmuxd.log"), env=env)
            log("%s: usbmuxd pid %d (mux %d, usb %d)"
                % (self.tag, self.mux.pid, cfg.mux_port, cfg.usb_port))
            time.sleep(2)

        # NO usb-patch-mux-gate. That patch is for 2.1.1/5F138, and on a 3.1.3
        # kernel it locates an instruction that is not the one it means and
        # rewrites it: the guest gets as far as loading drivers and then stops
        # dead - framebuffer frozen on the Apple logo, no further NAND writes,
        # and the vCPU spinning. Measured side by side on nand-appsync3: with
        # the flag, no home screen in 900s; without it (everything else
        # identical, usbmuxd included) the lock screen at t+32s and
        # `idevice_id -l` answering with the UDID. The gate it exists to open
        # is simply not shut on 3.1.3 - the mux enumerates without it.
        machine = ("iPod-Touch,bootrom=%s/bootrom_240_4,nand=%s,nor=%s,"
                   "nandrw=%s,usb-attached=on,usb-tcp-addr=127.0.0.1:%d"
                   % (cfg.files, cfg.base_nand, cfg.nor, cfg.overlay,
                      cfg.usb_port))
        if getattr(cfg, "kernel_console", False):
            machine += (",boot-args=amfi_allow_any_signature=1 "
                        "cs_enforcement_disable=1 serial=3 debug=0x8")
        for option in ("audio_hw", "h264_decode", "scaler_decode", "mpvd_decode", "amc_mode", "lcd_planes", "direct_iboot", "direct_llb"):
            value = getattr(cfg, option, None)
            if value is not None:
                machine += "," + option.replace("_", "-") + "=" + value.replace(",", ",,")
        # The BCM4325 is attached only for the check that tests it. It is not
        # free: 3.1.3's driver associates and then keeps the SDIO bus busy, and
        # every other check pays for a radio it never looks at. run-ios3.sh
        # makes the same split (--net is separate from --appsync).
        argv = [cfg.qemu, "-M", machine + (",wifi=on" if cfg.wifi else "")]
        argv += ["-cpu", cfg.cpu] if cfg.cpu else []
        argv += ["-m", cfg.mem, "-display", "none",
                # Headless runs stay silent: without this QEMU opens the host's
                # CoreAudio device and plays guest sound out of the speakers.
                "-audio", ("driver=wav,path=" + audio_wav if audio_wav else "driver=none"),
                "-serial", "file:" + self.serial,
                "-qmp", "tcp:127.0.0.1:%d,server=on,wait=off" % cfg.qmp_port]
        if cfg.wifi:
            proxy_option = ""
            if getattr(cfg, "web_proxy_config", None):
                helper = os.path.join(ROOT, "contrib", "it-webproxy", "itwebproxy")
                command = shlex.quote(helper) + " " + shlex.quote(cfg.web_proxy_config)
                proxy_option = ",guestfwd=tcp:10.0.2.100:3128-cmd:" + command.replace(",", ",,")
            argv += ["-netdev", "user,id=wifi0,net=10.0.2.0/24,host=10.0.2.2,"
                                "dhcpstart=10.0.2.15" + proxy_option,
                     "-object",
                     "filter-dump,id=cap0,netdev=wifi0,file=" + self.pcap]
        self.qemu = self.procs.spawn(argv, os.path.join(self.dir, "qemu.log"),
                                     env=boot_env(cfg))
        log("%s: qemu pid %d (qmp %d)" % (self.tag, self.qemu.pid, cfg.qmp_port))
        self.qmp = QMP(cfg.qmp_port, timeout=180, read_timeout=60)

    def alive(self):
        return self.qemu is not None and self.qemu.poll() is None

    def powerdown(self):
        """Require guest-origin SHUTDOWN plus process exit; SIGTERM also exits 0."""
        if self.qmp is None:
            log("%s: no QMP connection to confirm guest shutdown" % self.tag)
            return False
        try:
            if itqmp.agent_alive(self.qmp):
                try:
                    status, response = itqmp.agent(self.qmp, "halt", timeout=30)
                    if status:
                        raise RuntimeError("agent halt failed: %d %r" % (status, response))
                except EOFError:
                    # A guest shutdown can beat the RPC reply. The retained
                    # PMU SHUTDOWN event below remains the acceptance gate.
                    pass
                timeout = 60
            else:
                helper = os.path.join(ROOT, "contrib", "it-halt", "ithalt")
                port, error = ensure_guest_ssh(self.cfg, self.procs, self)
                if port is not None and os.path.exists(helper):
                    copied = guest_ssh(self.cfg, port, None, scp_from=helper,
                                       scp_to="/tmp/ithalt")
                    if copied.returncode != 0:
                        log("%s: could not stage shutdown helper: %s" % (self.tag, copied.stderr))
                        return False
                    halt = guest_ssh(self.cfg, port, ["chmod 755 /tmp/ithalt && /tmp/ithalt"], timeout=30)
                    log("%s: halt request rc=%d %s" %
                        (self.tag, halt.returncode, (halt.stdout + halt.stderr).strip()[-200:]))
                    timeout = 60
                else:
                    log("%s: gesture shutdown fallback (%s)" % (self.tag, error or "no ithalt"))
                    try:
                        self.qmp.cmd("system_powerdown")
                    except EOFError:
                        # An immediate shutdown may precede the command response;
                        # the retained SHUTDOWN event must still prove its origin.
                        pass
                    timeout = 180
            self.qmp.wait_for_guest_shutdown(timeout)
            rc = self.qemu.wait(timeout=10)
            log("%s: guest-confirmed shutdown, qemu exit=%d" % (self.tag, rc))
            return rc == 0
        except (OSError, EOFError, RuntimeError, ValueError, subprocess.TimeoutExpired) as exc:
            log("%s: shutdown not confirmed: %s" % (self.tag, exc))
            return False
        finally:
            self.qmp.close()
            self.qmp = None

    # -- boot --------------------------------------------------------------

    def wait_for_home(self, timeout):
        """Poll the framebuffer until the home screen is up.

        Polled rather than slept: a boot is ~3 minutes unloaded and much longer
        with other emulators on the machine, so any fixed sleep is either a
        false failure or a waste. The loop still fails hard if QEMU exits.
        """
        deadline = time.time() + timeout
        best = 0
        shot = os.path.join(self.dir, "boot.ppm")
        n = 0
        while time.time() < deadline:
            if not self.alive():
                return False, "qemu exited (rc=%s)" % self.qemu.returncode, best
            time.sleep(10)
            n += 1
            try:
                self.qmp.shot(shot)
            except Exception as e:
                return False, "screendump failed: %s" % e, best
            hi, lit = lit_count(shot)
            if lit >= SOLID_LIT_MAX:
                # See SOLID_LIT_MAX: a fill is iBoot, not SpringBoard.
                log("%s: t+%.0fs solid fill (%d lit) - the panel is showing a "
                    "flat colour, so this is iBoot/recovery, not a boot"
                    % (self.tag, time.time() - START, lit))
                continue
            best = max(best, lit)
            if n % 3 == 0 or lit > HOME_LIT_MIN // 2:
                log("%s: t+%.0fs max=%d lit=%d"
                    % (self.tag, time.time() - START, hi, lit))
            if lit >= HOME_LIT_MIN:
                time.sleep(HOME_CONFIRM_S)
                if not self.alive():
                    return False, "qemu exited right after the home screen", best
                self.qmp.shot(shot)
                _hi2, lit2 = lit_count(shot)
                best = max(best, lit2)
                if lit2 < HOME_LIT_MIN:
                    log("%s: home screen did not hold (%d -> %d), still waiting"
                        % (self.tag, lit, lit2))
                    continue
                to_png(shot, os.path.join(self.dir, "home.png"))
                return True, "lit=%d, held for %ds" % (lit2, HOME_CONFIRM_S), best
        return False, "timed out after %ds" % timeout, best

    def serial_text(self):
        try:
            with open(self.serial, "rb") as f:
                return f.read().decode("utf-8", "replace")
        except OSError:
            return ""


# --------------------------------------------------------------------------
# host-side device tools
# --------------------------------------------------------------------------

def mux_env(cfg):
    env = dict(os.environ)
    env["USBMUXD_SOCKET_ADDRESS"] = "127.0.0.1:%d" % cfg.mux_port
    return env


def run(argv, cfg, timeout, stdin=None):
    """Run a libimobiledevice tool against *our* mux.

    A timeout is reported as a failed run rather than raised: one tool wedging
    should fail its own check and let the rest of the suite continue, not abort
    the whole run. macOS has no timeout(1), and subprocess's own deadline is
    both portable and exact, so no perl alarm wrapper is needed.
    """
    try:
        return subprocess.run(argv, env=mux_env(cfg), input=stdin,
                              timeout=timeout, capture_output=True, text=True)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(
            argv, 124, "", "timed out after %ss" % timeout)


def wait_for_device(cfg, timeout=420):
    """Poll until the mux reports a device and lockdown answers.

    The guest needs ~100s of boot before it has programmed the USB core, and
    pairing can only happen after that, so this polls rather than assuming.
    """
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        try:
            r = run(["idevice_id", "-l"], cfg, 30)
            if r.stdout.strip():
                udid = r.stdout.split()[0]
                p = run(["idevicepair", "pair"], cfg, 120)
                last = (p.stdout + p.stderr).strip()
                if "SUCCESS" in last.upper() or "success" in last:
                    return udid, last
                # already paired counts too
                v = run(["idevicepair", "validate"], cfg, 60)
                if "SUCCESS" in (v.stdout + v.stderr).upper():
                    return udid, "already paired"
        except subprocess.TimeoutExpired:
            last = "timeout"
        except Exception as e:
            last = str(e)
        time.sleep(10)
    return None, last


def afc(cfg, commands, timeout=300):
    return run(["afcclient"], cfg, timeout,
               stdin="".join(c + "\n" for c in commands) + "quit\n")


# --------------------------------------------------------------------------
# checks
# --------------------------------------------------------------------------

class Result:
    def __init__(self, name):
        self.name = name
        self.ok = None           # None = never ran
        self.detail = ""
        self.xfail = False       # ok=False here is a documented, expected bug
        self.skipped = False     # explicitly skipped for missing inputs

    def set(self, ok, detail, xfail=False):
        self.ok = ok
        self.detail = detail
        self.xfail = xfail
        state = "PASS" if ok else ("XFAIL" if xfail else "FAIL")
        log("  %-11s %s  %s" % (self.name, state, detail))
        return ok

    def skip(self, reason):
        self.detail = reason
        self.skipped = True
        log("  %-11s SKIP  %s" % (self.name, reason))


def check_afc(cfg, dev, r):
    """Round-trip files over AFC and compare SHA-256.

    Sizes are chosen so that most are *not* multiples of 512: the USB bulk-IN
    residue bug corrupted only the trailing short packet, so 4096-byte test
    files passed while 65535-byte ones came back wrong.
    """
    sizes = [1000, 65535, 4096, 1, 262143]
    tmpdir = os.path.join(cfg.out, "afc")
    os.makedirs(tmpdir, exist_ok=True)
    bad = []
    for n in sizes:
        src = os.path.join(tmpdir, "s%d.bin" % n)
        dst = os.path.join(tmpdir, "r%d.bin" % n)
        with open(src, "wb") as f:
            f.write(os.urandom(n))
        remote = "/regress_%d.bin" % n
        p = afc(cfg, ["put -f %s %s" % (src, remote)])
        if p.returncode != 0:
            bad.append("%d: put rc=%d %s" % (n, p.returncode,
                                             (p.stdout + p.stderr)[-160:]))
            continue
        g = afc(cfg, ["get -f %s %s" % (remote, dst)])
        if g.returncode != 0 or not os.path.exists(dst):
            bad.append("%d: get rc=%d %s" % (n, g.returncode,
                                             (g.stdout + g.stderr)[-160:]))
            continue
        got = os.path.getsize(dst)
        if got != n:
            bad.append("%d: got %d bytes" % (n, got))
        elif sha256_file(src) != sha256_file(dst):
            bad.append("%d: sha mismatch" % n)
        os.path.exists(dst) and os.remove(dst)
    if bad:
        return r.set(False, "; ".join(bad))
    return r.set(True, "sha256 identical at %s bytes"
                 % ",".join(str(s) for s in sizes))


def lockdown_querytype(port, timeout=30):
    """Speak lockdown over a forwarded TCP port: 4-byte length + XML plist."""
    s = socket.create_connection(("127.0.0.1", port), timeout)
    s.settimeout(timeout)
    try:
        req = plistlib.dumps({"Request": "QueryType"})
        s.sendall(struct.pack(">I", len(req)) + req)
        hdr = b""
        while len(hdr) < 4:
            b = s.recv(4 - len(hdr))
            if not b:
                raise IOError("closed reading length")
            hdr += b
        n = struct.unpack(">I", hdr)[0]
        if n > 1 << 20:
            raise IOError("absurd reply length %d" % n)
        body = b""
        while len(body) < n:
            b = s.recv(n - len(body))
            if not b:
                raise IOError("closed reading body")
            body += b
        return plistlib.loads(body)
    finally:
        s.close()


def check_usbtcp(cfg, dev, procs, r):
    lport = free_port(cfg.proxy_lo, cfg.proxy_hi)
    p = procs.spawn(["iproxy", "-s", "127.0.0.1", "%d:62078" % lport],
                    os.path.join(dev.dir, "iproxy.log"), env=mux_env(cfg))
    try:
        time.sleep(2)
        err = ""
        for _ in range(6):
            try:
                reply = lockdown_querytype(lport)
                if reply.get("Type") == "com.apple.mobile.lockdown":
                    return r.set(True, "iproxy %d->62078, QueryType -> %s"
                                 % (lport, reply["Type"]))
                err = "unexpected reply %r" % reply
            except Exception as e:
                err = "%s: %s" % (type(e).__name__, e)
            time.sleep(5)
        return r.set(False, err)
    finally:
        procs.stop(p)


def pcap_dhcp_reply(path):
    """Count BOOTP/DHCP replies (UDP sport 67) in a libpcap capture."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return 0
    if len(data) < 24:
        return 0
    magic = data[:4]
    if magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1"):
        endian = "<"
    elif magic in (b"\xa1\xb2\xc3\xd4", b"\xa1\xb2\x3c\x4d"):
        endian = ">"
    else:
        return 0
    i, n = 24, 0
    while i + 16 <= len(data):
        _ts, _us, caplen, _olen = struct.unpack(endian + "IIII", data[i:i + 16])
        i += 16
        pkt = data[i:i + caplen]
        i += caplen
        if len(pkt) < 42:
            continue
        if struct.unpack(">H", pkt[12:14])[0] != 0x0800:
            continue
        ihl = (pkt[14] & 0x0f) * 4
        if pkt[23] != 17:
            continue
        u = 14 + ihl
        if len(pkt) < u + 8:
            continue
        sport, dport = struct.unpack(">HH", pkt[u:u + 4])
        if sport == 67 and dport == 68:
            n += 1
    return n


def check_wifi(cfg, dev, r):
    """Carrier *and* lease.

    "AirPort: Link Up on en0" on its own only says the driver's state machine
    was satisfied; the data channel can still be dead. The DHCP reply on the
    netdev capture is the part that proves packets move both ways.
    """
    deadline = time.time() + 180
    link = False
    while time.time() < deadline:
        if "AirPort: Link Up on en0" in dev.serial_text():
            link = True
            break
        time.sleep(10)
    leases = 0
    deadline = time.time() + 120
    while time.time() < deadline:
        leases = pcap_dhcp_reply(dev.pcap)
        if leases:
            break
        time.sleep(10)
    if link and leases:
        return r.set(True, "Link Up on en0, %d DHCP reply/replies" % leases)
    return r.set(False, "link=%s dhcp_replies=%d" % (link, leases))


def ipa_bundle_id(path):
    """Read the single root application's identity, including binary plists."""
    with zipfile.ZipFile(path) as archive:
        entries = [item for item in archive.infolist()
                   if re.fullmatch(r"Payload/[^/]+\.app/Info\.plist", item.filename)]
        if len(entries) != 1 or entries[0].file_size > 1024 * 1024:
            raise ValueError("IPA needs exactly one root app Info.plist (at most 1 MiB)")
        info = plistlib.loads(archive.read(entries[0]))
    bundle_id = info.get("CFBundleIdentifier") if isinstance(info, dict) else None
    if not isinstance(bundle_id, str) or not re.fullmatch(r"[A-Za-z0-9.-]+", bundle_id):
        raise ValueError("IPA has no valid CFBundleIdentifier")
    return bundle_id


def app_is_installed(cfg, bundle_id):
    listing = run(["ideviceinstaller", "list", "--xml"], cfg, 300)
    if listing.returncode != 0:
        return False
    try:
        apps = plistlib.loads(listing.stdout.encode())
    except (ValueError, plistlib.InvalidFileException, ExpatError):
        return False
    return isinstance(apps, list) and any(
        isinstance(app, dict) and app.get("CFBundleIdentifier") == bundle_id
        for app in apps)


def check_appinstall(cfg, procs, dev, r):
    """Install an IPA and confirm installd really registered it.

    The install runs through a file so that its progress output survives being
    killed: `ideviceinstaller install` waits for a completion status from
    installd in an unbounded loop (`idevice_wait_for_command_to_complete`), so
    if installd resets the connection part-way the tool never returns, and the
    only evidence of how far it got is what it had already printed. Success
    requires both installer completion and the exact IPA bundle ID in the
    device's application list; an already-installed app cannot hide failure.
    """
    if not os.path.exists(cfg.ipa):
        return r.set(False, "ipa not found: %s" % cfg.ipa)
    try:
        bundle_id = ipa_bundle_id(cfg.ipa)
    except (OSError, ValueError, zipfile.BadZipFile, RuntimeError, ExpatError) as exc:
        return r.set(False, "invalid IPA: %s" % exc)
    logpath = os.path.join(dev.dir, "install.log")
    p = procs.spawn(["ideviceinstaller", "install", cfg.ipa], logpath,
                    env=mux_env(cfg))
    deadline = time.time() + cfg.install_timeout
    while time.time() < deadline and p.poll() is None:
        time.sleep(5)
    timed_out = p.poll() is None
    if timed_out:
        procs.stop(p)
    try:
        with open(logpath) as f:
            out = " | ".join(l.strip() for l in f if l.strip())
    except OSError:
        out = ""
    listed = app_is_installed(cfg, bundle_id)
    if not timed_out and p.returncode == 0 and listed:
        return r.set(True, "%s installed and listed" % bundle_id)
    if timed_out:
        return r.set(False, "installer never finished within %ds and the app "
                            "installation was not confirmed; last progress: %s"
                     % (cfg.install_timeout, out[-300:]))
    return r.set(False, "rc=%s, %s listed=%s; %s"
                 % (p.returncode, bundle_id, listed, out[-300:]))


def ensure_guest_ssh(cfg, procs, dev):
    """Return (forwarded port, error), reusing this boot's SSH session."""
    if getattr(dev, "ssh_port", None) is not None:
        return dev.ssh_port, None
    if not shutil.which("iproxy") or not shutil.which("ssh"):
        return None, "iproxy/ssh not on PATH"
    port = free_port(cfg.proxy_lo, cfg.proxy_hi)
    cfg.askpass = os.path.join(cfg.out, "askpass")
    with open(cfg.askpass, "w") as f:
        f.write("#!/bin/sh\nprintf '%s\\n' %s\n" % ("%s", shlex.quote(
            os.environ.get("DEVICE_PASSWORD", "alpine"))))
    os.chmod(cfg.askpass, 0o700)
    procs.spawn(["iproxy", str(port), "22"],
                os.path.join(dev.dir, "iproxy-launch.log"), env=mux_env(cfg))
    time.sleep(2)
    probe = guest_ssh(cfg, port, ["true"], timeout=40)
    if probe.returncode != 0:
        return None, "no ssh on guest: %s" % probe.stderr.strip()[-120:]
    dev.ssh_port = port
    return port, None


def prepare_launcher(cfg, procs, dev, r):
    launcher = os.path.join(GLES_DIR, "sblaunch")
    if not os.path.exists(launcher):
        r.skip("requires built contrib/it-gles/sblaunch")
        return None
    port, error = ensure_guest_ssh(cfg, procs, dev)
    if port is None:
        r.skip(error)
        return None
    if getattr(dev, "launcher_ready", False):
        return port
    p = guest_ssh(cfg, port, None, timeout=300, scp_from=launcher, scp_to="/tmp/sblaunch")
    if p.returncode != 0:
        r.set(False, "scp sblaunch failed: %s" % p.stderr.strip()[-160:])
        return None
    p = guest_ssh(cfg, port, ["chmod 755 /tmp/sblaunch"])
    if p.returncode != 0:
        r.set(False, "could not make sblaunch executable")
        return None
    dev.launcher_ready = True
    return port


class AgentControl:
    """A selected command session. Submitted RPCs are never replayed via SSH."""
    def __init__(self, qmp):
        self.qmp = qmp


def prepare_app_control(cfg, procs, dev, result):
    if itqmp.agent_alive(dev.qmp):
        return AgentControl(dev.qmp)
    return prepare_launcher(cfg, procs, dev, result)


def control_exec(cfg, control, command, timeout=60):
    if not isinstance(control, AgentControl):
        return guest_ssh(cfg, control, [command], timeout=timeout)
    status, data = itqmp.agent(control.qmp, "exec", command, timeout=timeout)
    return subprocess.CompletedProcess(["agent", "exec", command], status,
                                       data.decode("utf-8", "replace"), "")


def springboard(cfg, port, request, timeout=60):
    if isinstance(port, AgentControl):
        op = {":frontmost": "frontmost", ":lock-status": "lockstatus"}.get(request, "launch")
        status, data = itqmp.agent(port.qmp, op, request if op == "launch" else "", timeout=timeout)
        text = data.decode("utf-8", "replace")
        if status == 0:
            if op == "frontmost":
                lines = text.splitlines()
                text = "sblaunch: frontmost=" + (lines[0] if lines else "")
            elif op == "lockstatus":
                text = "sblaunch: " + text
        return subprocess.CompletedProcess(["agent", op, request], status, text, "")
    return guest_ssh(cfg, port, ["printf '%s' %s > /tmp/sblaunch.id && /tmp/sblaunch"
                               % ("%s", shlex.quote(request))], timeout=timeout)


def foreground_is(cfg, port, bundle_id):
    p = springboard(cfg, port, ":frontmost")
    return p.returncode == 0 and p.stdout.strip() == "sblaunch: frontmost=" + bundle_id


def check_webproxy(cfg, procs, dev, result):
    """Native NSURLConnection must reach the host without resolving the origin."""
    import http.server
    import threading
    helpers = [os.path.join(ROOT, "contrib", "it-proxy", name)
               for name in ("itproxy", "httpget")]
    if not all(os.path.exists(p) for p in helpers):
        return result.skip("requires built contrib/it-proxy helpers")
    port, error = ensure_guest_ssh(cfg, procs, dev)
    if port is None:
        return result.set(False, error)
    class Fixture(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"LIGHTTOUCH_PROXY_NATIVE_PASS")
        def log_message(self, *args):
            pass
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    enabled = False
    try:
        for helper in helpers:
            copied = guest_ssh(cfg, port, None, scp_from=helper,
                               scp_to="/tmp/" + os.path.basename(helper))
            if copied.returncode:
                return result.set(False, "could not stage proxy test helper")
        with open(cfg.web_proxy_config, "w") as f:
            f.write("upstream\n127.0.0.1\n%d\n" % server.server_port)
        changed = guest_ssh(cfg, port, ["chmod 755 /tmp/itproxy /tmp/httpget && /tmp/itproxy on"])
        if changed.returncode:
            return result.set(False, "guest proxy configuration failed: " + changed.stderr[-200:])
        enabled = True
        time.sleep(8)
        response = guest_ssh(cfg, port, ["/tmp/httpget http://example.invalid/fixture"], timeout=90)
        if response.returncode or "HTTP 200" not in response.stdout or "LIGHTTOUCH_PROXY_NATIVE_PASS" not in response.stdout:
            return result.set(False, response.stdout.strip() or response.stderr[-200:])
        with open(cfg.web_proxy_config, "w") as f:
            f.write("direct\n")
        for host in ("api.openfeint.com", "gdata.youtube.com"):
            started = time.monotonic()
            response = guest_ssh(cfg, port, ["/tmp/httpget http://" + host + "/"], timeout=30)
            elapsed = time.monotonic() - started
            if response.returncode or "HTTP 410" not in response.stdout or elapsed >= 10:
                return result.set(False, "retired service did not fail promptly: " + response.stdout.strip())
        result.set(True, "native HTTP fixture and prompt retired-service HTTP 410 responses")
    finally:
        if enabled:
            restored = guest_ssh(cfg, port, ["/tmp/itproxy off"])
            if restored.returncode:
                result.set(False, "failed to restore guest proxy preferences")
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


def check_serial_console(dev, result):
    """Require real XNU output, not iBoot echoing the requested arguments."""
    try:
        with open(dev.serial, "rb") as f:
            serial = f.read()
    except OSError as exc:
        return result.set(False, "cannot read kernel console: %s" % exc)
    if (b"AppleS5L8720XFMSS::start:" in serial and
            b"BSD root: disk0s1" in serial):
        return result.set(True, "XNU driver startup and BSD mount logged on serial")
    return result.set(False, "serial log lacks XNU driver startup or BSD mount")


def check_respring(cfg, procs, dev, result):
    """Restart SpringBoard in this boot, retaining installation pressure.

    A cold restart clears session-only NAND mappings and can hide corruption.
    Require a service response from the new SpringBoard; successful killall
    and a still-lit old framebuffer are not recovery evidence.
    """
    port = prepare_app_control(cfg, procs, dev, result)
    if port is None:
        return False
    dev.qmp.cmd("query-status")  # drain events from before this operation
    resets = dev.qmp.reset_count
    p = control_exec(cfg, port, "killall SpringBoard", timeout=10)
    if p.returncode != 0:
        return result.set(False, "could not restart SpringBoard: %s" %
                          (p.stdout + p.stderr).strip()[-200:])
    deadline = time.monotonic() + 45
    failure = "SpringBoard did not recover within 45s"
    while time.monotonic() < deadline:
        time.sleep(2)
        try:
            p = springboard(cfg, port, ":lock-status",
                            timeout=min(8, max(1, deadline - time.monotonic())))
        except (TimeoutError, RuntimeError) as error:
            # Read-only readiness probes may be retried while SBS restarts.
            p = subprocess.CompletedProcess(["lockstatus"], 124, "", str(error))
        dev.qmp.cmd("query-status")
        if dev.qmp.reset_count != resets:
            failure = "guest reset during SpringBoard restart"
            break
        if p.returncode == 0 and p.stdout.strip().startswith("sblaunch: locked="):
            return result.set(True, "SpringBoard service recovered in the same boot")
    diagnostic = control_exec(cfg, port,
        "launchctl list; cat /var/mobile/Library/Logs/CrashReporter/LatestCrash-SpringBoard.plist",
        timeout=15)
    path = os.path.join(dev.dir, "respring-diagnostics.txt")
    with open(path, "w") as f:
        f.write("Last service probe: rc=%s\n%s\n%s\n" %
                (p.returncode, p.stdout, p.stderr))
        f.write(diagnostic.stdout + "\n" + diagnostic.stderr)
    return result.set(False, "%s; see %s" % (failure, path))


def check_applaunch(cfg, procs, dev, r):
    port = prepare_app_control(cfg, procs, dev, r)
    if port is None:
        return False
    bundle_id = ipa_bundle_id(cfg.ipa)
    ok, detail = unlock(cfg, port, dev)
    if not ok:
        return r.set(False, detail)
    p = springboard(cfg, port, bundle_id)
    if p.returncode != 0:
        return r.set(False, "launch refused: %s" % (p.stdout + p.stderr).strip()[-200:])
    if getattr(cfg, "launch_stages", False):
        started = time.monotonic()
        for seconds in (5, 20):
            time.sleep(max(0, started + seconds - time.monotonic()))
            capture = dev.qmp.shot(os.path.join(dev.dir, "app-%ds.ppm" % seconds))
            to_png(capture, os.path.join(dev.dir, "app-%ds.png" % seconds))
        time.sleep(max(0, started + 30 - time.monotonic()))
    else:
        time.sleep(30)
    shot = dev.qmp.shot(os.path.join(dev.dir, "app.ppm"))
    to_png(shot, os.path.join(dev.dir, "app.png"))
    _hi, lit = lit_count(shot)
    if not foreground_is(cfg, port, bundle_id):
        return r.set(False, "%s is not the foreground app after launch" % bundle_id)
    if lit < 20000:
        return r.set(False, "%s is foreground but screen is dark (lit=%d)" % (bundle_id, lit))
    return r.set(True, "%s verified foreground, lit=%d" % (bundle_id, lit))


def quad_signature(path):
    """(magenta, cyan, yellow) fractions of a frame.

    Classified relative to the frame's own maximum sample rather than against
    absolute RGB, because the panel backlight scales every pixel: without
    IT_LCD_BRIGHT=255 the same frame arrives several times darker, and fixed
    thresholds would read it as black.
    """
    try:
        _w, _h, px = read_ppm(path)
    except Exception:
        return 0.0, 0.0, 0.0
    if not px:
        return 0.0, 0.0, 0.0
    hi = max(px) or 1
    lo, up = 0.3 * hi, 0.7 * hi
    m = c = y = 0
    for i in range(0, len(px), 3):
        r, g, b = px[i], px[i + 1], px[i + 2]
        if r >= up and b >= up and g <= lo:
            m += 1
        elif g >= up and b >= up and r <= lo:
            c += 1
        elif r >= up and g >= up and b <= lo:
            y += 1
    n = len(px) / 3.0
    return m / n, c / n, y / n


def install_gles_app(cfg, r):
    """Install GLTest.app over USB, as an .ipa, and confirm installd lists it.

    The .ipa is built here rather than committed: it is just Payload/ around
    the bundle contrib/it-gles/build.sh already produces, and a stale one would
    silently test an old binary.
    """
    if app_is_installed(cfg, GLES_BUNDLE_ID):
        return True
    ipa_dir = os.path.join(cfg.out, "glesipa")
    shutil.rmtree(ipa_dir, ignore_errors=True)
    os.makedirs(os.path.join(ipa_dir, "Payload"))
    shutil.copytree(os.path.join(GLES_DIR, "GLTest.app"),
                    os.path.join(ipa_dir, "Payload", "GLTest.app"))
    ipa = os.path.join(cfg.out, "GLTest.ipa")
    # zip(1) rather than shutil.make_archive: the executable bit has to survive
    # into the archive or the app installs and only ever bounces.
    z = subprocess.run(["zip", "-qr", ipa, "Payload"], cwd=ipa_dir,
                       capture_output=True, text=True, timeout=120)
    if z.returncode != 0:
        return r.set(False, "could not build GLTest.ipa: %s"
                     % z.stderr.strip()[-160:])
    ins = run(["ideviceinstaller", "install", ipa], cfg, cfg.install_timeout)
    if not app_is_installed(cfg, GLES_BUNDLE_ID):
        return r.set(False, "GLTest did not install: %s"
                     % (ins.stdout + ins.stderr).strip().replace("\n", " | ")[-250:])
    return True


def unlock(cfg, port, dev, tries=UNLOCK_TRIES):
    """Swipe only after SpringBoard confirms that the screen is locked.

    Pixel totals vary with the installed icons and cannot identify a lock
    screen. Unknown lock state fails rather than dragging an unlocked dock.
    """
    for attempt in range(tries + 1):
        dev.qmp.home()
        time.sleep(2)
        p = springboard(cfg, port, ":lock-status")
        status = re.fullmatch(r"sblaunch: locked=([01]) passcode=([01])", p.stdout.strip())
        if p.returncode != 0 or status is None:
            return False, "SpringBoard lock status unavailable: %s" % (p.stdout + p.stderr).strip()[-160:]
        if status[1] == "0":
            return True, "SpringBoard reports unlocked"
        if status[2] == "1":
            return False, "device has a passcode; unlock manually"
        if attempt == tries:
            break
        dev.qmp.swipe(60, 427, 295, 427)
        time.sleep(3)
    return False, "device remained locked after %d attempts" % tries


def slot_names():
    """slot -> glFunctionName, from contrib/it-gles/slotmap.txt.

    The shim can only print a bare integer (it has no string table and barely a
    libc), so the translation happens here, at check time. Regenerate the map
    with `genstubs.py --emit-map`; it self-validates against the GLES_SLOT_*
    defines in include/hw/arm/guest-services/gles.h before writing anything.
    """
    names = {}
    try:
        with open(os.path.join(GLES_DIR, "slotmap.txt")) as f:
            for line in f:
                if line.startswith("#"):
                    continue
                n, _, name = line.strip().partition(" ")
                if name:
                    names[int(n)] = name
    except OSError:
        pass
    return names


def guest_ssh(cfg, port, argv, timeout=60, scp_from=None, scp_to=None):
    """One ssh/scp to the guest through an already-running iproxy.

    Password auth cannot read from a pipe, so the password goes through
    SSH_ASKPASS exactly as imgtools/install-ipa.sh does.
    """
    # Reuse the installer's multiplexing pattern: short-lived SSH connections
    # can race teardown in the old guest stack. Keep the Unix path below 104B.
    if not hasattr(cfg, "ssh_control"):
        cfg.ssh_control = tempfile.TemporaryDirectory(prefix="itssh-", dir="/tmp")
    opts = ["-o", "ControlMaster=auto", "-o", "ControlPersist=30",
            "-o", "ControlPath=" + os.path.join(cfg.ssh_control.name, "%C"),
            "-o", "StrictHostKeyChecking=no",
            "-o", "UserKnownHostsFile=/dev/null",
            "-o", "LogLevel=ERROR",
            "-o", "PreferredAuthentications=password",
            "-o", "ConnectTimeout=10"]
    env = dict(os.environ)
    env.update(SSH_ASKPASS=cfg.askpass, SSH_ASKPASS_REQUIRE="force",
               DISPLAY=os.environ.get("DISPLAY", ":0"))
    if scp_from:
        cmd = ["scp", "-O", "-r"] + opts + ["-P", str(port), scp_from,
                                            "root@127.0.0.1:" + scp_to]
    else:
        cmd = ["ssh"] + opts + ["-p", str(port), "root@127.0.0.1"] + argv
    try:
        return subprocess.run(cmd, env=env, timeout=timeout,
                              capture_output=True, text=True)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(cmd, 124, "", "timed out")


def check_agent(cfg, procs, dev, r):
    """Exercise the production command tunnel without USB or SSH."""
    deadline = time.monotonic() + 60
    while not itqmp.agent_alive(dev.qmp):
        if time.monotonic() >= deadline:
            return r.set(False, "guest agent did not become ready within 60 seconds")
        time.sleep(1)
    remote = "/tmp/regress-agent-" + os.urandom(12).hex()
    payload = os.urandom(70 * 1024)
    try:
        for op, args, body, expected in (
            ("ping", "", b"", b"it_agent v1\n"),
            ("exec", "echo $((6*7))", b"", b"42\n"),
            ("put", remote + " 600", payload, b""),
            ("get", remote, b"", payload),
        ):
            status, response = itqmp.agent(dev.qmp, op, args, body)
            if status != 0 or response != expected:
                return r.set(False, "agent %s failed: status=%d, response bytes=%d" %
                             (op, status, len(response)))
        return r.set(True, "ping, shell arithmetic and 70 KiB binary round trip")
    finally:
        itqmp.agent(dev.qmp, "exec", "rm -f " + remote)


def check_audio(cfg, procs, dev, r):
    """Start the bundled stereo fixture; validate the finalized WAV after shutdown."""
    port = prepare_app_control(cfg, procs, dev, r)
    if port is None:
        return False
    if not app_is_installed(cfg, "com.qemuios.harness"):
        install = run(["ideviceinstaller", "install", HARNESS_IPA], cfg, cfg.install_timeout)
        if install.returncode or not app_is_installed(cfg, "com.qemuios.harness"):
            return r.set(False, "Harness installation failed: " + install.stderr[-200:])
    ok, detail = unlock(cfg, port, dev)
    if not ok:
        return r.set(False, detail)
    # Each run starts at row zero, even after prior app or audio checks.
    stopped = control_exec(cfg, port, "killall Harness", timeout=10)
    if stopped.returncode not in (0, 1):
        return r.set(False, "could not reset the Harness menu")
    response = springboard(cfg, port, "com.qemuios.harness")
    if response.returncode:
        return r.set(False, "Harness launch failed: " + response.stderr[-200:])
    time.sleep(4)
    if not foreground_is(cfg, port, "com.qemuios.harness"):
        return r.set(False, "Harness is not the foreground app")
    # Establish audible guest volume independently of saved NAND preferences.
    for _ in range(16):
        itqmp.button(dev.qmp, "volup", hold_ms=100)
    time.sleep(2)
    itqmp.move(dev.qmp, 160, 290)
    dev.qmp.cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
    for step in range(1, 27):
        itqmp.move(dev.qmp, 160, 290 - step * 5)
        time.sleep(.05)
    time.sleep(.5)
    dev.qmp.cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
    time.sleep(.5)
    dev.qmp.tap(160, 211)
    time.sleep(.25)
    to_png(dev.qmp.shot(os.path.join(dev.dir, "audio.ppm")), os.path.join(dev.dir, "audio.png"))
    if itqmp.agent_alive(dev.qmp):
        status, tree = itqmp.agent(dev.qmp, "uidump")
        with open(os.path.join(dev.dir, "audio-ui.txt"), "wb") as output:
            output.write(tree)
        if status or b"RUNNING audio stereo.wav" not in tree:
            return r.set(False, "Harness did not start stereo PCM; see audio-ui.txt")
    time.sleep(8)
    dev.qmp.tap(50, 39)  # Stop
    dev.qmp.home()
    log("  audio: playback requested; awaiting finalized host WAV")
    return True


def verify_audio(path, r):
    import wave
    import numpy as np
    with wave.open(path, "rb") as recording:
        if recording.getnchannels() != 2 or recording.getsampwidth() != 2:
            return r.set(False, "expected stereo 16-bit PCM")
        rate = recording.getframerate()
        samples = np.frombuffer(recording.readframes(recording.getnframes()), dtype="<i2").reshape(-1, 2)
    active = np.max(np.abs(samples.astype(np.int32)), axis=1) > 100
    seconds = np.count_nonzero(active) / rate
    if seconds < 4:
        return r.set(False, "only %.2f seconds of non-silent audio" % seconds)
    peaks = []
    for channel, expected in enumerate((440, 880)):
        spectrum = np.abs(np.fft.rfft(samples[:, channel]))
        hz = (np.argmax(spectrum[1:]) + 1) * rate / len(samples)
        peaks.append(hz)
        if abs(hz - expected) > 5:
            return r.set(False, "channel %d peak %.1f Hz; expected %d Hz" % (channel, hz, expected))
    return r.set(True, "%.2f seconds; left %.1f Hz, right %.1f Hz" % (seconds, *peaks))


def check_gles(cfg, procs, dev, r):
    """Prove the OpenGL ES HLE layer still renders, on the real panel.

    The frame is never hashed and never compared to a golden image: host GPUs
    differ, and a pixel-exact reference would be flaky on every machine but the
    one that recorded it. What no host GPU can change is *which colours* come
    out of `glClear(magenta); glDrawArrays(cyan quad)` -- both are flat, both
    are unfiltered, and glapp.c picked them because nothing in the iOS UI
    produces either. So the assertion is the colour signature, held across two
    samples, plus a scan of the shim's own unimplemented-slot log.
    """
    app = os.path.join(GLES_DIR, "GLTest.app")
    shim = os.path.join(GLES_DIR, "MBXGLEngine")
    launcher = os.path.join(GLES_DIR, "sblaunch")
    harness = not os.path.exists(app)
    bundle_id = "com.qemuios.harness" if harness else GLES_BUNDLE_ID
    prerequisites = [HARNESS_IPA if harness else app]
    if getattr(cfg, "stage_gles_shim", False):
        prerequisites.extend([shim, launcher])
    missing = [os.path.basename(path) for path in prerequisites if not os.path.exists(path)]
    if missing:
        return r.skip("build the guest fixtures first (no %s)" % ", ".join(missing))
    port = (prepare_launcher if getattr(cfg, "stage_gles_shim", False) else prepare_app_control)(cfg, procs, dev, r)
    if port is None:
        return False

    # Install GLTest properly rather than dropping it in /Applications. A
    # hand-copied bundle is not installed: SpringBoard launches from installd's
    # database, and installd adopts a directory that appeared behind its back
    # only sporadically -- measured 4 times out of 8 identical runs, with pokes
    # over 200s failing to force it. `ideviceinstaller install` is the path
    # this image was built to accept and it registers first time, every time.
    if harness:
        if not app_is_installed(cfg, bundle_id):
            installed = run(["ideviceinstaller", "install", HARNESS_IPA], cfg, cfg.install_timeout)
            if installed.returncode or not app_is_installed(cfg, bundle_id):
                return r.set(False, "Harness installation failed")
    elif not install_gles_app(cfg, r):
        return False
    if getattr(cfg, "stage_gles_shim", False):
        p = guest_ssh(cfg, port, None, timeout=300, scp_from=shim, scp_to="/tmp/MBXGLEngine")
        if p.returncode != 0:
            return r.set(False, "scp MBXGLEngine failed: %s" % p.stderr.strip()[-160:])
        # The stock bundle is kept alongside ours so a later manual run can restore
        # it; /System is why this goes over ssh and not AFC.
        bundle = ("/System/Library/Frameworks/OpenGLES.framework/"
                  "MBXGLEngine.bundle")
        p = guest_ssh(cfg, port, [
            "set -e; "
            "chmod 755 /tmp/sblaunch; "
            "if [ ! -f {b}/MBXGLEngine.stock ]; then "
            "cp {b}/MBXGLEngine {b}/MBXGLEngine.stock; fi; "
            "cp /tmp/MBXGLEngine {b}/MBXGLEngine; chmod 755 {b}/MBXGLEngine; "
            "printf %s {id} > /tmp/sblaunch.id".format(b=bundle,
                                                       id=GLES_BUNDLE_ID)],
            timeout=120)
        if p.returncode != 0:
            return r.set(False, "staging the shim failed: %s"
                         % (p.stdout + p.stderr).strip()[-200:])

    ok, detail = unlock(cfg, port, dev)
    if not ok:
        return r.set(False, detail)
    if harness:
        stopped = control_exec(cfg, port, "killall Harness", timeout=10)
        if stopped.returncode not in (0, 1):
            return r.set(False, "could not reset the Harness menu")
    p = springboard(cfg, port, bundle_id)
    out = (p.stdout + p.stderr).strip()
    if p.returncode != 0:
        return r.set(False, "sblaunch refused: %s" % out[-200:])
    log("  gles: %s" % out)

    if harness:
        time.sleep(4)
        dev.qmp.tap(150, 79)  # First menu row: deterministic GL scene
    time.sleep(GLES_SETTLE_S)
    if not foreground_is(cfg, port, bundle_id):
        return r.set(False, "GLES fixture is not the foreground app after launch")
    a = dev.qmp.shot(os.path.join(dev.dir, "gles-a.ppm"))
    time.sleep(GLES_HOLD_S)
    b = dev.qmp.shot(os.path.join(dev.dir, "gles-b.ppm"))
    to_png(a, os.path.join(dev.dir, "gles-a.png"))
    to_png(b, os.path.join(dev.dir, "gles-b.png"))
    ma, ca, ya = quad_signature(a)
    mb, cb, yb = quad_signature(b)
    _hi, lit = lit_count(b)

    # Slot scan. The shim writes to fd 2, which for a SpringBoard-launched app
    # goes nowhere addressable on a stock image: measured on 3.1.3, there is no
    # /var/log/syslog and nothing reaches the QEMU or serial log either. So an
    # empty scan is *reported* in the verdict rather than passed over in
    # silence, and the assertion above is what actually carries the check.
    # ponytail: no log source for the slot trace on 3.1.3; if this needs to
    # become a real gate, have mbxshim write the line to a file under /tmp and
    # cat it back here, or route gles_unimpl through a guest-services call.
    text = dev.serial_text()
    for extra in (os.path.join(dev.dir, "qemu.log"),):
        try:
            with open(extra, "rb") as f:
                text += f.read().decode("utf-8", "replace")
        except OSError:
            pass
    text += control_exec(cfg, port, "cat /var/log/syslog 2>/dev/null",
                      timeout=60).stdout
    seen = set(int(n) for n in re.findall(r"unimplemented slot (\d+)", text))
    new = sorted(seen - GLES_ALLOWED_SLOTS)
    names = slot_names()

    if new:
        return r.set(False, "the app called %d unimplemented entry point(s): %s"
                     % (len(new), ", ".join("%d (%s)"
                                            % (n, names.get(n, "slot %d?" % n))
                                            for n in new)))
    if min(ma, ca) < GLES_QUAD_MIN:
        return r.set(False, "GLES fixture scene is not on the panel: magenta=%.3f "
                            "cyan=%.3f of the frame, need >=%.3f of each "
                            "(lit=%d, so the screen is %s)"
                     % (ma, ca, GLES_QUAD_MIN, lit,
                        "showing something else - SpringBoard, most likely"
                        if lit > 20000 else "dark"))
    if not harness and min(ya, yb) < GLES_QUAD_MIN:
        return r.set(False, "GLES fixture's glDrawTexfOES quad is missing: yellow=%.3f/%.3f "
                            "of the frame, need >=%.3f (magenta=%.3f cyan=%.3f)"
                     % (ya, yb, GLES_QUAD_MIN, mb, cb))
    if min(mb, cb) < GLES_QUAD_MIN:
        return r.set(False, "the scene rendered and then vanished within %ds "
                            "(magenta %.3f->%.3f, cyan %.3f->%.3f): the "
                            "renderer wedged after its first present"
                     % (GLES_HOLD_S, ma, mb, ca, cb))
    return r.set(True, "GLES fixture rendering through the HLE layer: magenta=%.3f "
                       "cyan=%.3f yellow=%.3f, held for %ds, lit=%d%s"
                 % (mb, cb, yb, GLES_HOLD_S, lit,
                    "" if "unimplemented slot" in text
                    else " (no log source carried the slot trace)"))


def check_persist(cfg, dev2, marker_src, remote, r, event="clean shutdown + reboot"):
    # The directory has to exist before afcclient can create the local file:
    # it reports a missing *local* directory with exactly the same "No such
    # file or directory" it uses for a missing *remote* file, so without this
    # the check reads as "the guest lost the file" whenever `afc` -- which is
    # what used to create this directory -- was not among the selected checks.
    dst = os.path.join(cfg.out, "afc", "persist-back.bin")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.exists(dst):
        os.remove(dst)
    g = afc(cfg, ["get -f %s %s" % (remote, dst)])
    if not os.path.exists(dst):
        # Distinguish the two failures the message above conflates: ask the
        # guest whether the file is in its directory listing at all.
        ls = afc(cfg, ["ls /"])
        seen = os.path.basename(remote) in (ls.stdout + ls.stderr)
        return r.set(False, "%s: %s"
                     % ("file gone after reboot" if not seen else
                        "the guest still lists the file but the read failed",
                        (g.stdout + g.stderr).strip()[-200:]))
    if sha256_file(dst) != sha256_file(marker_src):
        return r.set(False, "file survived but the contents changed")
    return r.set(True, "%s identical after %s" % (remote, event))


def check_restart(cfg, procs, dev, result):
    """Reset the running machine after writes, preserving its overlay in-process."""
    port, error = ensure_guest_ssh(cfg, procs, dev)
    if port is None:
        return result.set(False, error)
    src = os.path.join(dev.dir, "restart-marker.bin")
    remote = "/regress_restart.bin"
    with open(src, "wb") as f:
        # Cross the old 512-page write-script limit and end on a partial page.
        f.write(os.urandom(4 * 1024 * 1024 + 65535))
    put = afc(cfg, ["put -f %s %s" % (src, remote)])
    if put.returncode or guest_ssh(cfg, port, ["sync"]).returncode:
        return result.set(False, "could not write and sync restart marker")
    dev.qmp.cmd("system_reset")
    ok, detail, _ = dev.wait_for_home(cfg.boot_timeout)
    if not ok:
        return result.set(False, "warm reset: " + detail)
    udid, detail = wait_for_device(cfg)
    if not udid:
        return result.set(False, "USB after warm reset: " + detail)
    return check_persist(cfg, dev, src, remote, result, event="warm reset")


def hfs_volume_blocks(first_page):
    """Validate the header before using its size to construct a host file."""
    if len(first_page) != 4096 or first_page[1024:1026] not in (b"H+", b"HX"):
        raise ValueError("missing HFS+ volume header in allocation block 0")
    block_size, total_blocks = struct.unpack_from(">II", first_page, 1024 + 40)
    if block_size != 4096 or not 1 <= total_blocks <= 4 * 4096 * 128:
        raise ValueError("unsupported HFS+ geometry: blockSize=%d totalBlocks=%d"
                         % (block_size, total_blocks))
    return total_blocks


def compose_fsck_volume(base, overlay, destination):
    """Compose the full generated-layout volume; absent pages remain sparse zeros."""
    from ftlmap import predict

    if not os.path.isdir(base):
        raise ValueError("fsck composition requires a NAND page directory")

    def index(directory):
        pages, erased = {}, set()
        for cs in range(4):
            try:
                entries = os.scandir(os.path.join(directory, "cs%d" % cs))
            except FileNotFoundError:
                continue
            with entries:
                for entry in entries:
                    page = re.fullmatch(r"([0-9]+)\.page", entry.name)
                    marker = re.fullmatch(r"blk([0-9]+)\.erased", entry.name)
                    if page:
                        pages[cs, int(page.group(1))] = entry.path
                    elif marker:
                        erased.add((cs, int(marker.group(1))))
        return pages, erased

    base_pages, _ = index(base)
    overlay_pages, erased = index(overlay)
    if "FMSS_ERASE" not in os.environ:
        erased.clear()  # Match the optional erase model used by boot_env().
    zero = bytes(4096)

    def read_page(key):
        path = overlay_pages.get(key)
        if path is None and (key[0], key[1] // 128) not in erased:
            path = base_pages.get(key)
        if path is None:
            return zero
        with open(path, "rb") as page:
            data = page.read(4096)
        if len(data) != 4096:
            raise ValueError("short NAND page: %s (%d bytes)" % (path, len(data)))
        return data

    total_blocks = hfs_volume_blocks(read_page(predict(0)))
    used = 0
    with open(destination, "wb") as out:
        out.truncate(total_blocks * 4096)
        for block in range(total_blocks):
            key = predict(block)
            used += key in overlay_pages
            data = read_page(key)
            if data != zero:
                out.seek(block * 4096)
                out.write(data)
    return total_blocks, used


def check_fsck(cfg, clean_stop, r):
    """Check every allocation block; a nonzero fsck result is always a failure."""
    img = os.path.join(cfg.out, "volume.img")
    try:
        blocks, used = compose_fsck_volume(cfg.base_nand, cfg.overlay, img)
    except (OSError, ValueError) as exc:
        return r.set(False, "cannot compose volume: %s" % exc)
    att = subprocess.run(["hdiutil", "attach", "-imagekey", "diskimage-class=CRawDiskImage",
                          "-nomount", "-readonly", "-noverify", img],
                         capture_output=True, text=True, timeout=300)
    dev = next((tok for tok in att.stdout.split()
                if re.fullmatch(r"/dev/disk[0-9]+(?:s[0-9]+)?", tok)), "")
    if att.returncode != 0 or not dev:
        return r.set(False, "hdiutil attach failed: %s" % att.stderr.strip())
    try:
        fs = subprocess.run(["fsck_hfs", "-n", dev], capture_output=True,
                            text=True, timeout=900)
    finally:
        subprocess.run(["hdiutil", "detach", dev], capture_output=True,
                       timeout=120)
    output = fs.stdout + fs.stderr
    with open(os.path.join(cfg.out, "fsck.log"), "w") as log_file:
        log_file.write(output)
    tail = " | ".join(line.strip() for line in output.strip().splitlines()[-4:])
    return r.set(fs.returncode == 0,
                 "fsck_hfs rc=%d; %d blocks, %d overlay pages%s; %s"
                 % (fs.returncode, blocks, used,
                    "" if clean_stop else "; guest shutdown was not clean", tail))


# --------------------------------------------------------------------------
# driver
# --------------------------------------------------------------------------

def report_prereqs(cfg):
    """List what each tier needs and whether it's there, run nothing.

    cfg.files/cfg.base_nand must already be resolved by the caller.
    """
    items = [
        ("qemu binary", cfg.qemu, "every check", True),
        ("base NAND", cfg.base_nand, "every check", True),
        ("usbmuxd", cfg.usbmuxd,
         "afc, usbtcp, persist, appinstall, applaunch", False),
        ("ipa", cfg.ipa, "appinstall, applaunch (--ipa or place one at "
                         "the default path)", False),
        ("Harness.ipa", HARNESS_IPA, "bundled test application", False),
        ("it_agent", os.path.join(ROOT, "contrib", "it-agent", "it_agent"),
         "agent (must also be installed in the guest NAND)", False),
        ("GLTest.app", os.path.join(GLES_DIR, "GLTest.app"),
         "gles (build it with contrib/it-gles/build.sh)", False),
        ("gles slotmap", os.path.join(GLES_DIR, "slotmap.txt"),
         "gles, to name an unimplemented slot "
         "(genstubs.py --emit-map)", False),
    ]
    hard_missing = False
    for what, path, needed_for, required in items:
        ok = os.path.exists(path)
        if required and not ok:
            hard_missing = True
        print("%-4s  %-12s %s" % ("OK" if ok else "MISS", what, path))
        print("      needed for: %s" % needed_for)
    print("")
    if hard_missing:
        print("default tier (boot, fsck, persist, appinstall, applaunch, gles, agent, audio) CANNOT run: "
              "missing qemu binary and/or base NAND")
    else:
        print("default tier (boot, fsck, persist, appinstall, applaunch, gles, agent, audio) can run "
              "(persist SKIPs without usbmuxd)")
        print("opt-in tier (--with-apps) checks needing usbmuxd/ipa will "
              "SKIP individually if those are still missing")
    return 1 if hard_missing else 0


def main():
    global START
    START = time.time()
    ap = argparse.ArgumentParser(
        description="Regression harness for the emulated iPod touch 2G",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="checks: " + ", ".join(ALL_CHECKS))
    ap.add_argument("--files-dir",
                    default=os.path.expanduser("~/Developer/qemu-ios-files"))
    ap.add_argument("--base-nand", default=None,
                    help="base NAND image dir (default: <files-dir>/nand-current, the shipping image)")
    ap.add_argument("--cpu", default=None,
                    help="-cpu (default: the machine's own, arm1176). Do not "
                         "pass 'max': it NOPs the CP15 WFI XNU idles on, so "
                         "the guest burns a host core doing nothing.")
    ap.add_argument("--mem", default="128M",
                    help="-m (default 128M, what the device has)")
    ap.add_argument("--audio-hw", choices=("auto", "on", "off"), default=None,
                    help="explicit audio-hw machine option; overrides the legacy IT_AUDIO_HW alias")
    for option in ("h264-decode", "scaler-decode", "mpvd-decode", "lcd-planes"):
        ap.add_argument("--" + option, choices=("on", "off"), default=None,
                        help="explicit " + option + " startup option")
    ap.add_argument("--amc-mode", choices=("registers", "handshake", "decode"), default=None,
                    help="explicit AMC startup mode")
    ap.add_argument("--nor", default=None,
                    help="NOR image (default <files-dir>/ios3/nor_7E18.bin if "
                         "present, else <files-dir>/nor_n72ap.bin)")
    native_qemu = os.path.join(ROOT, "build-native14", "qemu-build", "qemu-system-arm")
    default_qemu = native_qemu if os.path.exists(native_qemu) else os.path.join(ROOT, "build", "qemu-system-arm")
    ap.add_argument("--qemu", default=os.environ.get("QEMU", default_qemu))
    ap.add_argument("--usbmuxd",
                    default=os.path.expanduser(
                        "~/Developer/usbmuxd-qemu/usbmuxd/src/usbmuxd"))
    ap.add_argument("--ipa", default=APP_IPA_DEFAULT)
    ap.add_argument("--ledger", metavar="DIRECTORY",
                    help="test each IPA in a directory on its own disposable guest and write a review ledger")
    ap.add_argument("--launch-stages", action="store_true",
                    help="save additional app screenshots 5 and 20 seconds after launch")
    ap.add_argument("--stage-gles-shim", action="store_true",
                    help="replace the guest GLES shim in the disposable overlay")
    ap.add_argument("--out", default=None, help="run directory")
    ap.add_argument("--checks", default=None,
                    help="comma-separated subset, any tier (default: "
                         "the default tier, or all checks with --with-apps)")
    ap.add_argument("--quick", action="store_true",
                    help="boot + afc only, single boot")
    ap.add_argument("--with-apps", action="store_true",
                    help="also run the opt-in tier: " +
                         ", ".join(OPT_IN_CHECKS))
    ap.add_argument("--check-prereqs", action="store_true",
                    help="report which artefacts are missing for each tier "
                         "and exit, without running anything")
    ap.add_argument("--boot-timeout", type=int, default=900,
                    help="seconds to wait for the home screen (default 900; "
                         "boots are ~3 min unloaded and much longer under "
                         "contention)")
    ap.add_argument("--install-timeout", type=int, default=420,
                    help="seconds to wait for ideviceinstaller (default 420; "
                         "it has no internal timeout and hangs forever if "
                         "installd drops the connection)")
    ap.add_argument("--qemu-port-lo", type=int, default=1511)
    ap.add_argument("--qemu-port-hi", type=int, default=1519)
    ap.add_argument("--mux-port-lo", type=int, default=27311)
    ap.add_argument("--mux-port-hi", type=int, default=27319)
    ap.add_argument("--qmp-port-lo", type=int, default=28001)
    ap.add_argument("--qmp-port-hi", type=int, default=28019)
    ap.add_argument("--proxy-port-lo", type=int, default=28101)
    ap.add_argument("--proxy-port-hi", type=int, default=28119)
    ap.add_argument("--clean", action="store_true",
                    help="remove the run directory (screendumps, PPMs, "
                         "~512MB volume.img) if every selected check passed "
                         "or was skipped")
    cfg = ap.parse_args()
    if cfg.ledger:
        if cfg.checks or cfg.quick or cfg.with_apps or cfg.clean or cfg.check_prereqs:
            ap.error("--ledger runs boot/install/launch checks and retains evidence; do not combine it with check selection or --clean")
        from app_ledger import run_ledger
        return run_ledger(cfg)

    cfg.files = os.path.expanduser(cfg.files_dir)
    # NAND, NOR and iBoot are one set and cannot be mixed: nand-canonical is a
    # 2.1.1 image, and against 3.1.3's iBoot its FTL will not even open --
    # "NAND initialisation failed due to format mismatch", "root filesystem
    # mount failed", "Entering recovery mode". That recovery-mode device is the
    # 05ac:1281 with no mux interface described below, so picking the 3.1.3 NOR
    # while leaving the 2.1.1 NAND reports itself as a USB fault several
    # minutes later rather than as the image mismatch it is. Whichever
    # firmware boot_env() and the NOR default choose, the NAND matches it.
    # nand-current is the shipping image; no fallback, so a missing link is
    # reported by the prerequisite check instead of silently testing another.
    # Resolved so the run log names the real image.
    cfg.base_nand = os.path.realpath(cfg.base_nand or os.path.join(cfg.files, "nand-current"))
    # The 3.1.3 NOR, if this checkout has one. 3.x iBoot unwraps the SHSH blob
    # in flash with a UID-derived key, and nor_n72ap.bin (the 2.1.1 NOR) has no
    # wrapped blob: iBoot then prints "load_macho_image: failed to load device
    # tree", drops into recovery mode, and paints the panel solid white. That
    # device answers USB as 05ac:1281 with no AppleUSBMux interface, which is
    # the "device never appeared on the mux" every USB check used to report.
    # boot_env() already picks the matching 3.1.3 iBoot the same way; the NOR
    # has to travel with it.
    cfg.nor = cfg.nor or next(
        (p for p in (os.path.join(cfg.files, "ios3", "nor_7E18.bin"),
                     os.path.join(cfg.files, "nor_n72ap.bin"))
         if os.path.exists(p)), os.path.join(cfg.files, "nor_n72ap.bin"))

    if cfg.check_prereqs:
        return report_prereqs(cfg)

    if cfg.quick and cfg.checks:
        sys.exit("--quick and --checks are mutually exclusive")
    if cfg.quick:
        selected = list(QUICK_CHECKS)
    elif cfg.checks:
        selected = [c.strip() for c in cfg.checks.split(",")]
    else:
        selected = list(DEFAULT_CHECKS) + (OPT_IN_CHECKS if cfg.with_apps
                                           else [])
    for c in selected:
        if c not in ALL_CHECKS:
            sys.exit("unknown check %r; known: %s" % (c, ", ".join(ALL_CHECKS)))
    if "applaunch" in selected and "appinstall" not in selected:
        selected.insert(selected.index("applaunch"), "appinstall")
    # boot is a precondition for everything else: nothing can be measured on a
    # device that never came up, and reporting the rest as PASS would be a lie.
    # Unconditional, not `len(selected) > 1`: main() always boots and always
    # records results["boot"], so a single non-boot check (`--checks fsck`) used
    # to KeyError there.
    if "boot" not in selected:
        selected.insert(0, "boot")

    cfg.kernel_console = "serial-console" in selected
    cfg.out = cfg.out or os.path.join(
        os.environ.get("TMPDIR", "/tmp"),
        "itregress-%d-%d" % (os.getpid(), int(START)))
    os.makedirs(cfg.out, exist_ok=True)
    cfg.overlay = os.path.join(cfg.out, "overlay")
    if os.path.exists(cfg.overlay):
        shutil.rmtree(cfg.overlay)
    os.makedirs(cfg.overlay)

    # qemu and the base NAND are the whole default tier's only inputs: without
    # them nothing at all can run, so this is still a hard exit.
    for path, what in ((cfg.qemu, "qemu binary"), (cfg.base_nand, "base NAND")):
        if not os.path.exists(path):
            sys.exit("missing %s: %s" % (what, path))

    # usbmuxd and the .ipa are per-check inputs, not run-wide ones: a check
    # that needs one it doesn't have SKIPs instead of taking the whole run
    # down, so a clean checkout without the maintainer's personal files still
    # goes green on the default tier.
    cfg.usbmuxd_ok = os.path.exists(cfg.usbmuxd)
    cfg.ipa_ok = os.path.exists(cfg.ipa)

    cfg.usb_port = free_port(cfg.qemu_port_lo, cfg.qemu_port_hi)
    cfg.mux_port = free_port(cfg.mux_port_lo, cfg.mux_port_hi)
    cfg.qmp_port = free_port(cfg.qmp_port_lo, cfg.qmp_port_hi)
    cfg.proxy_lo, cfg.proxy_hi = cfg.proxy_port_lo, cfg.proxy_port_hi

    results = {c: Result(c) for c in selected}
    skipped = set()
    for c in selected:
        if c in USB_DEPENDENT_CHECKS and not cfg.usbmuxd_ok:
            results[c].skip("usbmuxd binary not found: %s" % cfg.usbmuxd)
            skipped.add(c)
        elif c in IPA_DEPENDENT_CHECKS and not cfg.ipa_ok:
            results[c].skip("ipa not found: %s" % cfg.ipa)
            skipped.add(c)
    if "audio" in selected and "audio" not in skipped:
        if not os.path.exists(HARNESS_IPA):
            results["audio"].skip("Harness.ipa not found: " + HARNESS_IPA)
            skipped.add("audio")
        else:
            try:
                import numpy
            except ImportError:
                results["audio"].skip("NumPy is required for the audio spectrum check")
                skipped.add("audio")
    selected = [c for c in selected if c not in skipped]
    cfg.wifi = "wifi" in selected or "webproxy" in selected
    if "webproxy" in selected:
        cfg.web_proxy_config = os.path.join(cfg.out, "web-proxy.conf")
        with open(cfg.web_proxy_config, "w") as f:
            f.write("off\n")

    log("run dir   %s" % cfg.out)
    log("base nand %s" % cfg.base_nand)
    log("checks    %s" % ", ".join(selected))

    procs = Procs()
    clean_stop = False
    needs_second_boot = "persist" in selected
    marker_src = os.path.join(cfg.out, "persist-marker.bin")
    marker_remote = "/regress_persist.bin"

    try:
        # ---------------- first boot ----------------
        dev = Device(cfg, procs, "boot1")
        dev.start(audio_wav=os.path.join(dev.dir, "audio.wav") if "audio" in selected else None)
        ok, detail, best = dev.wait_for_home(cfg.boot_timeout)
        results["boot"].set(ok, detail if ok else
                            "%s (best lit=%d, need >=%d)"
                            % (detail, best, HOME_LIT_MIN))
        if not ok:
            return finish(results, procs, cfg)

        if "webproxy" in selected:
            check_webproxy(cfg, procs, dev, results["webproxy"])

        if "serial-console" in selected:
            check_serial_console(dev, results["serial-console"])

        if "wifi" in selected:
            check_wifi(cfg, dev, results["wifi"])

        need_usb = any(c in selected for c in USB_DEPENDENT_CHECKS)
        udid = None
        if need_usb:
            udid, pdetail = wait_for_device(cfg)
            log("usbmux: udid=%s (%s)" % (udid, pdetail))
            if not udid:
                for c in USB_DEPENDENT_CHECKS:
                    if c in results:
                        results[c].set(False, "device never appeared on the "
                                              "mux: %s" % pdetail)
                return finish(results, procs, cfg)

        if "afc" in selected:
            check_afc(cfg, dev, results["afc"])
        if "usbtcp" in selected:
            check_usbtcp(cfg, dev, procs, results["usbtcp"])

        if "appinstall" in selected:
            if check_appinstall(cfg, procs, dev, results["appinstall"]):
                if "applaunch" in selected:
                    check_applaunch(cfg, procs, dev, results["applaunch"])
            elif "applaunch" in selected:
                results["applaunch"].set(False, "install failed")

        if "respring" in selected:
            if not check_respring(cfg, procs, dev, results["respring"]):
                return finish(results, procs, cfg)

        audio_ready = False
        if "audio" in selected:
            audio_ready = check_audio(cfg, procs, dev, results["audio"])

        if "agent" in selected:
            check_agent(cfg, procs, dev, results["agent"])

        if "gles" in selected:
            check_gles(cfg, procs, dev, results["gles"])
            dev.qmp.home()              # leave the graphics fixture
            time.sleep(5)

        if "restart" in selected:
            if not check_restart(cfg, procs, dev, results["restart"]):
                return finish(results, procs, cfg)

        if needs_second_boot:
            with open(marker_src, "wb") as f:
                f.write(os.urandom(4 * 1024 * 1024 + 65535))
            p = afc(cfg, ["put -f %s %s" % (marker_src, marker_remote)])
            if p.returncode != 0:
                results["persist"].set(
                    False, "could not write the marker: %s"
                    % (p.stdout + p.stderr)[-200:])
                needs_second_boot = False

        # ---------------- clean shutdown ----------------
        if needs_second_boot or "fsck" in selected:
            if "applaunch" in selected:
                dev.qmp.home()          # leave the app so SpringBoard settles
                time.sleep(5)
            clean_stop = dev.powerdown()
            if not clean_stop:
                log("powerdown did not complete; killing")
                procs.stop(dev.qemu)
        else:
            procs.stop(dev.qemu)
        procs.stop(dev.mux)
        time.sleep(3)

        if "audio" in selected and audio_ready:
            verify_audio(dev.audio_wav, results["audio"])

        # ---------------- second boot ----------------
        if needs_second_boot:
            if not clean_stop:
                results["persist"].set(
                    False, "the guest did not shut down cleanly, so the test "
                           "for surviving a clean shutdown cannot be run")
            else:
                dev2 = Device(cfg, procs, "boot2")
                dev2.start()
                ok2, d2, best2 = dev2.wait_for_home(cfg.boot_timeout)
                log("second boot: %s (%s, best lit=%d)"
                    % ("up" if ok2 else "FAILED", d2, best2))
                if not ok2:
                    results["persist"].set(
                        False, "second boot did not reach the home screen: %s"
                        % d2)
                else:
                    udid2, pd2 = wait_for_device(cfg)
                    if not udid2:
                        results["persist"].set(
                            False, "device did not reappear on the mux: %s"
                            % pd2)
                    else:
                        check_persist(cfg, dev2, marker_src, marker_remote,
                                      results["persist"])
                    clean_stop = dev2.powerdown()
                    if not clean_stop:
                        procs.stop(dev2.qemu)
                procs.stop(dev2.mux)
                time.sleep(2)

        if "fsck" in selected:
            check_fsck(cfg, clean_stop, results["fsck"])

    except KeyboardInterrupt:
        procs.stop_all()
        raise
    except Exception:
        # An exception is a harness failure, not a pass: print it, mark every
        # check that never produced a verdict as failed, and still tear down.
        import traceback
        traceback.print_exc()
        for r in results.values():
            if r.ok is None and not r.skipped:
                r.set(False, "harness error before this check completed")

    return finish(results, procs, cfg)


def finish(results, procs, cfg):
    procs.stop_all()
    with open(os.path.join(cfg.out, "results.json"), "w") as f:
        json.dump({name: {"ok": r.ok, "xfail": r.xfail, "detail": r.detail}
                   for name, r in results.items()}, f, indent=2)
    print("")
    print("=" * 62)
    failed = 0
    for name in ALL_CHECKS:
        r = results.get(name)
        if r is None:
            continue
        if r.ok is None:
            state = "SKIP"
        elif r.ok:
            state = "PASS"
        elif r.xfail:
            state = "XFAIL"
        else:
            state = "FAIL"
            failed += 1
        print("%-4s  %-11s %s" % (state, r.name, r.detail))
    print("=" * 62)
    print("%d check(s) failed; artifacts in %s" % (failed, cfg.out))
    if failed == 0 and getattr(cfg, "clean", False):
        shutil.rmtree(cfg.out, ignore_errors=True)
        print("--clean: removed %s" % cfg.out)
    print("total runtime %.1f min" % ((time.time() - START) / 60.0))
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
