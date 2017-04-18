# QEmu for LiteX

This repository contains a version of QEmu for emulating
[LiteX based SoCs](https://github.com/enjoy-digital/litex). This is mainly
targeted at being used with the
[TimVideos HDMI2USB firmware](https://hdmi2usb.tv) which can be
[found here](https://github.com/timvideos/HDMI2USB-litex-firmware) but could be
reused with other LiteX based SoCs with some work.


## Building

There are two methods for building qemu-litex.

As qemu-litex targets a specific LiteX SoC it currently needs the configuration
information from that SoC.

### Building inside HDMI2USB-litex-firmware

 1) Follow ["getting started" instructions][1] for getting the gateware setup.

   - You can skip the ["Prerequisite (Xilinx)"][2] section if you never want to
     build gateware for the FPGAs.

 2) Enter the HDMI2USB-litex-firmware environment with `source scripts/enter-env.sh`

 3) (Optional) Set `PLATFORM` and `TARGET` as needed.

   - For example on the MimasV2 you need to set `export PLATFORM=mimasv2`.

 4) Run `./scripts/build-qemu.sh`. This will;

   - Get the qemu-litex repo.
   - Configure a qemu system for your platform/target in `./build/$PLATFORM_$TARGET_$CPU/qemu`.
   - Build qemu.
   - Run the bios+firmware using qemu.

 [1]: https://github.com/timvideos/HDMI2USB-litex-firmware/blob/master/getting-started.md
 [2]: https://github.com/timvideos/HDMI2USB-litex-firmware/blob/master/getting-started.md#prerequisite-xilinx


### Building outside HDMI2USB-litex-firmware

 1) Clone the git repo with `git clone https://github.com/timvideos/qemu-litex.git`

 2) Enter the `qemu-litex` directory.

 3) (Optional) Set the `PLATFORM`, `TARGET` and `CPUS` to your required
    configuration. The defaults should be fine if you are just playing with
    things.

 4) Run the `./build-litex-qemu.sh` which will
   - Download the configuration information, BIOS and firmware from the
     [HDMI2USB-firmware-prebuilt repo][3] into the `build` directory.
   - Configure qemu to build in the `build` directory.
   - Build qemu.

 5) You can then run qemu by following the examples output at the end of the
    script.

 [3]: https://github.com/timvideos/HDMI2USB-firmware-prebuilt


## Status

 * [Architectures](https://github.com/timvideos/qemu-litex/issues/14)
   - [X] lm32
   - [X] or1k (openrisc)
   - [ ] riscv
   - [ ] sh2 (j-core)

 * Memory
   - [X] block rom
   - [X] block sram
   - [X] main memory (DDR?)
   - [X] spiflash (including bitbanging)

 * Peripherals
   - [ ] [timer (current broken)](https://github.com/timvideos/qemu-litex/issues/14)
   - [X] uart
   - [ ] [GPIO (LEDs, Switches, etc)](https://github.com/timvideos/qemu-litex/issues/5)
   - [ ] [I2C BitBanging](https://github.com/timvideos/qemu-litex/issues/5)
   - [ ] [LiteEth](https://github.com/enjoy-digital/liteeth)
     - [X] Ethernet transmission
     - [X] MDIO bitbang emulation
     - [ ] [MDIO is connected to virtual device info](https://github.com/timvideos/qemu-litex/issues/16)
   - [ ] [LiteVideo](https://github.com/enjoy-digital/litevideo)
     - [ ] [HDMI Video Output](https://github.com/timvideos/qemu-litex/issues/5)
     - [ ] [HDMI Video Input](https://github.com/timvideos/qemu-litex/issues/14)

