#!/bin/bash
# TWRP kernel for Samsung Galaxy Note 3 build script by jcadduono
# This build script is for TeamWin Recovery Project only

################### BEFORE STARTING ################
#
# download a working toolchain and extract it somewhere and configure this
# file to point to the toolchain's root directory.
# I highly recommend using a Linaro GCC 4.9.x arm-linux-gnueabihf toolchain.
# Download it here:
# https://releases.linaro.org/components/toolchain/binaries/4.9-2016.02/arm-linux-gnueabihf/
#
# once you've set up the config section how you like it, you can simply run
# ./build.sh
#
##################### VARIANTS #####################
#
# unified = hltexx, hlteatt, hltecan, hltetmo, hltespr, hlteusc, hltevzw
#           N9005,  N900A,   N900W8,  N900T,   N900P,   N900R4,  N900V
#
# chn     = hltezn, hlteduoszn, hltezm, hlteduoszm, hltezc, hltezh, hlteke
#           N9006,  N9002,      N9008,  N9008V,     N9008S, N9007,  N9009
#
# kdi     = hltekdi, hltedcm
#           SCL22,   SC-01F
#
# skt     = hltektt, hlteskt, hltelgt
#           N900K,   N900S,   N900L
#
###################### CONFIG ######################

# root directory of NetHunter hlte git repo (default is this script's location)
RDIR=$(pwd)

[ "$VER" ] ||
# version number
VER=$(cat "$RDIR/VERSION")

# directory containing cross-compile arm toolchain
TOOLCHAIN=$HOME/build/toolchain/gcc-linaro-4.9-2016.02-x86_64_arm-linux-gnueabihf

# amount of cpu threads to use in kernel make process
THREADS=5

############## SCARY NO-TOUCHY STUFF ###############

export ARCH=arm
export CROSS_COMPILE=$TOOLCHAIN/bin/arm-linux-gnueabihf-

[ "$DEVICE" ] || DEVICE=hlte
[ "$TARGET" ] || TARGET=twrp
[ "$1" ] && VARIANT=$1
[ "$VARIANT" ] || VARIANT=unified
DEFCONFIG=${TARGET}_${DEVICE}_defconfig
VARIANT_DEFCONFIG=variant_${DEVICE}_${VARIANT}

ABORT()
{
	echo "Error: $*"
	exit 1
}

[ -f "$RDIR/arch/$ARCH/configs/${DEFCONFIG}" ] ||
abort "Config $DEFCONFIG not found in $ARCH configs!"

[ -f "$RDIR/arch/$ARCH/configs/$VARIANT_DEFCONFIG" ] ||
abort "Device variant/carrier $VARIANT not found in $ARCH configs!"

export LOCALVERSION="$TARGET-$DEVICE-$VARIANT-$VER"

KDIR=$RDIR/build/arch/arm/boot

CLEAN_BUILD()
{
	echo "Cleaning build..."
	cd "$RDIR"
	rm -rf build
}

SETUP_BUILD()
{
	echo "Creating kernel config for $LOCALVERSION..."
	cd "$RDIR"
	mkdir -p build
	make -C "$RDIR" O=build "$DEFCONFIG" \
		VARIANT_DEFCONFIG="$VARIANT_DEFCONFIG" \
		|| ABORT "Failed to set up build"
}

BUILD_KERNEL()
{
	echo "Starting build for $LOCALVERSION..."
	while ! make -C "$RDIR" O=build -j"$THREADS"; do
		read -p "Build failed. Retry? " do_retry
		case $do_retry in
			Y|y) continue ;;
			*) return 1 ;;
		esac
	done
}

BUILD_DTB()
{
	echo "Generating dtb.img..."
	"$RDIR/scripts/dtbTool/dtbTool" -o "$KDIR/dtb.img" "$KDIR/" -s 2048 || ABORT "Failed to generate dtb.img!"
}

CLEAN_BUILD && SETUP_BUILD && BUILD_KERNEL && BUILD_DTB && echo "Finished building $LOCALVERSION!"
