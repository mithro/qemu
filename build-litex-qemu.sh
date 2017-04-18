#!/bin/bash

PLATFORM="${PLATFORM:-mimasv2}"
TARGET="${TARGET:-base}"
CPUS="${CPU:-lm32 or1k}"
BRANCH="${BRANCH:-master}"

GITHUB_URL=https://github.com/timvideos/HDMI2USB-firmware-prebuilt/

set -e

function download() {
	NAME=$1
	RPATH=$2
	LPATH=$3
	if [ ! -e "$LPATH" ]; then
		# Automatically figure out the latest revision
		if [ -e gateware.rev ]; then
			REV="$(cat gateware.rev)"
		fi
		if [ x"$REV" = x"" ]; then
			REV="$(svn ls $GITHUB_URL/trunk/archive/$BRANCH | sort | tail -n1 | sed -e's-/$--')"
			if [ x"$REV" = x"" ]; then
				echo "Unable to find upstream revision!"
			fi
			echo $REV > gateware.rev
			echo "Using revision $REV"
		fi
		echo
		echo "Download $NAME from $GITHUB_URL/trunk/archive/$BRANCH/$REV/$PLATFORM/$TARGET/$RPATH"
		echo "---------------------------------------"
		# archive/master/v0.0.3-696-g2f815c1/minispartan6/base/lm32
		svn export --force $GITHUB_URL/trunk/archive/$BRANCH/$REV/$PLATFORM/$TARGET/$RPATH $LPATH | grep -v "^Export"
	fi
}

echo
echo "Using headers from branch '$BRANCH' for '$PLATFORM/$TARGET'"
echo

mkdir -p build
(
	cd build

	# Get the flash file
	for CPU in $CPUS; do
		download "generated headers" $CPU/software/include/generated generated
		download "bios ($CPU)" $CPU/software/bios/bios.bin bios-$CPU.bin
		download "bios debug symbols ($CPU)" $CPU/software/bios/bios.elf bios-$CPU.elf
		download "firmware ($CPU)" $CPU/software/firmware/firmware.bin firmware-$CPU.bin
		download "firmware debug symbols ($CPU)" $CPU/software/firmware/firmware.elf firmware-$CPU.elf
		download "flash image ($CPU)" $CPU/flash.bin flash-$CPU.bin
	done

	if [ ! -f config.log -o ! -f Makefile -o ! -f qemu-img ]; then
		../configure \
				--target-list=lm32-softmmu,or32-softmmu \
				--python=/usr/bin/python2 \
				--enable-fdt \
				--disable-kvm \
				--disable-xen \
				--enable-debug \
				--enable-debug-info
	fi
	make -j32

	for CPU in $CPUS; do
		if [ ! -e flash-$CPU.qcow2 ]; then
			# The flash image stops short, but qemu requires a full
			# size image.
			echo "Generating QEmu COW image (build/flash-$CPU.qcow2) for SPI flash"
			./qemu-img convert \
				-f raw flash-$CPU.bin \
				-O qcow2 flash-$CPU.qcow2
			# FIXME: Hard coded 16M should be looked up somewhere?
			./qemu-img resize flash-$CPU.qcow2 16M
		fi
	done

	declare -a EXTRA_ARGS
	# BIOS
	if grep -q 'ROM_BASE 0x00000000' generated/mem.h; then
		echo "Platform has BIOS ROM, adding BIOS"
		EXTRA_ARGS+=('-bios build/bios-$CPU.bin')
	fi
	# SPI Flash
	if grep -q 'SPIFLASH_BASE' generated/mem.h; then
		echo "Platform has SPI flash (WARNING: assuming n25q16!)"
		EXTRA_ARGS+=('-drive if=mtd,format=qcow2,file=build/flash-$CPU.qcow2,serial=n25q16')
	fi
	# Ethernet
	if grep -q 'ETHMAC_BASE' generated/csr.h; then
		echo "Platform has Ethernet, **needs to be setup**!"
		EXTRA_ARGS+=('-net nic -net tap,ifname=tap0,script=no,downscript=no')
	fi

	echo
	echo "To use run something like the following;"

	for CPU in $CPUS; do
		if [ "$CPU" = "or1k" ]; then
			QEMU_CPU=or32
		else
			QEMU_CPU="$CPU"
		fi
		cat <<EOF
 build/$QEMU_CPU-softmmu/qemu-system-$QEMU_CPU \\
        -nographic -nodefaults \\
        -monitor pty \\
        -serial stdio \\
        -M litex \\
EOF
		for EXTRA in "${EXTRA_ARGS[@]}"; do
			EXTRA=$(eval echo "$EXTRA")
			cat<<EOF
	$EXTRA \\
EOF
		done
	echo
	done
	cat <<'EOF'

 Note: QEmu uses or32 while LiteX uses or1k for the OpenRISC architecture

EOF

)
