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

#include <string.h>
#include <sys/types.h>

#include "libavutil/common.h"
#include "libavutil/hwcontext_oh.h"
#include "libavutil/mem.h"
#include "libavutil/log.h"
#include "libavutil/pixfmt.h"
#include "libavutil/time.h"
#include "libavutil/timestamp.h"

#include "avcodec.h"
#include "decode.h"

#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avcapability.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avformat.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <native_buffer/native_buffer.h>
#include "oh_video_decoder_common.h"

//static struct {
//    enum OH_TransferCharacteristic oh_transfer;
//    enum AVColorTransferCharacteristic transfer;
//} color_transfer_map[] = {
//        { COLOR_TRANSFER_LINEAR,        AVCOL_TRC_LINEAR        },
//        { COLOR_TRANSFER_SDR_VIDEO,     AVCOL_TRC_SMPTE170M     },
//        { COLOR_TRANSFER_ST2084,        AVCOL_TRC_SMPTEST2084   },
//        { COLOR_TRANSFER_HLG,           AVCOL_TRC_ARIB_STD_B67  },
//};
int format_to_oh_video_decoder_context(AVCodecContext *avctx, OHVideoDecoderContext *s, OH_AVFormat *format);

static enum AVColorTransferCharacteristic OHFormatColorTransfer_to_AVColorTransfer(OH_TransferCharacteristic color_transfer)
{
    return (enum AVColorTransferCharacteristic)(color_transfer);
}

static enum OH_TransferCharacteristic OHFormatColorTransfer_from_AVColorTransfer(
        enum AVColorTransferCharacteristic color_transfer)
{
    return (enum OH_TransferCharacteristic)(color_transfer);

}


static enum AVColorSpace OHFormatColorPrimary_to_AVColorSpace(OH_ColorPrimary color_primary)
{


    return (enum AVColorSpace)color_primary;
}

static enum OH_ColorPrimary OHFormatColorPrimary_from_AVColorSpace(enum AVColorSpace color_space)
{
    return (enum OH_ColorPrimary)color_space;
}


static enum AVColorPrimaries OHFormatColorPrimary_to_AVColorPrimaries(enum OH_ColorPrimary color_primary)
{
    return (enum AVColorPrimaries)color_primary;
}



static void ff_oh_video_decoder_ref(OHVideoDecoderContext *s) {
    atomic_fetch_add(&s->refcount, 1);
}

static void ff_oh_video_decoder_unref(OHVideoDecoderContext *s) {
    if (!s)
        return;

    if (atomic_fetch_sub(&s->refcount, 1) == 1) {
        if (s->codec) {
            OH_VideoDecoder_Destroy(s->codec);
            s->codec = NULL;
        }

        if (s->format) {
            OH_AVFormat_Destroy(s->format);
            s->format = NULL;
        }

        if (s->buffers_group) {
            oh_video_decoder_internal_buffer_group_destroy(s->buffers_group);
            s->buffers_group = NULL;
        }

        s->native_window = NULL;
        s->codec_name = NULL;
        av_freep(&s);
    }
}

static void on_error(OH_AVCodec *codec, int32_t error_code, void *user_data) {
    OHVideoDecoderContext *s = (OHVideoDecoderContext *)(user_data);
//    s->codec_error_code = error_code;

    av_log(NULL, AV_LOG_ERROR, "oh video decoder error code=%d\n", error_code);
}

// 解码数据流变化回调OH_AVCodecOnStreamChanged实现
static void on_stream_changed(OH_AVCodec *codec, OH_AVFormat *format, void *user_data) {

    OHVideoDecoderContext *s = (OHVideoDecoderContext *)(user_data);
    OH_AVFormat_Copy(s->format, format);
    format_to_oh_video_decoder_context(s->avctx, s, format);
}

// 解码输入回调OH_AVCodecOnNeedInputBuffer实现
static void on_need_input_buffer(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *user_data) {
    if (user_data == NULL) {
        return;
    }
    OHVideoDecoderContext *s = (OHVideoDecoderContext *)(user_data);
    OHVideoDecoderInternalBuffersGroup *buffers_group = s->buffers_group;
    oh_video_decoder_internal_buffer_group_push_input_buffer(buffers_group, oh_video_decoder_internal_buffer_create(index, buffer));
}

// 解码输出回调OH_AVCodecOnNewOutputBuffer实现
static void on_new_output_buffer(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *user_data) {
    if (user_data == NULL) {
        return;
    }
    OHVideoDecoderContext *s = (OHVideoDecoderContext *)(user_data);
    OHVideoDecoderInternalBuffersGroup *buffers_group = s->buffers_group;
    oh_video_decoder_internal_buffer_group_push_output_buffer(buffers_group, oh_video_decoder_internal_buffer_create(index, buffer));

}


