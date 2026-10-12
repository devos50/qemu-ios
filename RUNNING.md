## Running the iPod Touch 2G Emulator

This file contains the instructions on how to run the iPod Touch 2G emulator using QEMU.
Note that this is an experimental release and the functionality of the device is still limited.
Linux compatibility is currently unstable. Efforts are underway to improve it.

### Getting the Source Code

Clone the repository using the following commands:

```
git clone -b skeuo https://github.com/devos50/qemu-ios.git
cd qemu-ios
```

Now you can proceed with the rest of the instructions for building and running the emulator.

### Building QEMU

Make sure you have the required libraries installed to compile QEMU:

```
# On MacOS
brew install glib libslirp ninja pixman pkg-config sdl2 openssl@3 faad2 mpg123

# On Linux (Ubuntu)
sudo apt install make ninja-build pkg-config libssl-dev libsdl2-dev libpixman-1-dev libglib2.0-dev libslirp-dev libfaad-dev libmpg123-dev

# On Linux (Arch)
sudo pacman -S --needed make ninja pkgconf openssl sdl2 pixman glib2 libslirp faad2 mpg123

# On Windows (MSYS2/mingw64)
pacman -S base-devel mingw-w64-x86_64-toolchain git python ninja mingw-w64-x86_64-glib2 mingw-w64-x86_64-pixman python-setuptools mingw-w64-x86_64-SDL2 mingw-w64-x86_64-openssl mingw-w64-x86_64-libslirp
```

Compile QEMU by running the following commands from the root directory:

```
mkdir build
cd build
../configure --target-list=arm-softmmu --enable-sdl
make
```

The iPod Touch machine needs OpenSSL's libcrypto, which the AES, SHA1 and PKE engines use; `configure` finds it with pkg-config and leaves the machine out when it is missing.
Homebrew does not put OpenSSL on the pkg-config path, so on macOS run `configure` with `PKG_CONFIG_PATH=$(brew --prefix openssl@3)/lib/pkgconfig`.
faad2 and mpg123 are optional: without them the AMC audio decoder plays no AAC or MP3.
SDL handles the emulator window and its keyboard and mouse input.
You can speed up the `make` command by passing the number of available CPU cores with the `-j` flag, e.g., use `make -j6` to compile using six CPU cores.
The compilation produces the `qemu-system-arm` binary in the `build` directory.

### Downloading the Required Files

We need a few files to successfully boot the iPod Touch emulator to the home screen, which I published [here](https://github.com/devos50/qemu-ios/releases/tag/n72ap_v1) for convenience. You can download all these files from here, and they include the following:
- The S5L8720 bootrom binary, which is started as the very first thing when booting.
- A NOR image that contains various auxillary files used by the bootloader. I provide some instructions on how to generate this image yourself [here](https://github.com/devos50/qemu-ios-generate-nor).
- A NAND image that contains the root file system. I provide some instructions on how to generate this image yourself [here](https://github.com/devos50/qemu-ios-generate-nand).

Download all the required files and save them to a convenient location. You should unzip the `nand_n72ap.zip` file, which contains a single directory named `nand`.

### Running the Emulator

We are now ready to run the emulator from the build directory with the following command:

```
./qemu-system-arm -M iPod-Touch,bootrom=<path to bootrom>,nand=<path to NAND directory>,nor=<path to NOR directory> -serial mon:stdio -cpu max -m 2G -d unimp
```

### Sensors

Press R in the emulator window to turn the device a quarter turn (portrait, landscape, upside down, landscape); apps that support landscape, such as Safari, rotate with it.
The accelerometer starts lying flat, which keeps the current orientation, as on a real device.
From the QEMU monitor you can also set the acceleration in mg, e.g. `qom-set /machine/accelerometer x 1000`, and the ambient light in lux, e.g. `qom-set /machine/lightsensor lux 10000`.
With auto-brightness on, iOS 2 brightens the screen right away when the light increases, but only dims it when the device wakes up.

### Wi-Fi

The emulator models the BCM4325 Wi-Fi chip. It shows one open network, `QEMU`, which you can join from Settings > Wi-Fi.
The network traffic goes to a QEMU network backend. With QEMU's user-mode networking the device gets an address by DHCP (10.0.2.15) and can reach the internet through the host.
User-mode networking needs libslirp: QEMU builds it in when the library is installed and `--disable-slirp` is not passed to `configure` (the commands above install it).

Without further options, QEMU connects the Wi-Fi chip to user-mode networking. To choose the backend yourself, add either

```
-nic user,model=bcm4325
```

or a netdev and the `netdev` machine option:

```
-netdev user,id=net0 -M iPod-Touch,bootrom=...,nand=...,nor=...,netdev=net0
```

Use `-nic none` to run without a network, and `-nic user,model=bcm4325,mac=<address>` to set the Wi-Fi MAC address.
To trace the SDIO and Wi-Fi traffic, add `-trace 'ipod_touch_bcm4325_*' -trace ipod_touch_sdio_cmd`.

### Graphics

The emulator models the PowerVR MBX Lite graphics core that SpringBoard uses for compositing. It carries out the 2D core's fills and copies, and the 3D blits that blend layers, directly in the emulated memory, so a stock SpringBoard draws through the MBX and no longer needs `LK_ENABLE_MBX2D=0`.
OpenGL ES is not supported yet: the tile accelerator, which apps' 3D scenes go through, is not modelled, so their scenes stay empty.
To trace the graphics commands, add `-trace 'ipod_touch_mbx_2d_*' -trace 'ipod_touch_mbx_3d_*'`.

If there are any issues running the above commands, please let me know by [opening an issue](https://github.com/devos50/qemu-ios/issues/new).
