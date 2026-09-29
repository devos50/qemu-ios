#!/bin/bash
# macOS sibling of contrib/ios-app/make-dylib.sh: relink the emulator as a
# dylib a Mac app links directly. Same trick -- take ninja's own link line for
# qemu-system-arm, swap system/main.c (whose main() wants the main thread for
# CFRunLoopRun) for qemu-ios-entry.c, and emit a dylib exporting only the app
# ABI. The app calls qemu_ios_main() on a background thread.
#
#     make-dylib-macos.sh [build-dir]     # default build-min12b
set -eu

SRC="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$SRC/build-min12b}"
LIBNAME="libqemu-arm.dylib"
OUT="$BUILD/$LIBNAME"

cd "$BUILD"

# Compile the app-facing pieces with the same include set the emulator uses.
# glib/pixman come from Homebrew, exactly as the meson build found them.
DEP_CFLAGS="$(pkg-config --cflags glib-2.0 pixman-1)"
# Use the build's own compiler and host CPU, so a cross-compiled x86_64 build
# (the Intel slice of a universal app) compiles these for the same target.
read -r HOST_CPU CC_LINE < <(python3 -c '
import json, shlex
info = "meson-info/intro-"
cpu = json.load(open(info + "machines.json"))["host"]["cpu_family"]
print(cpu, shlex.join(json.load(open(info + "compilers.json"))["host"]["c"]["exelist"]))')
eval "CC_ARGV=($CC_LINE)"
case "$HOST_CPU" in
    aarch64) TCG_HOST=aarch64 ;;
    x86_64) TCG_HOST=i386 ;;
    *) echo "unsupported host CPU: $HOST_CPU" >&2; exit 1 ;;
esac
for f in "$SRC/contrib/ios-app/qemu-ios-entry.c" \
         "$SRC/contrib/ios-app/qemu-ios-ui.c" \
         "$SRC/contrib/macos-app/qemu-macos-extras.c"; do
o="$(basename "${f%.c}").o"
"${CC_ARGV[@]}" -c "$f" -o "$o" \
    -I. -I.. -Iqapi -Itrace -Iui \
    -I"$SRC/contrib/ios-app" -I"$SRC/contrib/macos-app" \
    $DEP_CFLAGS \
    -iquote . -iquote "$SRC" -iquote "$SRC/include" \
    -iquote "$SRC/host/include/$HOST_CPU" -iquote "$SRC/host/include/generic" \
    -iquote "$SRC/tcg/$TCG_HOST" \
    -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -std=gnu11 -O2 \
    -mmacosx-version-min=12.0
done

# Take ninja's own link line so this cannot drift from the real build.
ninja qemu-system-arm >/dev/null
ninja -t commands qemu-system-arm-unsigned | tail -1 | python3 -c '
import shlex, sys
argv = shlex.split(sys.stdin.read())
out = []
skip = False
for a in argv:
    if skip:
        skip = False
        continue
    if a == "-o":
        skip = True                        # drop the executable name
        continue
    if a.endswith("system_main.c.o"):
        out.append("qemu-ios-entry.o")     # main() -> qemu_ios_main()
        out.append("qemu-ios-ui.o")        # frames out, touches in
        out.append("qemu-macos-extras.o")  # keys, pinch, machine controls
        continue
    out.append(a)
# @block.syms / @qemu.syms are exported-symbol response files for an
# executable; a dylib must export our entry points instead.
out = [a for a in out if not a.startswith("@")]
out += ["-dynamiclib", "-o", "'"$OUT"'",
        "-install_name", "@rpath/'"$LIBNAME"'",
        "-Wl,-exported_symbols_list,'"$BUILD"'/macos-exports.syms",
        "-Wl,-undefined,dynamic_lookup"]
print(shlex.join(out))
' > macos-link-dylib.sh

# Only what the app calls; everything else stays private.
cat > macos-exports.syms <<'SYMS'
_qemu_ios_main
_qemu_ios_ui_attach
_qemu_ios_ui_frame
_qemu_ios_ui_frame_size
_qemu_ios_ui_copy_frame
_qemu_ios_ui_touch
_qemu_ios_ui_touch2
_qemu_ios_ui_button
_qemu_ios_ui_key
_qemu_ios_ui_key_mac
_qemu_ios_ui_rotate
_qemu_ios_ui_shake
_qemu_ios_ui_accel
_qemu_ios_ui_attitude
_qemu_ios_ui_battery
_qemu_ios_ui_battery_config
_qemu_ios_ui_usb_connection
_qemu_ios_audio_capture_start
_qemu_ios_audio_capture_read
_qemu_ios_audio_capture_time
_qemu_ios_audio_capture_stop
_qemu_ios_ui_paste
_qemu_ios_agent_request
_qemu_ios_agent_cancel
_qemu_ios_agent_result
_qemu_ios_agent_free_result
_qemu_ios_agent_status
_qemu_ios_gles_contexts
_qemu_ios_build_id
_qemu_ios_ui_pause
_qemu_ios_ui_resume
_qemu_ios_ui_reset
_qemu_ios_ui_powerdown
_qemu_ios_ui_quit
_qemu_ios_snapshot_save
_qemu_ios_snapshot_done
_qemu_ios_ui_ready
_qemu_ios_ui_storage_failed
_qemu_ios_ui_guest_shutdown_confirmed
_qemu_ios_ui_display_sleeping
_qemu_ios_snapshot_save2
_qemu_ios_snapshot_status
_qemu_ios_snapshot_resume
_qemu_ios_set_foreground
_qemu_ios_ui_icon_state_generation
SYMS

sh macos-link-dylib.sh
codesign -f -s - "$OUT"
echo "built $OUT"
nm -gU "$OUT" | head -20