static void oh_video_decoder_buffer_release(void *opaque, uint8_t *data)
{
    OHVideoDecoderBuffer *buffer = opaque;
    OHVideoDecoderContext *ctx = buffer->ctx;
    int released = atomic_load(&buffer->released);

    if (!released && (ctx->delay_flush || buffer->serial == atomic_load(&ctx->serial))) {
        atomic_fetch_sub(&ctx->hw_buffer_count, 1);
        av_log(NULL, AV_LOG_DEBUG,
               "Releasing output buffer %zd (%p) ts=%"PRId64" on free() [%d pending]\n",
                buffer->index, buffer, buffer->pts, atomic_load(&ctx->hw_buffer_count));
        OH_VideoDecoder_RenderOutputBuffer(ctx->codec, buffer->index);
    }

    ff_oh_video_decoder_unref(ctx);
    av_freep(&buffer);
}


static int oh_video_decoder_wrap_hw_buffer(AVCodecContext *avctx,
                                           OHVideoDecoderContext *s,
                                           ssize_t index,
                                           OH_AVCodecBufferAttr *codec_buffer_attr,
                                           AVFrame *frame) {
    int ret = 0;
    int status = 0;
    OHVideoDecoderBuffer *buffer = NULL;

    frame->buf[0] = NULL;
    frame->width = avctx->width;
    frame->height = avctx->height;
    frame->format = avctx->pix_fmt;
    frame->sample_aspect_ratio = avctx->sample_aspect_ratio;

    if (avctx->pkt_timebase.num && avctx->pkt_timebase.den) {
        frame->pts = av_rescale_q(codec_buffer_attr->pts,
                                  AV_TIME_BASE_Q,
                                  avctx->pkt_timebase);
    } else {
        frame->pts = codec_buffer_attr->pts;
    }
    frame->pkt_dts = AV_NOPTS_VALUE;
    frame->color_range = avctx->color_range;
    frame->color_primaries = avctx->color_primaries;
    frame->color_trc = avctx->color_trc;
    frame->colorspace = avctx->colorspace;

    buffer = av_mallocz(sizeof(OHVideoDecoderBuffer));
    if (!buffer) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

//    atomic_init(&buffer->released, 0);

    frame->buf[0] = av_buffer_create(NULL,
                                     0,
                                     oh_video_decoder_buffer_release,
                                     buffer,
                                     AV_BUFFER_FLAG_READONLY);

    if (!frame->buf[0]) {
        ret = AVERROR(ENOMEM);
        goto fail;

    }

    buffer->ctx = s;
    buffer->serial = atomic_load(&s->serial);
    ff_oh_video_decoder_ref(s);

    buffer->index = index;
    buffer->pts = codec_buffer_attr->pts;

    frame->data[3] = (uint8_t *) buffer;

    atomic_fetch_add(&s->hw_buffer_count, 1);
    av_log(avctx, AV_LOG_DEBUG,
           "Wrapping output buffer %zd (%p) ts=%"PRId64" [%d pending]\n",
            buffer->index, buffer, buffer->pts, atomic_load(&s->hw_buffer_count));

    return 0;
fail:
    av_freep(&buffer);
    return ret;
}

