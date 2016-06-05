#!/bin/bash

LANG=C

# location
KERNELDIR=$(readlink -f .);

PYTHON_CHECK=$(ls -la /usr/bin/python | grep python3 | wc -l);
PYTHON_WAS_3=0;

if [ "$PYTHON_CHECK" -eq "1" ] && [ -e /usr/bin/python2 ]; then
	if [ -e /usr/bin/python2 ]; then
		rm /usr/bin/python
		ln -s /usr/bin/python2 /usr/bin/python
		echo "Switched to Python2 for cleaning kernel will switch back when done";
		PYTHON_WAS_3=1;
	else
		echo "You need Python2 to clean this kernel. install and come back."
		exit 1;
	fi;
else
	echo "Python2 is used! all good, cleaning!";
fi;

rm .config .config.bkp .config.old;
make ARCH=arm64 mrproper;
make clean;

rm -rf "$KERNELDIR"/READY-KERNEL/boot
rm -f "$KERNELDIR"/READY-KERNEL/system/lib/modules/*;
rm -f "$KERNELDIR"/READY-KERNEL/*.zip
rm -f "$KERNELDIR"/READY-KERNEL/*.img
rm -f "$KERNELDIR"/READY-KERNEL/view_only_config
rm -f "$KERNELDIR"/READY-KERNEL/.config

if [ -d ../Ramdisk-SM-G935D-MM-tmp ]; then
	rm -rf ../Ramdisk-SM-G935D-MM-tmp/*
else
	mkdir ../Ramdisk-SM-G935D-MM-tmp
	chmod 777 ../Ramdisk-SM-G935D-MM-tmp
fi;

# force regeneration of .dtb and Image files for every compile
rm -f arch/arm64/boot/*.dtb
rm -f arch/arm64/boot/dts/*.dtb
rm -f arch/arm64/boot/dts/samsung/*.dtb
rm -f arch/arm64/boot/dts/samsung/*.dtb.reverse.dts
rm -f arch/arm64/boot/*.cmd
rm -f arch/arm64/boot/zImage
rm -f arch/arm64/boot/Image
rm -f arch/arm64/boot/Image.gz
rm -f arch/arm64/boot/Image.gz-dtb
rm -f firmware/abov/*.fw
rm -f firmware/tsp_sec/*.fw
rm -f firmware/tsp_stm/*.fw

if [ "$PYTHON_WAS_3" -eq "1" ]; then
	rm /usr/bin/python
	ln -s /usr/bin/python3 /usr/bin/python
fi;

git checkout firmware/
