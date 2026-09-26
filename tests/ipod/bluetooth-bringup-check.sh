#!/bin/bash
#
# Does a byte handed to a UART chardev actually reach guest memory?
#
# It did not, for the whole life of this tree, and nothing noticed because
# nothing was ever connected to a UART's input. Two bugs stacked: the Rx DMA
# REQUEST line was never driven (hw/arm/ipod_touch_2g.c), and UCON[1:0]==11b --
# what the S5L8720 uses to select DMA -- was decoded as "not DMA"
# (hw/char/exynos4210_uart.c). The guest's Bluetooth driver arms a 2048-byte
# peripheral->memory channel on UART1's URXH and never reads the register
# itself, so replies sat in the Rx FIFO forever.
#
# The check: boot with the Bluetooth HCI model on UART1 and observe the final
# BCM Launch RAM request. Reaching it requires the guest to accept preceding
# HCI replies through the Rx DMA path.
#
#     tests/ipod/bluetooth-bringup-check.sh [nand-image] [boots]
#
# It boots the guest `boots` times (default 3, resetting in between) and
# requires bring-up EVERY time. One pass proves nothing here: the failures this
# guards are races, and they present as "Bluetooth worked yesterday and says
# unavailable today, and app launches stopped animating with it".
#
# Passing requires firmware download and the subsequent BCM sleep-mode setup.
# It does not prove BluetoothManager readiness or app-launch animation.
# Verbose DMA tracing changes timing and can hide the UART regression; use
# quiet boots with a guest BluetoothManager probe for stack acceptance.
set -u

QEMU="${QEMU:-$(cd "$(dirname "$0")/../.." && pwd)/build/qemu-system-arm}"
F="$HOME/Developer/qemu-ios-files"
NAND="${1:-$F/nand-testapps}"
BOOTS="${2:-3}"
WORK="$(mktemp -d)"
QMP="${QMP_PORT:-4599}"
trap 'rm -rf "$WORK"; [ -n "${PID:-}" ] && kill "$PID" 2>/dev/null' EXIT

IT_LCD_BRIGHT=255 IT_DIRECT_IBOOT="$F/ios3/iBoot.bin" \
IT_TVOUT_READY=1 \
IT_BOOT_ARGS="amfi_allow_any_signature=1 cs_enforcement_disable=1" \
IT_BOOT_ARGS_DELAY_MS=1500 IT_BOOT_ARGS_REPEAT=200 IT_BOOT_ARGS_INTERVAL_MS=250 \
IT_BT_TRACE=1 IT_DMAC_TRACE=1 \
"$QEMU" -M "iPod-Touch,bootrom=$F/bootrom_240_4,nand=$NAND,nor=$F/ios3/nor_7E18.bin,nandrw=$WORK/ovl" \
    -m 128M -display none -serial file:"$WORK/serial.log" \
    -qmp tcp:127.0.0.1:"$QMP",server=on,wait=off \
    >"$WORK/trace.log" 2>&1 &
PID=$!
fail=0
prev=0
prev_setup=0
for boot in $(seq 1 "$BOOTS"); do
    sleep 95
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "boot $boot: FAILED -- emulator exited" >&2
        fail=1
        break
    fi
    now=$(grep -c '0xfc4e' "$WORK/trace.log")
    if [ "$now" -gt "$prev" ]; then
        echo "boot $boot: BCM firmware Launch RAM command observed"
    else
        echo "boot $boot: FAILED -- no BCM Launch RAM this boot" >&2
        fail=1
    fi
    prev=$now
    setup=$(grep -c '0xfc27' "$WORK/trace.log")
    if [ "$setup" -le "$prev_setup" ]; then
        echo "boot $boot: FAILED -- controller setup did not reach sleep-mode configuration" >&2
        fail=1
    fi
    prev_setup=$setup
    if [ "$boot" -lt "$BOOTS" ]; then
        if ! python3 "$(dirname "$0")/../../contrib/ipod-touch-qmp.py" "$QMP" \
            cmd system_reset >/dev/null 2>&1; then
            echo "boot $boot: FAILED -- QMP reset failed" >&2
            fail=1
            break
        fi
    fi
done
kill "$PID" 2>/dev/null; PID=""

cmds=$(grep -c '\[BT\] cmd' "$WORK/trace.log")
echo "HCI commands the guest sent: $cmds"
if [ "$cmds" -lt 1 ]; then
    echo "FAIL: no HCI command reached the controller. Either UART1 Tx is" >&2
    echo "      broken, or the model is switched off (IT_BT=0)." >&2
    exit 1
fi

# 0xfc4e is BCM Launch RAM -- the last step of the firmware download, which the
# guest only reaches by running /etc/bluetool/iPod2,1.boot.script to the end:
# HCI_Reset, Update Baud Rate, Download Minidriver, the launch announcement,
# every Write RAM chunk, then Launch RAM. Getting there exercises the entire
# path both ways -- chardev -> Rx FIFO -> Rx DMA request -> DMAC -> guest memory
# -> UART receive timeout -> partial-buffer driver path -> BlueTool -- one grep
# for the firmware-download path. It does not establish stack readiness.
if [ "$fail" = 0 ] && grep -q '0xfc4e' "$WORK/trace.log"; then
    echo "PASS: BCM firmware and controller setup observed on all $BOOTS boots"
    grep -o '\[BT\] cmd .*' "$WORK/trace.log" | sort -u
    exit 0
fi

echo "FAIL: Bluetooth firmware/controller setup/reset checks did not pass every boot." >&2
if ! grep -q '0xfc18' "$WORK/trace.log"; then
    echo "      The guest never got past HCI_Reset, so it never accepted a" >&2
    echo "      reply: check the rxdmareq wiring, the UCON[1:0] DMA-mode" >&2
    echo "      decode, and the S5L UTRSTAT/UCON interrupt mapping." >&2
else
    echo "      It accepted replies but stalled mid-script. If it stops after" >&2
    echo "      0xfc2e, the two-byte launch announcement is missing." >&2
fi
grep -o '\[BT\] cmd .*' "$WORK/trace.log" | sort -u >&2
exit 1