static int oh_video_decoder_wrap_sw_buffer(AVCodecContext *avctx,
                                           OHVideoDecoderContext *s,
                                           OH_AVBuffer *buffer,
                                           ssize_t index,
                                           OH_AVCodecBufferAttr *codec_buffer_attr,
                                           AVFrame *frame) {
    //TODO

    return -1;
//    int ret = 0;
//    int status = 0;
//
//    frame->width = avctx->width;
//    frame->height = avctx->height;
//    frame->format = avctx->pix_fmt;
//
//    /* MediaCodec buffers needs to be copied to our own refcounted buffers
//     * because the flush command invalidates all input and output buffers.
//     */
//    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0) {
//        av_log(avctx, AV_LOG_ERROR, "Could not allocate buffer\n");
//        goto done;
//    }
//
//    /* Override frame->pkt_pts as ff_get_buffer will override its value based
//     * on the last avpacket received which is not in sync with the frame:
//     *   * N avpackets can be pushed before 1 frame is actually returned
//     *   * 0-sized avpackets are pushed to flush remaining frames at EOS */
//    if (avctx->pkt_timebase.num && avctx->pkt_timebase.den) {
//        frame->pts = av_rescale_q(codec_buffer_attr->pts,
//                                  AV_TIME_BASE_Q,
//                                  avctx->pkt_timebase);
//    } else {
//        frame->pts = codec_buffer_attr->pts;
//    }
//    frame->pkt_dts = AV_NOPTS_VALUE;
//
//    av_log(avctx, AV_LOG_TRACE,
//           "Frame: width=%d stride=%d height=%d slice-height=%d "
//           "crop-top=%d crop-bottom=%d crop-left=%d crop-right=%d encoder=%s "
//           "destination linesizes=%d,%d,%d\n",
//           avctx->width, s->stride, avctx->height, s->slice_height,
//           s->crop_top, s->crop_bottom, s->crop_left, s->crop_right, s->codec_name,
//           frame->linesize[0], frame->linesize[1], frame->linesize[2]);
//
//    switch (s->color_format) {
//        case COLOR_FormatYUV420Planar:
//            ff_mediacodec_sw_buffer_copy_yuv420_planar(avctx, s, data, size, info, frame);
//            break;
//        case COLOR_FormatYUV420SemiPlanar:
//        case COLOR_QCOM_FormatYUV420SemiPlanar:
//        case COLOR_QCOM_FormatYUV420SemiPlanar32m:
//            ff_mediacodec_sw_buffer_copy_yuv420_semi_planar(avctx, s, data, size, info, frame);
//            break;
//        case COLOR_TI_FormatYUV420PackedSemiPlanar:
//        case COLOR_TI_FormatYUV420PackedSemiPlanarInterlaced:
//            ff_mediacodec_sw_buffer_copy_yuv420_packed_semi_planar(avctx, s, data, size, info, frame);
//            break;
//        case COLOR_QCOM_FormatYUV420PackedSemiPlanar64x32Tile2m8ka:
//            ff_mediacodec_sw_buffer_copy_yuv420_packed_semi_planar_64x32Tile2m8ka(avctx, s, data, size, info, frame);
//            break;
//        default:
//            av_log(avctx, AV_LOG_ERROR, "Unsupported color format 0x%x (value=%d)\n",
//                   s->color_format, s->color_format);
//            ret = AVERROR(EINVAL);
//            goto done;
//    }
//
//    ret = 0;
//    done:
//    status = ff_AMediaCodec_releaseOutputBuffer(s->codec, index, 0);
//    if (status < 0) {
//        av_log(avctx, AV_LOG_ERROR, "Failed to release output buffer\n");
//        ret = AVERROR_EXTERNAL;
//    }
//
//    return ret;
}


static int oh_video_decoder_set_extradata(OHVideoDecoderContext *s, uint8_t *extradata, size_t extradata_size) {
    size_t offset = 0;
    OH_AVCodec *codec = s->codec;
    uint8_t *data;
    size_t size;
    int64_t pts;
    OH_AVCodecBufferAttr codec_buffer_attr;
    OH_AVErrCode error_code;
    if (s->flushing) {
        av_log(s, AV_LOG_ERROR, "Decoder is flushing and cannot accept new buffer "
                                    "until all output buffers have been released\n");
        return AVERROR_EXTERNAL;
    }


    while (offset < extradata_size) {
        OHVideoDecoderInternalBuffer *input_buffer = oh_video_decoder_internal_buffer_group_pop_input_buffer(s->buffers_group, 0);
        error_code = OH_AVBuffer_GetBufferAttr(input_buffer->buffer, &codec_buffer_attr);
        if (error_code != AV_ERR_OK) {
            av_log(s, AV_LOG_ERROR, "Failed to get decode buffer attr=%d\n", error_code);
            return AVERROR_EXTERNAL;
        }
        size_t capacity_size = OH_AVBuffer_GetCapacity(input_buffer->buffer);
        av_log(s, AV_LOG_DEBUG, "Inputbuffer capacity size=%d capacity_size and it's buffer size is enough for data\n", capacity_size);

        data = OH_AVBuffer_GetAddr(input_buffer->buffer);
        if (data == NULL) {
            av_log(s, AV_LOG_ERROR, "Failed to get decode buffer addr=NULL\n");
            return AVERROR_EXTERNAL;
        }

        codec_buffer_attr.flags = AVCODEC_BUFFER_FLAGS_CODEC_DATA;

        size = FFMIN(extradata_size - offset, size);

        codec_buffer_attr.size = size;
        codec_buffer_attr.offset = 0;

        error_code = OH_AVBuffer_SetBufferAttr(input_buffer->buffer, &codec_buffer_attr);
        if (error_code != AV_ERR_OK) {
            av_log(s, AV_LOG_ERROR, "Failed to set buffer attr error_code=%d\n", error_code);
            return AVERROR_EXTERNAL;
        }

        memcpy(data, extradata + offset, size);
        offset += size;

        error_code = OH_VideoDecoder_PushInputBuffer(s->codec, input_buffer->buffer_index);
        if (error_code != AV_ERR_OK) {
            av_log(s, AV_LOG_ERROR, "Failed to push input buffer error_code=%d\n", error_code);
            return AVERROR_EXTERNAL;
        }

        av_log(s, AV_LOG_TRACE,
               "Queued input extradata %zd size=%zd ts=%"
        PRIi64
        "\n", input_buffer->buffer_index, size, pts);
    }

    if (offset == 0)
        return AVERROR(EAGAIN);
    return offset;
}

