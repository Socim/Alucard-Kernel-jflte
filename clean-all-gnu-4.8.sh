#!/bin/bash

cp -pv .config .config.bkp;
make ARCH=arm CROSS_COMPILE=/Volumes/android/cm-11.0/prebuilt/linux-x86/toolchain/arm-cortex_a15-linux-gnueabihf-linaro_4.8.3-2014.02/bin/arm-gnueabi- mrproper;
cp -pv .config.bkp .config;
make clean && make mrproper;
rm -rf ./kernel/usr;
rm -rf READY-JB/system/lib;
rm -rf READY-JB/*.zip;
rm -rf READY-JB/boot.img;
sh clean-junk.sh;
# clean ccache
read -t 5 -p "clean ccache, 5sec timeout (y/n)?";
if [ "$REPLY" == "y" ]; then
	ccache -C;
fi;
