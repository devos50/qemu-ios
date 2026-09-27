# QEMU-iOS

Built with use from agentic coding products.

This is a fork of [devos50/qemu-ios](https://github.com/devos50/qemu-ios),
targeting an emulated **iPod touch 2G (n72ap) running iOS 3.1.3**. It boots to
an interactive home screen, runs real 2008-era App Store apps (including
OpenGL ES 1.1 games — see `docs/capabilities.md`), talks to `libimobiledevice`
over an emulated USB link, and persists guest writes across reboots. See
`docs/capabilities.md` for the full, currently-accurate list of what works and
where it stops.

## Build

```sh
../configure --enable-sdl --target-list=arm-softmmu --disable-capstone --disable-pie --disable-slirp --disable-fuse \
    --extra-cflags=-I/opt/homebrew/opt/openssl@3/include --extra-ldflags='-L/opt/homebrew/opt/openssl@3/lib -lcrypto'
ninja -C build qemu-system-arm
```

(Apple Silicon Homebrew paths shown; see `RUNNING.md` for other platforms and
build troubleshooting.)

## Run

```sh
contrib/run-ipod-touch.sh
```

This is the in-repo launcher; run `contrib/run-ipod-touch.sh --help` for the
full flag list (installing apps, USB/networking, audio, headless QMP driving,
etc). It expects the 3.1.3 release images under `~/Developer/qemu-ios-files`
(override with `IPOD_FILES`) — these are not included in this repository.

The native macOS frontend is the sibling `LightTouchMac` repository. Its
README and `scripts/build-package-native.sh` / `scripts/package.sh` describe
the current self-contained build. `contrib/macos-app/build-app.sh` is the older
launcher packaging pipeline.

---

QEMU-iOS is an emulator for legacy Apple devices.
Currently, the iPod Touch 1G and iPod Touch 2G are supported.

<img width="331" alt="it2g-qemu" src="https://github.com/devos50/qemu-ios/assets/1707075/9bf7f6c1-5918-47e9-bb3e-2e39ae15d519">

The schematic below shows the most important hardware components of the iPod Touch 2G and their interactions.
The schematic for the iPod Touch 1G is mostly similar.

<img width="80%" alt="it2g-schematic" src="https://github.com/devos50/qemu-ios/assets/1707075/4b8eca9a-74b0-4590-ad23-bc056acde434">

### Running the iPod Touch 1G

Instructions on how to run the iPod Touch 1G emulator can be found [here](https://devos50.github.io/blog/2022/ipod-touch-qemu-pt2/).
A technical blog post with more information about the peripherals and reverse engineering process is published [here](https://devos50.github.io/blog/2022/ipod-touch-qemu/).

### Running the iPod Touch 2G

Instructions on how to run the iPod Touch 2G emulator can be found [here](https://github.com/devos50/qemu-ios/blob/ipod_touch_2g/RUNNING.md).