static enum AVPixelFormat oh_map_color_format(AVCodecContext *avctx,
                                                 OHVideoDecoderContext *s,
                                                 int color_format)
{
    int i;
    enum AVPixelFormat ret = AV_PIX_FMT_NONE;

    if (s->native_window) {
        return AV_PIX_FMT_OH;
    }

    //set by oh_video_decoder OH_AVFormat_SetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
    return AV_PIX_FMT_NV12;

//    if (!strcmp(s->codec_name, "OMX.k3.video.decoder.avc") && color_format == COLOR_FormatYCbYCr) {
//        s->color_format = color_format = COLOR_TI_FormatYUV420PackedSemiPlanar;
//    }
//
//    for (i = 0; i < FF_ARRAY_ELEMS(color_formats); i++) {
//        if (color_formats[i].color_format == color_format) {
//            return color_formats[i].pix_fmt;
//        }
//    }
//
//    av_log(avctx, AV_LOG_ERROR, "Output color format 0x%x (value=%d) is not supported\n",
//           color_format, color_format);
//
//    return ret;
}


int format_to_oh_video_decoder_context(AVCodecContext *avctx, OHVideoDecoderContext *s, OH_AVFormat *format) {
    int ret = 0;
    int width = 0;
    int height = 0;
    int color_range = 0;
    int color_primary = 0;
    int color_transfer = 0;
//    char *format = NULL;

    if (s == NULL || format == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Output OHFormat is not set\n");
        return AVERROR(EINVAL);
    }

    /* Mandatory fields */
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_WIDTH, &(s->width));
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_HEIGHT, &(s->height));
    int32_t stride = 0;
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_STRIDE, &stride);
    s->stride = s->stride > 0 ? s->stride : s->width;

    OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_SLICE_HEIGHT, &(s->slice_height));



    if (strstr(s->codec_name, "OMX.Nvidia.") && s->slice_height == 0) {
        s->slice_height = FFALIGN(s->height, 16);
    } else if (strstr(s->codec_name, "OMX.SEC.avc.dec")) {
        s->slice_height = avctx->height;
        s->stride = avctx->width;
    } else if (s->slice_height == 0) {
        s->slice_height = s->height;
    }
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, &(s->color_format));
    avctx->pix_fmt = oh_map_color_format(avctx, s, s->color_format);
    if (avctx->pix_fmt == AV_PIX_FMT_NONE) {
        av_log(avctx, AV_LOG_ERROR, "Output color format is not supported\n");
        ret = AVERROR(EINVAL);
        goto fail;
    }

    /* Optional fields */
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_CROP_TOP, &(s->crop_top));
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_CROP_BOTTOM, &(s->crop_bottom));
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_CROP_LEFT, &(s->crop_left));
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_CROP_RIGHT, &(s->crop_right));

    if (s->crop_right && s->crop_bottom) {
        width = s->crop_right + 1 - s->crop_left;
        height = s->crop_bottom + 1 - s->crop_top;
    } else {
        /* TODO: NDK MediaFormat should try getRect() first.
         * Try crop-width/crop-height, it works on NVIDIA Shield.
         */
//        OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_CROP_TOP, &(width));
//        OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_CROP_BOTTOM, &(height));
//
//        AMEDIAFORMAT_GET_INT32(,  "crop-width",  0);
//        AMEDIAFORMAT_GET_INT32(, "crop-height", 0);
    }
    if (!width || !height) {
        width = s->width;
        height = s->height;
    }
//
//    AMEDIAFORMAT_GET_INT32(s->display_width,  "display-width",  0);
//    AMEDIAFORMAT_GET_INT32(s->display_height, "display-height", 0);

