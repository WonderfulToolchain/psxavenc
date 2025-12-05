#!/bin/bash

ROOT_DIR="$(pwd)"
FFMPEG_VERSION="8.0.1"
NUM_JOBS="4"

if [ $# -eq 1 ]; then
	PACKAGE_NAME="$1"
	FFMPEG_OPTIONS=""
	PSXAVENC_OPTIONS=""
elif [ $# -eq 3 ]; then
	PACKAGE_NAME="$1"
	FFMPEG_OPTIONS="--arch=x86 --target-os=mingw32 --cross-prefix=$2-"
	PSXAVENC_OPTIONS="--cross-file $3"
else
	echo "Usage: $0 <package name> [cross prefix] [cross file]"
	exit 0
fi

## Download FFmpeg

if [ ! -d ffmpeg-$FFMPEG_VERSION ]; then
	wget "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz" \
		|| exit 1
	tar Jxf ffmpeg-$FFMPEG_VERSION.tar.xz \
		|| exit 1

	rm -f ffmpeg-$FFMPEG_VERSION.tar.xz
fi

## Build FFmpeg

mkdir -p ffmpeg-build
cd ffmpeg-build

../ffmpeg-$FFMPEG_VERSION/configure \
	--prefix="$ROOT_DIR/ffmpeg-dist" \
	$FFMPEG_OPTIONS \
	--enable-gpl \
	--enable-version3 \
	--enable-static \
	--disable-shared \
	--enable-small \
	--disable-programs \
	--disable-doc \
	--disable-avdevice \
	--disable-avfilter \
	--disable-network \
	--disable-encoders \
	--disable-hwaccels \
	--disable-muxers \
	--disable-bsfs \
	--disable-devices \
	--disable-filters \
	--disable-mmx \
	|| exit 2
make -j $NUM_JOBS \
	|| exit 2
make install \
	|| exit 2

cd ..
rm -rf ffmpeg-build

## Build psxavenc

meson setup \
	--buildtype release \
	-Db_lto=true \
	--strip \
	--prefix $ROOT_DIR/psxavenc-dist \
	--pkg-config-path $ROOT_DIR/ffmpeg-dist/lib/pkgconfig \
	$PSXAVENC_OPTIONS \
	psxavenc-build \
	psxavenc \
	|| exit 3
meson compile -C psxavenc-build \
	|| exit 3
meson install -C psxavenc-build \
	|| exit 3

rm -rf ffmpeg-dist psxavenc-build

## Package psxavenc

cd psxavenc-dist

zip -9 -r ../$PACKAGE_NAME.zip . \
	|| exit 4

cd ..
rm -rf psxavenc-dist
