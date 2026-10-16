Apple iPod touch (``iPod-Touch-2G``)
====================================

The iPod touch machines emulate Apple's S5L-based iPod touch models well enough to restore and run their
iPhone OS firmware:

- ``iPod-Touch-2G``: the iPod touch (2nd generation), ``n72ap``, S5L8720. ``iPod-Touch`` is an alias, the
  machine's name before there was more than one.

The device models of the SoC blocks the S5L chips share are in ``hw/arm/s5l_*.c`` (``CONFIG_S5L``); the
external parts both boards use (the LIS302DL accelerometer) are named after the part. Everything else is the
board's own and lives with its machine.

The machine contract
--------------------

Skeuo (and anyone else driving these machines) relies on the following being the same on every iPod touch
machine. A machine that can't do one of these yet refuses it with an error, instead of ignoring it.

Options (``-M <machine>,<option>=<value>,...``):

``nor=<file>``
  The NOR image: the boot stages (LLB, iBoot, the device tree, the boot logos) and the SysCfg.

``nand-disk=<file>``, ``nand-overlay=<dir>``, ``nand-epoch=<n>``
  The NAND, presented as a freshly formatted NAND holding ``nand-disk`` (a raw disk image, as the restore
  ramdisk partitions it), with the pages the guest writes kept in ``nand-overlay`` on top of it.
  ``nand-epoch`` is the security epoch of the NAND driver the disk was formatted for.
  On the iPod touch 1G the disk is presented in Whimory 2.1's legacy format (the one iPhone OS 1.1.x reads,
  logical pages of 2048 bytes, so the MBR's LBAs count 2048 bytes), and ``nand-epoch`` is refused: that NAND
  driver has no security epoch. The 1G doesn't write the overlay yet (iBoot only reads).

``nand=<dir>``
  A NAND kept as one file per page, the older layout. Superseded by ``nand-disk``.

``usb-chardev=<id>``
  The chardev that carries the USB link to the host: the device side of USB, so that the host can restore
  the device and talk to it over usbmux.

``bootrom=<file>``
  Optional. A dump of the SoC's SecureROM to boot through. Without it the machine starts the first boot stage
  itself (``boot-image``, or LLB from the NOR), as the bootrom would have.

``boot-image=<file>``
  An image to start instead of LLB from the NOR, such as the iBSS of an IPSW, for a restore. Not with
  ``bootrom``.

``boot-args=<string>``
  The kernel boot arguments iBoot passes on a normal boot. Commas are doubled (``,,``) to escape them.

``dfu=on``
  Holds the force-DFU GPIO, so that the bootrom enters DFU mode. Only with ``bootrom``.

``netdev=<id>``
  The network backend of the Wi-Fi card, on a machine that has one. ``-nic user,model=<card>`` works too.

QOM paths (``qom-get`` and ``qom-set`` over QMP):

``/machine/accelerometer``, property ``orientation``
  ``flat`` (lying face up, as at power-on), ``portrait``, ``landscape-right``, ``portrait-upside-down`` or
  ``landscape-left``. Setting it holds the device in that orientation.

Input:

- The screen takes absolute pointer events, with the left button as a touch.
- Keys: ``h`` home, ``p`` power, ``+`` and ``-`` volume up and down, ``r`` rotates the device a quarter turn,
  ``m`` mutes the host's audio. The iPod touch 1G has only ``h`` and ``p`` so far (it has no volume buttons).

Running and saving:

- ``-action shutdown=pause``: closing the window pauses the machine instead of quitting.
- Every device model saves its state, so the machine can be migrated to a file (suspend) and loaded with
  ``-incoming`` (resume). The NAND is not part of the state; ``nand-overlay`` keeps it.
- ``query-machines`` names the device in the machine's description, e.g. "iPod touch (2nd generation)".

Example
-------

.. code-block:: bash

  $ qemu-system-arm -M iPod-Touch-2G,nor=nor.bin,nand-disk=disk.img,nand-overlay=overlay,nand-epoch=1 \
      -cpu max -m 2G -serial stdio