//    if (s->display_width && s->display_height) {
//        AVRational sar = av_div_q(
//                (AVRational){ s->display_width, s->display_height },
//                (AVRational){ width, height });
//        ff_set_sar(avctx, sar);
//    }

    int32_t is_full_range = 0;
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_CROP_RIGHT, &(is_full_range));
    if (is_full_range == AVCOL_RANGE_JPEG) {
        avctx->color_range = AVCOL_RANGE_JPEG;
    } else {
        avctx->color_range = AVCOL_RANGE_MPEG;

    }


    if (OH_AVFormat_GetIntValue(format, OH_MD_KEY_COLOR_PRIMARIES, &(color_primary))) {
        avctx->colorspace = OHFormatColorPrimary_to_AVColorSpace(color_primary);
        avctx->color_primaries = OHFormatColorPrimary_to_AVColorPrimaries(color_primary);
    }

    if(OH_AVFormat_GetIntValue(format, OH_MD_KEY_TRANSFER_CHARACTERISTICS, &(color_transfer))) {
        avctx->color_trc = OHFormatColorTransfer_to_AVColorTransfer(color_transfer);
    }


    av_log(avctx, AV_LOG_INFO,
           "Output crop parameters top=%d bottom=%d left=%d right=%d, "
           "resulting dimensions width=%d height=%d\n",
           s->crop_top, s->crop_bottom, s->crop_left, s->crop_right,
           width, height);

    return ff_set_dimensions(avctx, width, height);
    fail:
    return ret;
}


static int oh_decoder_flush_codec(AVCodecContext *avctx, OHVideoDecoderContext *s)
{
    OH_AVCodec *codec = s->codec;
    OH_AVErrCode error_code;

    s->output_buffer_count = 0;

    s->draining = 0;
    s->flushing = 0;
    s->eos = 0;
    atomic_fetch_add(&s->serial, 1);
    atomic_init(&s->hw_buffer_count, 0);


    error_code = OH_VideoDecoder_Flush(s->codec);
    if (error_code != AV_ERR_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to flush codec error=%d\n", error_code);
        return AVERROR_EXTERNAL;
    }
    oh_video_decoder_internal_buffer_group_clear(s->buffers_group);

    error_code = OH_VideoDecoder_Start(s->codec);
    if (error_code != AV_ERR_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to restart codec after flush codec error=%d\n", error_code);
        return AVERROR_EXTERNAL;
    }
    av_log(avctx, AV_LOG_DEBUG, "oh_decoder_flush_codec flush success\n");

    return 0;
}


