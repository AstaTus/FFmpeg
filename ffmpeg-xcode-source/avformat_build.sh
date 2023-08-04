#!/bin/sh
QPLAYER_FFMPEG_FOLDER="${SRCROOT}/../../libs/ios/qplayer-ffmpeg"

LIB_FOLDER="${QPLAYER_FFMPEG_FOLDER}/lib"

INCLUDE_FOLDER="${QPLAYER_FFMPEG_FOLDER}/include"
HEADERS_FOLDER="${QPLAYER_FFMPEG_FOLDER}/include/libavformat"


if [ ! -d "${QPLAYER_FFMPEG_FOLDER}" ];then
    mkdir -p "${QPLAYER_FFMPEG_FOLDER}"
fi
if [ ! -d "${LIB_FOLDER}" ];then
    mkdir -p "${LIB_FOLDER}"
fi

if [ ! -d "${INCLUDE_FOLDER}" ];then
    mkdir -p "${INCLUDE_FOLDER}"
fi
if [ ! -d "${HEADERS_FOLDER}" ];then
    mkdir -p "${HEADERS_FOLDER}"
fi

cp -f ${BUILD_DIR}/Debug-iphoneos/libavformat.a ${LIB_FOLDER}/

cp -f ${BUILD_DIR}/Debug-iphoneos/include/avformat/* ${HEADERS_FOLDER}/
