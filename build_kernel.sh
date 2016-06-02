#!/bin/bash

export ARCH=arm64
export CROSS_COMPILE=../../prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin/aarch64-linux-android-

mkdir out

export SEC_BUILD_OPTION_HW_REVISION=02
make -j12 -C $(pwd) O=$(pwd)/out ARCH=arm64 CROSS_COMPILE=$(pwd)/../../prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin/aarch64-linux-android- KCFLAGS=-mno-android hero2qlte_dcm_defconfig
make -j12 -C $(pwd) O=$(pwd)/out ARCH=arm64 CROSS_COMPILE=$(pwd)/../../prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin/aarch64-linux-android- KCFLAGS=-mno-android

cp out/arch/arm64/boot/Image $(pwd)/arch/arm64/boot/Image
