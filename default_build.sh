#!/bin/sh
clear

LANG=C

# What you need installed to compile
# gcc, gpp, cpp, c++, g++, lzma, lzop, ia32-libs flex

# What you need to make configuration easier by using xconfig
# qt4-dev, qmake-qt4, pkg-config

# Setting the toolchain
# the kernel/Makefile CROSS_COMPILE variable to match the download location of the
# bin/ folder of your toolchain
# toolchain already axist and set! in kernel git. ../aarch64-linux-android-4.9/bin/

# Structure for building and using this script

#--project/				(progect container folder)
#------Ramdisk-SM-G935D-MM/		(ramdisk files for boot.img)
#------Ramdisk-SM-G935D-MM-tmp/		(ramdisk tmp store without .git)
#--------lib/modules/			(modules dir, will be added to system on boot)
#------Kernel-SM-G935D-MM/		(kernel source goes here)
#--------READY-RELEASES/		(When using all selector, all models ready kernels will go to this folder)
#--------READY-KERNEL/			(output directory, where the final boot.img is placed)
#----------meta-inf/			(meta-inf folder for your flashable zip)
#----------system/

# location
KERNELDIR=$(readlink -f .);

CLEANUP()
{
	# begin by ensuring the required directory structure is complete, and empty
	echo "Initialising................."
	rm -rf "$KERNELDIR"/READY-KERNEL/boot
	rm -f "$KERNELDIR"/READY-KERNEL/system/lib/modules/*;
	rm -f "$KERNELDIR"/READY-KERNEL/*.zip
	rm -f "$KERNELDIR"/READY-KERNEL/*.img
	mkdir -p "$KERNELDIR"/READY-KERNEL/boot

	if [ -d ../Ramdisk-SM-G935D-MM-tmp ]; then
		rm -rf ../Ramdisk-SM-G935D-MM-tmp/*
	else
		mkdir ../Ramdisk-SM-G935D-MM-tmp
		chown root:root ../Ramdisk-SM-G935D-MM-tmp
		chmod 777 ../Ramdisk-SM-G935D-MM-tmp
	fi;

	# force regeneration of .dtb and Image files for every compile
	rm -f arch/arm64/boot/*.dtb
	rm -f arch/arm64/boot/dts/*.dtb
	rm -f arch/arm64/boot/*.cmd
	rm -f arch/arm64/boot/zImage
	rm -f arch/arm64/boot/Image
	rm -f arch/arm64/boot/Image.gz
	rm -f arch/arm64/boot/Image.gz-dtb

	BUILD_H850=0
}
CLEANUP;

BUILD_NOW()
{
	PYTHON_CHECK=$(ls -la /usr/bin/python | grep python3 | wc -l);
	PYTHON_WAS_3=0;

	if [ "$PYTHON_CHECK" -eq "1" ] && [ -e /usr/bin/python2 ]; then
		if [ -e /usr/bin/python2 ]; then
			rm /usr/bin/python
			ln -s /usr/bin/python2 /usr/bin/python
			echo "Switched to Python2 for building kernel will switch back when done";
			PYTHON_WAS_3=1;
		else
			echo "You need Python2 to build this kernel. install and come back."
			exit 1;
		fi;
	else
		echo "Python2 is used! all good, building!";
	fi;

	# move into the kernel directory and compile the main image
	echo "Compiling Kernel.............";
	if [ ! -f "$KERNELDIR"/.config ]; then
		if [ "$BUILD_G935D" -eq "1" ]; then
			cp arch/arm64/configs/hero2qlte_dcm_defconfig .config
		fi;
	fi;

	# get version from config
	GETVER=$(cat "$KERNELDIR/VERSION")

	cp "$KERNELDIR"/.config "$KERNELDIR"/arch/arm64/configs/"$KERNEL_CONFIG_FILE";

	# remove all old modules before compile
	for i in $(find "$KERNELDIR"/ -name "*.ko"); do
		rm -f "$i";
	done;

	# Idea by savoca
	NR_CPUS=$(grep -c ^processor /proc/cpuinfo)

	if [ "$NR_CPUS" -le "2" ]; then
		NR_CPUS=4;
		echo "Building kernel with 4 CPU threads";
	else
		echo "Building kernel with $NR_CPUS CPU threads";
	fi;

	# build Image
	export SEC_BUILD_OPTION_HW_REVISION=02
	time make ARCH=arm64 CROSS_COMPILE=../../../prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin/aarch64-linux-android-  KCFLAGS=-mno-android -j ${NR_CPUS}

	cp "$KERNELDIR"/.config "$KERNELDIR"/arch/arm64/configs/"$KERNEL_CONFIG_FILE";

	stat "$KERNELDIR"/arch/arm64/boot/Image || exit 1;

	# compile the modules, and depmod to create the final Image
	echo "Compiling Modules............"
	time make ARCH=arm64 CROSS_COMPILE=../../../prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin/aarch64-linux-android- modules -j ${NR_CPUS} KCFLAGS=-mno-android || exit 1

	# move the compiled Image and modules into the READY-KERNEL working directory
	echo "Move compiled objects........"

	# copy needed branch files to Ramdisk temp dir.
	cp -a ../Ramdisk-SM-G935D-MM/* ../Ramdisk-SM-G935D-MM-tmp/

	if [ ! -d "$KERNELDIR"/READY-KERNEL/system/lib/modules ]; then
		mkdir -p "$KERNELDIR"/READY-KERNEL/system/lib/modules;
	fi;

	for i in $(find "$KERNELDIR" -name '*.ko'); do
		cp -av "$i" "$KERNELDIR"/READY-KERNEL/system/lib/modules/;
	done;

	chmod 755 "$KERNELDIR"/READY-KERNEL/system/lib/modules/*

	# remove empty directory placeholders from tmp-initramfs
	for i in $(find ../Ramdisk-SM-G935D-MM-tmp/ -name .gitignore); do
		rm -f "$i";
	done;

	if [ -e "$KERNELDIR"/arch/arm64/boot/Image ]; then

		if [ ! -d READY-KERNEL/boot ]; then
			mkdir READY-KERNEL/boot
		fi;

		cp arch/arm64/boot/Image READY-KERNEL/boot/
		cp arch/arm64/boot/Image.gz READY-KERNEL/boot/kernel
		cp .config READY-KERNEL/view_only_config

		# strip not needed debugs from modules.
		../aarch64-linux-android-4.9/bin/aarch64-linux-android-strip --strip-unneeded "$KERNELDIR"/READY-KERNEL/system/lib/modules/* 2>/dev/null
		../aarch64-linux-android-4.9/bin/aarch64-linux-android-strip --strip-debug "$KERNELDIR"/READY-KERNEL/system/lib/modules/* 2>/dev/null

		# create the Ramdisk and move it to the output working directory
		echo "Create Ramdisk..............."
		scripts/mkbootfs ../Ramdisk-SM-G935D-MM-tmp | gzip > ramdisk.gz 2>/dev/null
		mv ramdisk.gz READY-KERNEL/boot/

		# create the dt.img from the compiled device files, necessary for msm8996 boot images
		echo "Create dtb.img................"
		./scripts/dtbTool/dtbTool -s 4096 -o READY-KERNEL/boot/dtb.img -p scripts/dtc/ arch/arm64/boot/

		if [ "$PYTHON_WAS_3" -eq "1" ]; then
			rm /usr/bin/python
			ln -s /usr/bin/python3 /usr/bin/python
		fi;

		# add kernel config to kernle zip for other devs
		cp "$KERNELDIR"/.config READY-KERNEL/

		# build the final boot.img ready for inclusion in flashable zip
		echo "Build boot.img..............."
		cp scripts/mkbootimg READY-KERNEL/boot/
		cd READY-KERNEL/boot/
		base=0x80000000
		kernel_offset=0x00008000
		ramdisk_offset=0x02200000
		tags_addr=0x80000100
		pagesize=4096
		board="SRPPA14C000KU"
		cmd_line="console=null androidboot.hardware=qcom user_debug=31 msm_rtb.filter=0x37 ehci-hcd.park=3 lpm_levels.sleep_disabled=1 cma=24M@0-0xffffffff rcupdate.rcu_expedited=1"
		./mkbootimg --kernel kernel --ramdisk ramdisk.gz --dt dtb.img --base $base --cmdline "$cmd_line" --pagesize $pagesize -o newboot.img
		mv newboot.img ../boot.img

		# cleanup all temporary working files
		echo "Post build cleanup..........."
		cd ..
		rm -rf boot

		cd "$KERNELDIR"/READY-KERNEL/

		# create the flashable zip file from the contents of the output directory
		echo "Make flashable zip..........."
		zip -r Homura-Kernel-"${GETVER}"-MM-"$(date +"[%H-%M]-[%d-%m]")".zip * >/dev/null
		stat boot.img
		rm -f ./*.img
		cd ..
	else
		if [ "$PYTHON_WAS_3" -eq "1" ]; then
			rm /usr/bin/python
			ln -s /usr/bin/python3 /usr/bin/python
		fi;

		# with red-color
		echo -e "\e[1;31mKernel STUCK in BUILD! no Image exist\e[m"
	fi;
}

CLEAN_KERNEL()
{
	PYTHON_CHECK=$(ls -la /usr/bin/python | grep python3 | wc -l);
	CLEAN_PYTHON_WAS_3=0;

	if [ "$PYTHON_CHECK" -eq "1" ] && [ -e /usr/bin/python2 ]; then
		if [ -e /usr/bin/python2 ]; then
			rm /usr/bin/python
			ln -s /usr/bin/python2 /usr/bin/python
			echo "Switched to Python2 for building kernel will switch back when done";
			CLEAN_PYTHON_WAS_3=1;
		else
			echo "You need Python2 to build this kernel. install and come back."
			exit 1;
		fi;
	else
		echo "Python2 is used! all good, building!";
	fi;

	if [ -e .config ]; then
		cp -pv .config .config.bkp;
	elif [ -e .config.bkp ]; then
		rm .config.bkp
	fi;
	make ARCH=arm64 mrproper;
	make clean;
	if [ -e .config.bkp ]; then
		cp -pv .config.bkp .config;
	fi;

	if [ "$CLEAN_PYTHON_WAS_3" -eq "1" ]; then
		rm /usr/bin/python
		ln -s /usr/bin/python3 /usr/bin/python
	fi;

	# restore firmware libs*.a
	git checkout firmware/
}

export KERNEL_CONFIG=hero2qlte_dcm_defconfig
KERNEL_CONFIG_FILE=hero2qlte_dcm_defconfig
BUILD_G935D=1;
BUILD_NOW;

