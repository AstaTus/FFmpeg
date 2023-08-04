#!/bin/sh
QPLAYER_FFMPEG_FOLDER="${SRCROOT}/../../libs/ios/qplayer-ffmpeg"

LIB_FOLDER="${QPLAYER_FFMPEG_FOLDER}/lib"

INCLUDE_FOLDER="${QPLAYER_FFMPEG_FOLDER}/include"
HEADERS_FOLDER="${QPLAYER_FFMPEG_FOLDER}/include/libavcodec"

#UNIVERSAL_PRODUCT_PATH="${UNIVERSAL_OUTPUT_FOLDER}/framework名.framework"

#创建输出目录，并删除之前的framework文件
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

cp -f ${BUILD_DIR}/Debug-iphoneos/libavcodec.a ${LIB_FOLDER}/

cp -f ${BUILD_DIR}/Debug-iphoneos/include/avcodec/* ${HEADERS_FOLDER}/
