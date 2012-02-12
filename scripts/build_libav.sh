#!/bin/bash
set -e

if [[ "$#" == 0 ]]
then
    echo "
Usage: $0 <debug|release> [local]
'local' means using non-system-wide installation of x264 and libfaac,
expects them in `pwd`/../x264, `pwd`/../faac-1.28
"
    exit 1
fi

if [[ "$1" == "debug" ]]
then
    echo 'debug build'
    OPTIM=' --enable-debug --disable-optimizations --disable-yasm --disable-asm '
else
    echo 'optimized build'
    OPTIM=' --disable-debug --enable-optimizations --enable-yasm --enable-asm '
fi

if [[ "$2" == "local" ]]
then
    echo 'adding extra params for local build'
    EXTRA_LIBS="`pwd`/../x264/libx264.a `pwd`/../faac-1.28/libfaac/.libs/libfaac.a"
    EXTRA_CFLAGS="-I`pwd`/../x264 -I`pwd`/../faac-1.28/include"
    EXTRA_LDFLAGS="-L`pwd`/../x264 -L`pwd`/../faac-1.28/libfaac/.libs"
fi

git clean -dxf

./configure \
--disable-protocols \
--enable-protocol=file \
--enable-protocol=pipe \
--enable-protocol=tcp \
--enable-protocol=http \
--disable-avdevice \
--disable-indevs \
--disable-outdevs \
--disable-bsfs \
--enable-bsf=h264_mp4toannexb \
--disable-filters \
--enable-filter=nullsink \
--enable-filter=fifo \
--enable-filter=drawtext \
--disable-muxers \
--enable-muxer=mpegts \
--enable-muxer=flv \
--enable-muxer=mp4 \
--disable-demuxers \
--enable-demuxer=mpegts \
--enable-demuxer=mov \
--enable-demuxer=flv \
--disable-encoders \
--enable-encoder=libx264 \
--enable-encoder=libfaac \
--disable-decoders \
--enable-decoder=h264 \
--enable-decoder=aac \
--disable-parsers \
--enable-parser=h264 \
--enable-parser=aac \
--enable-parser=aac_latm \
--enable-static \
--enable-shared \
--disable-libgsm \
--enable-libx264 \
--enable-libfaac \
--enable-libfreetype \
--enable-nonfree \
--disable-zlib \
--enable-gpl \
--extra-libs="$EXTRA_LIBS" \
--extra-cflags="$EXTRA_CFLAGS" \
--extra-ldflags="$EXTRA_LDFLAGS" \
$OPTIM

make -j`grep processor /proc/cpuinfo  | wc -l`

cp */*.so* .