int ff_oh_video_decoder_init(AVCodecContext *avctx,
                             OHVideoDecoderContext *s,
                             const char *mime,
                             OH_AVFormat *format) {
    int ret = 0;
    int status;
    int profile;

    enum AVPixelFormat pix_fmt;
    static const enum AVPixelFormat pix_fmts[] = {
            AV_PIX_FMT_OH,
            AV_PIX_FMT_NONE,
    };

    s->avctx = avctx;
    atomic_init(&s->refcount, 1);
    atomic_init(&s->hw_buffer_count, 0);
    atomic_init(&s->serial, 1);
//    s->current_input_buffer = -1;

    pix_fmt = ff_get_format(avctx, pix_fmts);
    if (pix_fmt == AV_PIX_FMT_OH) {

        if (avctx->hw_device_ctx) {
            AVHWDeviceContext *device_ctx = (AVHWDeviceContext * )(avctx->hw_device_ctx->data);
            if (device_ctx->type == AV_HWDEVICE_TYPE_OH) {
                if (device_ctx->hwctx) {
                    AVOHDeviceContext *oh_ctx = (AVOHDeviceContext *) device_ctx->hwctx;
                    s->native_window = oh_ctx->native_window;
                    av_log(avctx, AV_LOG_INFO, "Using native window %p\n", s->native_window);
                }
            }
        }
    }

    // 通过codecname创建解码器，应用有特殊需求，比如选择支持某种分辨率规格的解码器，可先查询capability，再根据codec name创建解码器。
    OH_AVCapability *pcapability = OH_AVCodec_GetCapability(mime, false);
    if (pcapability == NULL) {
        av_log(avctx, AV_LOG_WARNING, "Unsupported or unknown capability\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    const char *pname = OH_AVCapability_GetName(pcapability);
    s->codec = OH_VideoDecoder_CreateByName(pname);
    if (s->codec == NULL) {
        av_log(avctx, AV_LOG_WARNING, "Unsupported or unknown profile\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    OH_AVCodecCallback callback = {&on_error, &on_stream_changed, &on_need_input_buffer, &on_new_output_buffer};

    s->buffers_group = oh_video_decoder_internal_buffer_group_create();

    OH_AVErrCode error_code = OH_VideoDecoder_RegisterCallback(s->codec, callback, s);
    if (error_code != AV_ERR_OK) {
        av_log(avctx, AV_LOG_WARNING, "OH Video decoder register callback error=%d\n", error_code);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    s->codec_name = mime;

    error_code = OH_VideoDecoder_Configure(s->codec, format);
    if (error_code != AV_ERR_OK) {
        av_log(avctx, AV_LOG_ERROR,
               "Failed to configure codec %s (error_code = %d)\n",
               s->codec_name, error_code);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if (s->native_window != NULL) {
        error_code = OH_VideoDecoder_SetSurface(s->codec, s->native_window);
        if (error_code != AV_ERR_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to set surface error (error_code = %d)\n", error_code);
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }

    
    error_code = OH_VideoDecoder_Prepare(s->codec);
    if (error_code != AV_ERR_OK) {
        av_log(avctx, AV_LOG_ERROR,
               "Failed to prepare codec %s (error_code = %d)\n",
               s->codec_name, error_code);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    // 启动解码器，开始解码
    error_code = OH_VideoDecoder_Start(s->codec);
    if (ret != AV_ERR_OK) {
        av_log(avctx, AV_LOG_ERROR,
               "Failed to start codec %s (error_code = %d)\n",
               s->codec_name, error_code);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    av_log(avctx, AV_LOG_DEBUG, "OH Video Decoder %p started successfully\n", s->codec);

    return 0;

    fail:
    av_log(avctx, AV_LOG_ERROR, "OH Video Decoder %p failed to start\n", s->codec);
    ff_oh_video_decoder_close(avctx, s);
    return ret;
}

int ff_oh_video_decoder_set_h264_extradata(OHVideoDecoderContext *s, uint8_t *data, size_t size) {
    return oh_video_decoder_set_extradata(s, data, size);
}

int ff_oh_video_decoder_set_hevc_extradata(OHVideoDecoderContext *s, uint8_t *data, size_t size) {
    return oh_video_decoder_set_extradata(s, data, size);
}

int ff_oh_video_decoder_send(AVCodecContext *avctx, OHVideoDecoderContext *s,
                             AVPacket *pkt) {
    size_t offset = 0;
    int need_draining = 0;
    uint8_t *data;
    size_t size;
    OH_AVCodec *codec = s->codec;

    int64_t pts;
    OH_AVCodecBufferAttr codec_buffer_attr;
    OH_AVErrCode error_code;

    if (s->codec_error_code != 0) {
        av_log(avctx, AV_LOG_ERROR, "Decoder has been occur error code=%d\n", s->codec_error_code);
        return AVERROR_EXTERNAL;
    }
    if (s->flushing) {
        av_log(avctx, AV_LOG_ERROR, "Decoder is flushing and cannot accept new buffer "
                                    "until all output buffers have been released\n");
        return AVERROR_EXTERNAL;
    }

    if (pkt->size == 0) {
        need_draining = 1;
    }

    if (s->draining && s->eos) {
        return AVERROR_EOF;
    }

    while (offset < pkt->size || (need_draining && !s->draining)) {
        OHVideoDecoderInternalBuffer *input_buffer = oh_video_decoder_internal_buffer_group_pop_input_buffer(s->buffers_group, 1000);
        if (input_buffer == NULL) {
            break;
        }
        error_code = OH_AVBuffer_GetBufferAttr(input_buffer->buffer, &codec_buffer_attr);
        if (error_code != AV_ERR_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get decode buffer attr=%d\n", error_code);
            return AVERROR_EXTERNAL;
        }
        data = OH_AVBuffer_GetAddr(input_buffer->buffer);
        if (data == NULL) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get decode buffer addr=NULL\n");
            return AVERROR_EXTERNAL;
        }


        pts = pkt->pts;
        if (pts == AV_NOPTS_VALUE) {
            av_log(avctx, AV_LOG_WARNING, "Input packet is missing PTS\n");
            pts = 0;
        }
        if (pts && avctx->pkt_timebase.num && avctx->pkt_timebase.den) {
            pts = av_rescale_q(pts, avctx->pkt_timebase, AV_TIME_BASE_Q);
        }
        if (need_draining) {
            codec_buffer_attr.flags = AVCODEC_BUFFER_FLAGS_EOS;
            codec_buffer_attr.pts = 0;
            codec_buffer_attr.size = 0;
            codec_buffer_attr.offset = 0;
            av_log(avctx, AV_LOG_DEBUG, "Sending End Of Stream signal\n");

            error_code = OH_AVBuffer_SetBufferAttr(input_buffer->buffer, &codec_buffer_attr);
            if (error_code != AV_ERR_OK) {
                av_log(avctx, AV_LOG_ERROR, "Failed to set buffer attr error_code=%d\n", error_code);
                return AVERROR_EXTERNAL;
            }

            error_code = OH_VideoDecoder_PushInputBuffer(s->codec, input_buffer->buffer_index);
            if (error_code != AV_ERR_OK) {
                av_log(avctx, AV_LOG_ERROR, "Failed to push input buffer error_code=%d\n", error_code);
                return AVERROR_EXTERNAL;
            }

            av_log(avctx, AV_LOG_TRACE,
                   "Queued empty EOS input buffer %zd with flags=%d\n", input_buffer->buffer_index, codec_buffer_attr.flags);

            s->draining = 1;
            return 0;
        }


        //TODO:AVCODEC_BUFFER_FLAGS_INCOMPLETE_FRAME / AVCODEC_BUFFER_FLAGS_CODEC_DATA
        if (pkt->flags & AV_PKT_FLAG_KEY) {
            codec_buffer_attr.flags = AVCODEC_BUFFER_FLAGS_SYNC_FRAME;
        } else {
            codec_buffer_attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
        }

        size = FFMIN(pkt->size - offset, size);

        codec_buffer_attr.pts = pts;
        codec_buffer_attr.size = pkt->size;
        codec_buffer_attr.offset = 0;

        error_code = OH_AVBuffer_SetBufferAttr(input_buffer->buffer, &codec_buffer_attr);
        if (error_code != AV_ERR_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to set buffer attr error_code=%d\n", error_code);
            return AVERROR_EXTERNAL;
        }

        memcpy(data, pkt->data + offset, size);
        offset += size;

        error_code = OH_VideoDecoder_PushInputBuffer(s->codec, input_buffer->buffer_index);
        if (error_code != AV_ERR_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to push input buffer error_code=%d\n", error_code);
            return AVERROR_EXTERNAL;
        }

        av_log(avctx, AV_LOG_TRACE,
               "Queued input buffer %zd size=%zd ts=%"
        PRIi64
        "\n", input_buffer->buffer_index, size, pts);
    }

    if (offset == 0)
        return AVERROR(EAGAIN);
    return offset;
}

int ff_oh_video_decoder_receive(AVCodecContext *avctx, OHVideoDecoderContext *s,
                                AVFrame *frame) {
    int ret;
    uint8_t *data;
    size_t size;
    OH_AVCodec *codec = s->codec;
//    FFAMediaCodecBufferInfo info = {0};
    int status;
    OH_AVErrCode error_code;
    OH_AVCodecBufferAttr codec_buffer_attr;

    if (s->codec_error_code != 0) {
        av_log(avctx, AV_LOG_ERROR, "Decoder has been occur error code=%d\n", s->codec_error_code);
        return AVERROR_EXTERNAL;
    }

    if (s->draining && s->eos) {
        return AVERROR_EOF;
    }

    OHVideoDecoderInternalBuffer *output_buffer = oh_video_decoder_internal_buffer_group_pop_output_buffer(s->buffers_group, 1000);
    if (output_buffer != NULL) {
        error_code = OH_AVBuffer_GetBufferAttr(output_buffer->buffer, &codec_buffer_attr);
        if (error_code != AV_ERR_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to pop output buffer error_code=%d\n", error_code);
            return AVERROR_EXTERNAL;
        }

        av_log(avctx, AV_LOG_TRACE, "Got output buffer %zd offset=%"PRIi32" size=%"PRIi32" pts=%"PRIi64" flags=%"PRIu32"\n", output_buffer->buffer_index, codec_buffer_attr.offset, codec_buffer_attr.size,
                codec_buffer_attr.pts, codec_buffer_attr.flags);

        if (codec_buffer_attr.flags == AVCODEC_BUFFER_FLAGS_EOS) {
            s->eos = 1;
        }


         if (codec_buffer_attr.flags | AVCODEC_BUFFER_FLAGS_NONE | AVCODEC_BUFFER_FLAGS_SYNC_FRAME) {
            if (s->native_window) {

//                error_code = OH_VideoDecoder_RenderOutputBuffer(s->codec, output_buffer->buffer_index);
//                if (error_code != AV_ERR_OK) {
//                    av_log(avctx, AV_LOG_ERROR, "Failed to render output buffer error_code=%d\n", error_code);
//                    return AVERROR_EXTERNAL;
//                }

                if ((ret = oh_video_decoder_wrap_hw_buffer(avctx, s, output_buffer->buffer_index, &codec_buffer_attr, frame)) < 0) {
                    av_log(avctx, AV_LOG_ERROR, "Failed to wrap OH video decoder buffer\n");
                    return ret;
                }
            } else {

                if ((ret = oh_video_decoder_wrap_sw_buffer(avctx, s, output_buffer->buffer,
                                                           output_buffer->buffer_index, &codec_buffer_attr, frame)) < 0) {
                    av_log(avctx, AV_LOG_ERROR, "Failed to wrap MediaCodec buffer\n");
                    return ret;
                }

                error_code = OH_VideoDecoder_FreeOutputBuffer(s->codec, output_buffer->buffer_index);
                if (error_code != AV_ERR_OK) {
                    av_log(avctx, AV_LOG_ERROR, "Failed to free output buffer error_code=%d\n", error_code);
                    return AVERROR_EXTERNAL;
                }
            }

            s->output_buffer_count++;
            return 0;
        } 
//         else {
//             error_code = OH_VideoDecoder_FreeOutputBuffer(s->codec, output_buffer->buffer_index);
//             if (error_code != AV_ERR_OK) {
//                 av_log(avctx, AV_LOG_ERROR, "Failed to free output buffer error_code=%d\n", error_code);
//             }
//         }
    }


    //TODO format changed

//    if (index >= 0) {
//
//    } else if (ff_AMediaCodec_infoOutputFormatChanged(codec, index)) {
//        char *format = NULL;
//
//        if (s->format) {
//            status = ff_AMediaFormat_delete(s->format);
//            if (status < 0) {
//                av_log(avctx, AV_LOG_ERROR, "Failed to delete MediaFormat %p\n", s->format);
//            }
//        }
//
//        s->format = ff_AMediaCodec_getOutputFormat(codec);
//        if (!s->format) {
//            av_log(avctx, AV_LOG_ERROR, "Failed to get output format\n");
//            return AVERROR_EXTERNAL;
//        }
//
//        format = ff_AMediaFormat_toString(s->format);
//        if (!format) {
//            return AVERROR_EXTERNAL;
//        }
//        av_log(avctx, AV_LOG_INFO, "Output MediaFormat changed to %s\n", format);
//        av_freep(&format);
//
//        if ((ret = mediacodec_dec_parse_format(avctx, s)) < 0) {
//            return ret;
//        }
//
//    } else if (ff_AMediaCodec_infoOutputBuffersChanged(codec, index)) {
//        ff_AMediaCodec_cleanOutputBuffers(codec);
//    } else if (ff_AMediaCodec_infoTryAgainLater(codec, index)) {
//        if (s->draining) {
//            av_log(avctx, AV_LOG_ERROR, "Failed to dequeue output buffer within %"
//            PRIi64
//            "ms "
//            "while draining remaining frames, output will probably lack frames\n",
//                    output_dequeue_timeout_us / 1000);
//        } else {
//            av_log(avctx, AV_LOG_TRACE, "No output buffer available, try again later\n");
//        }
//    } else {
//        av_log(avctx, AV_LOG_ERROR, "Failed to dequeue output buffer (status=%zd)\n", index);
//        return AVERROR_EXTERNAL;
//    }
//
    if (s->draining && s->eos)
        return AVERROR_EOF;
    return AVERROR(EAGAIN);
}

int ff_oh_video_decoder_flush(AVCodecContext *avctx,
                              OHVideoDecoderContext *s) {
    if (!s->native_window || !s->delay_flush || atomic_load(&s->refcount) == 1) {
        int ret;

        /* No frames (holding a reference to the codec) are retained by the
         * user, thus we can flush the codec and returns accordingly */
        if ((ret = oh_decoder_flush_codec(avctx, s)) < 0) {
            return ret;
        }
        return 1;
    }

    s->flushing = 1;
    return 0;
}

int ff_oh_video_decoder_close(AVCodecContext *avctx,
                              OHVideoDecoderContext *s) {
    ff_oh_video_decoder_unref(s);
    return 0;
}

int ff_oh_video_decoder_is_flushing(AVCodecContext *avctx,
                                    OHVideoDecoderContext *s) {
    return s->flushing;
}


