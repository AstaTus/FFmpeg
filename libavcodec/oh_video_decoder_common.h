/*
 * Harmony OH Video Decoder
 *
 * Copyright (c) 2015-2024 AstaTus <523182099 qq.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVCODEC_OH_VIDEO_DECODER_COMMON_H
#define AVCODEC_OH_VIDEO_DECODER_COMMON_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <sys/types.h>

#include "libavutil/frame.h"
#include "libavutil/pixfmt.h"

#include "avcodec.h"
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avcapability.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avformat.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <native_buffer/native_buffer.h>
#include "oh_video_decoder_internal_buffer.h"

typedef struct OHVideoDecoderContext {

    AVCodecContext *avctx;
    atomic_int refcount;
    atomic_int hw_buffer_count;

    char *codec_name;

    OH_AVCodec *codec;
    OH_AVFormat *format;
    OHVideoDecoderInternalBuffersGroup *buffers_group;
    int32_t codec_error_code;

    void *native_window;

    void * hw_decode_sei_context;

    int started;
    int draining;
    int flushing;
    int eos;

    int width;
    int height;
    int stride;
    int slice_height;
    int color_format;
    int crop_top;
    int crop_bottom;
    int crop_left;
    int crop_right;
    int display_width;
    int display_height;

    uint64_t output_buffer_count;
//    ssize_t current_input_buffer;

    bool delay_flush;
    atomic_int serial;

} OHVideoDecoderContext;

int ff_oh_video_decoder_init(AVCodecContext *avctx,
                             OHVideoDecoderContext *s,
                             const char *mime,
                             OH_AVFormat *format);

int ff_oh_video_decoder_set_h264_extradata(OHVideoDecoderContext *s, uint8_t *data, size_t size);

int ff_oh_video_decoder_set_hevc_extradata(OHVideoDecoderContext *s, uint8_t *data, size_t size);

int ff_oh_video_decoder_send(AVCodecContext *avctx,
                             OHVideoDecoderContext *s,
                             AVPacket *pkt);

int ff_oh_video_decoder_receive(AVCodecContext *avctx,
                                OHVideoDecoderContext *s,
                                AVFrame *frame);

int ff_oh_video_decoder_flush(AVCodecContext *avctx,
                              OHVideoDecoderContext *s);

int ff_oh_video_decoder_close(AVCodecContext *avctx,
                              OHVideoDecoderContext *s);

int ff_oh_video_decoder_is_flushing(AVCodecContext *avctx,
                                    OHVideoDecoderContext *s);

int ff_oh_video_decoder_render_output_buffer(void *opaque, uint8_t *data);


typedef struct OHVideoDecoderBuffer {

    OHVideoDecoderContext *ctx;
    ssize_t index;
    int64_t pts;
    atomic_int released;
    int serial;

} OHVideoDecoderBuffer;


#endif //QPLAYER_FFMPEG_OH_MEDIACODECDEC_COMMON_H
