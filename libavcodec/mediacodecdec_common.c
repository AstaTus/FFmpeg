/*
 * Android MediaCodec decoder
 *
 * Copyright (c) 2015-2016 Matthieu Bouron <matthieu.bouron stupeflix.com>
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
#include "libavutil/hwcontext_mediacodec.h"
#include "libavutil/mem.h"
#include "libavutil/log.h"
#include "libavutil/pixfmt.h"
#include "libavutil/time.h"
#include "libavutil/timestamp.h"

#include "avcodec.h"
#include "internal.h"

#include "mediacodec.h"
#include "mediacodec_surface.h"
#include "mediacodec_sw_buffer.h"
#include "mediacodec_wrapper.h"
#include "mediacodecdec_common.h"

#include "h2645_parse.h"
#include "h264.h"
#include "hevc.h"
#include "hevc_sei.h"
#include "h264_sei.h"
#include "codec_id.h"
#include "h264_parse.h"
#include "hevc_parse.h"
#include "mediacodecdec_internal.h"
/**
 * OMX.k3.video.decoder.avc, OMX.NVIDIA.* OMX.SEC.avc.dec and OMX.google
 * codec workarounds used in various place are taken from the Gstreamer
 * project.
 *
 * Gstreamer references:
 * https://cgit.freedesktop.org/gstreamer/gst-plugins-bad/tree/sys/androidmedia/
 *
 * Gstreamer copyright notice:
 *
 * Copyright (C) 2012, Collabora Ltd.
 *   Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>
 *
 * Copyright (C) 2012, Rafaël Carré <funman@videolanorg>
 *
 * Copyright (C) 2015, Sebastian Dröge <sebastian@centricular.com>
 *
 * Copyright (C) 2014-2015, Collabora Ltd.
 *   Author: Matthieu Bouron <matthieu.bouron@gcollabora.com>
 *
 * Copyright (C) 2015, Edward Hervey
 *   Author: Edward Hervey <bilboed@gmail.com>
 *
 * Copyright (C) 2015, Matthew Waters <matthew@centricular.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */

#define INPUT_DEQUEUE_TIMEOUT_US 8000
#define OUTPUT_DEQUEUE_TIMEOUT_US 8000
#define OUTPUT_DEQUEUE_BLOCK_TIMEOUT_US 1000000

enum {
    COLOR_RANGE_FULL    = 0x1,
    COLOR_RANGE_LIMITED = 0x2,
};

static enum AVColorRange mcdec_get_color_range(int color_range)
{
    switch (color_range) {
    case COLOR_RANGE_FULL:
        return AVCOL_RANGE_JPEG;
    case COLOR_RANGE_LIMITED:
        return AVCOL_RANGE_MPEG;
    default:
        return AVCOL_RANGE_UNSPECIFIED;
    }
}

enum {
    COLOR_STANDARD_BT709      = 0x1,
    COLOR_STANDARD_BT601_PAL  = 0x2,
    COLOR_STANDARD_BT601_NTSC = 0x4,
    COLOR_STANDARD_BT2020     = 0x6,
};

static enum AVColorSpace mcdec_get_color_space(int color_standard)
{
    switch (color_standard) {
    case COLOR_STANDARD_BT709:
        return AVCOL_SPC_BT709;
    case COLOR_STANDARD_BT601_PAL:
        return AVCOL_SPC_BT470BG;
    case COLOR_STANDARD_BT601_NTSC:
        return AVCOL_SPC_SMPTE170M;
    case COLOR_STANDARD_BT2020:
        return AVCOL_SPC_BT2020_NCL;
    default:
        return AVCOL_SPC_UNSPECIFIED;
    }
}

static enum AVColorPrimaries mcdec_get_color_pri(int color_standard)
{
    switch (color_standard) {
    case COLOR_STANDARD_BT709:
        return AVCOL_PRI_BT709;
    case COLOR_STANDARD_BT601_PAL:
        return AVCOL_PRI_BT470BG;
    case COLOR_STANDARD_BT601_NTSC:
        return AVCOL_PRI_SMPTE170M;
    case COLOR_STANDARD_BT2020:
        return AVCOL_PRI_BT2020;
    default:
        return AVCOL_PRI_UNSPECIFIED;
    }
}

enum {
    COLOR_TRANSFER_LINEAR    = 0x1,
    COLOR_TRANSFER_SDR_VIDEO = 0x3,
    COLOR_TRANSFER_ST2084    = 0x6,
    COLOR_TRANSFER_HLG       = 0x7,
};

static enum AVColorTransferCharacteristic mcdec_get_color_trc(int color_transfer)
{
    switch (color_transfer) {
    case COLOR_TRANSFER_LINEAR:
        return AVCOL_TRC_LINEAR;
    case COLOR_TRANSFER_SDR_VIDEO:
        return AVCOL_TRC_SMPTE170M;
    case COLOR_TRANSFER_ST2084:
        return AVCOL_TRC_SMPTEST2084;
    case COLOR_TRANSFER_HLG:
        return AVCOL_TRC_ARIB_STD_B67;
    default:
        return AVCOL_TRC_UNSPECIFIED;
    }
}

enum {
    COLOR_FormatYUV420Planar                              = 0x13,
    COLOR_FormatYUV420SemiPlanar                          = 0x15,
    COLOR_FormatYCbYCr                                    = 0x19,
    COLOR_FormatAndroidOpaque                             = 0x7F000789,
    COLOR_QCOM_FormatYUV420SemiPlanar                     = 0x7fa30c00,
    COLOR_QCOM_FormatYUV420SemiPlanar32m                  = 0x7fa30c04,
    COLOR_QCOM_FormatYUV420PackedSemiPlanar64x32Tile2m8ka = 0x7fa30c03,
    COLOR_TI_FormatYUV420PackedSemiPlanar                 = 0x7f000100,
    COLOR_TI_FormatYUV420PackedSemiPlanarInterlaced       = 0x7f000001,
};

static const struct {

    int color_format;
    enum AVPixelFormat pix_fmt;

} color_formats[] = {

    { COLOR_FormatYUV420Planar,                              AV_PIX_FMT_YUV420P },
    { COLOR_FormatYUV420SemiPlanar,                          AV_PIX_FMT_NV12    },
    { COLOR_QCOM_FormatYUV420SemiPlanar,                     AV_PIX_FMT_NV12    },
    { COLOR_QCOM_FormatYUV420SemiPlanar32m,                  AV_PIX_FMT_NV12    },
    { COLOR_QCOM_FormatYUV420PackedSemiPlanar64x32Tile2m8ka, AV_PIX_FMT_NV12    },
    { COLOR_TI_FormatYUV420PackedSemiPlanar,                 AV_PIX_FMT_NV12    },
    { COLOR_TI_FormatYUV420PackedSemiPlanarInterlaced,       AV_PIX_FMT_NV12    },
    { 0 }
};

static enum AVPixelFormat mcdec_map_color_format(AVCodecContext *avctx,
                                                 MediaCodecDecContext *s,
                                                 int color_format)
{
    int i;
    enum AVPixelFormat ret = AV_PIX_FMT_NONE;

    if (s->surface) {
        return AV_PIX_FMT_MEDIACODEC;
    }

    if (!strcmp(s->codec_name, "OMX.k3.video.decoder.avc") && color_format == COLOR_FormatYCbYCr) {
        s->color_format = color_format = COLOR_TI_FormatYUV420PackedSemiPlanar;
    }

    for (i = 0; i < FF_ARRAY_ELEMS(color_formats); i++) {
        if (color_formats[i].color_format == color_format) {
            return color_formats[i].pix_fmt;
        }
    }

    av_log(avctx, AV_LOG_ERROR, "Output color format 0x%x (value=%d) is not supported\n",
        color_format, color_format);

    return ret;
}

static void ff_mediacodec_dec_ref(MediaCodecDecContext *s)
{
    atomic_fetch_add(&s->refcount, 1);
}

static void ff_mediacodec_dec_unref(MediaCodecDecContext *s)
{
    if (!s)
        return;

    if (atomic_fetch_sub(&s->refcount, 1) == 1) {
        if (s->codec) {
            ff_AMediaCodec_delete(s->codec);
            s->codec = NULL;
        }

        if (s->format) {
            ff_AMediaFormat_delete(s->format);
            s->format = NULL;
        }

        if (s->surface) {
            ff_mediacodec_surface_unref(s->surface, NULL);
            s->surface = NULL;
        }

        av_freep(&s->codec_name);
        av_freep(&s);
    }
}

static void mediacodec_buffer_release(void *opaque, uint8_t *data)
{
    AVMediaCodecBuffer *buffer = opaque;
    MediaCodecDecContext *ctx = buffer->ctx;
    int released = atomic_load(&buffer->released);

    if (!released && (ctx->delay_flush || buffer->serial == atomic_load(&ctx->serial))) {
        atomic_fetch_sub(&ctx->hw_buffer_count, 1);
        av_log(NULL, AV_LOG_DEBUG,
               "Releasing output buffer %zd (%p) ts=%"PRId64" on free() [%d pending]\n",
               buffer->index, buffer, buffer->pts, atomic_load(&ctx->hw_buffer_count));
        ff_AMediaCodec_releaseOutputBuffer(ctx->codec, buffer->index, 0);
    }

    if (ctx->delay_flush)
        ff_mediacodec_dec_unref(ctx);
    av_freep(&buffer);
}

static int mediacodec_export_sei_data(AVCodecContext *avctx, AVFrame *frame) {
    MediaCodecH264DecContext *h = (MediaCodecH264DecContext *)avctx->priv_data;

    for (int i = 0; i < h->h264_sei.unregistered.nb_buf_ref; i++) {
        H264SEIUnregistered *unreg = &h->h264_sei.unregistered;

        if (unreg->buf_ref[i]) {
            AVFrameSideData *sd = av_frame_new_side_data_from_buf(frame,
                                                                  AV_FRAME_DATA_SEI_UNREGISTERED,
                                                                  unreg->buf_ref[i]);
            if (!sd)
                av_buffer_unref(&unreg->buf_ref[i]);
            unreg->buf_ref[i] = NULL;
        }
    }
    h->h264_sei.unregistered.nb_buf_ref = 0;
    return 0;
}

static int mediacodec_wrap_hw_buffer(AVCodecContext *avctx,
                                  MediaCodecDecContext *s,
                                  ssize_t index,
                                  FFAMediaCodecBufferInfo *info,
                                  AVFrame *frame)
{
    int ret = 0;
    int status = 0;
    AVMediaCodecBuffer *buffer = NULL;

    frame->buf[0] = NULL;
    frame->width = avctx->width;
    frame->height = avctx->height;
    frame->format = avctx->pix_fmt;
    frame->sample_aspect_ratio = avctx->sample_aspect_ratio;

    if (avctx->pkt_timebase.num && avctx->pkt_timebase.den) {
        frame->pts = av_rescale_q(info->presentationTimeUs,
                                      AV_TIME_BASE_Q,
                                      avctx->pkt_timebase);
    } else {
        frame->pts = info->presentationTimeUs;
    }
#if FF_API_PKT_PTS
FF_DISABLE_DEPRECATION_WARNINGS
    frame->pkt_pts = frame->pts;
FF_ENABLE_DEPRECATION_WARNINGS
#endif
    frame->pkt_dts = AV_NOPTS_VALUE;
    frame->color_range = avctx->color_range;
    frame->color_primaries = avctx->color_primaries;
    frame->color_trc = avctx->color_trc;
    frame->colorspace = avctx->colorspace;

    buffer = av_mallocz(sizeof(AVMediaCodecBuffer));
    if (!buffer) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    atomic_init(&buffer->released, 0);

    frame->buf[0] = av_buffer_create(NULL,
                                     0,
                                     mediacodec_buffer_release,
                                     buffer,
                                     AV_BUFFER_FLAG_READONLY);

    if (!frame->buf[0]) {
        ret = AVERROR(ENOMEM);
        goto fail;

    }

    buffer->ctx = s;
    buffer->serial = atomic_load(&s->serial);
    if (s->delay_flush)
        ff_mediacodec_dec_ref(s);

    buffer->index = index;
    buffer->pts = info->presentationTimeUs;

    frame->data[3] = (uint8_t *)buffer;
    mediacodec_export_sei_data(avctx, frame);
    atomic_fetch_add(&s->hw_buffer_count, 1);
    av_log(avctx, AV_LOG_DEBUG,
            "Wrapping output buffer %zd (%p) ts=%"PRId64" [%d pending]\n",
            buffer->index, buffer, buffer->pts, atomic_load(&s->hw_buffer_count));

    return 0;
fail:
    av_freep(buffer);
    av_buffer_unref(&frame->buf[0]);
    status = ff_AMediaCodec_releaseOutputBuffer(s->codec, index, 0);
    if (status < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to release output buffer\n");
        ret = AVERROR_EXTERNAL;
    }

    return ret;
}

static int mediacodec_wrap_sw_buffer(AVCodecContext *avctx,
                                  MediaCodecDecContext *s,
                                  uint8_t *data,
                                  size_t size,
                                  ssize_t index,
                                  FFAMediaCodecBufferInfo *info,
                                  AVFrame *frame)
{
    int ret = 0;
    int status = 0;

    frame->width = avctx->width;
    frame->height = avctx->height;
    frame->format = avctx->pix_fmt;

    /* MediaCodec buffers needs to be copied to our own refcounted buffers
     * because the flush command invalidates all input and output buffers.
     */
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Could not allocate buffer\n");
        goto done;
    }

    /* Override frame->pkt_pts as ff_get_buffer will override its value based
     * on the last avpacket received which is not in sync with the frame:
     *   * N avpackets can be pushed before 1 frame is actually returned
     *   * 0-sized avpackets are pushed to flush remaining frames at EOS */
    if (avctx->pkt_timebase.num && avctx->pkt_timebase.den) {
        frame->pts = av_rescale_q(info->presentationTimeUs,
                                      AV_TIME_BASE_Q,
                                      avctx->pkt_timebase);
    } else {
        frame->pts = info->presentationTimeUs;
    }
#if FF_API_PKT_PTS
FF_DISABLE_DEPRECATION_WARNINGS
    frame->pkt_pts = frame->pts;
FF_ENABLE_DEPRECATION_WARNINGS
#endif
    frame->pkt_dts = AV_NOPTS_VALUE;
    mediacodec_export_sei_data(avctx, frame);

    av_log(avctx, AV_LOG_TRACE,
            "Frame: width=%d stride=%d height=%d slice-height=%d "
            "crop-top=%d crop-bottom=%d crop-left=%d crop-right=%d encoder=%s "
            "destination linesizes=%d,%d,%d\n" ,
            avctx->width, s->stride, avctx->height, s->slice_height,
            s->crop_top, s->crop_bottom, s->crop_left, s->crop_right, s->codec_name,
            frame->linesize[0], frame->linesize[1], frame->linesize[2]);

    switch (s->color_format) {
    case COLOR_FormatYUV420Planar:
        ff_mediacodec_sw_buffer_copy_yuv420_planar(avctx, s, data, size, info, frame);
        break;
    case COLOR_FormatYUV420SemiPlanar:
    case COLOR_QCOM_FormatYUV420SemiPlanar:
    case COLOR_QCOM_FormatYUV420SemiPlanar32m:
        ff_mediacodec_sw_buffer_copy_yuv420_semi_planar(avctx, s, data, size, info, frame);
        break;
    case COLOR_TI_FormatYUV420PackedSemiPlanar:
    case COLOR_TI_FormatYUV420PackedSemiPlanarInterlaced:
        ff_mediacodec_sw_buffer_copy_yuv420_packed_semi_planar(avctx, s, data, size, info, frame);
        break;
    case COLOR_QCOM_FormatYUV420PackedSemiPlanar64x32Tile2m8ka:
        ff_mediacodec_sw_buffer_copy_yuv420_packed_semi_planar_64x32Tile2m8ka(avctx, s, data, size, info, frame);
        break;
    default:
        av_log(avctx, AV_LOG_ERROR, "Unsupported color format 0x%x (value=%d)\n",
            s->color_format, s->color_format);
        ret = AVERROR(EINVAL);
        goto done;
    }

    ret = 0;
done:
    status = ff_AMediaCodec_releaseOutputBuffer(s->codec, index, 0);
    if (status < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to release output buffer\n");
        ret = AVERROR_EXTERNAL;
    }

    return ret;
}

#define AMEDIAFORMAT_GET_INT32(name, key, mandatory) do {                              \
    int32_t value = 0;                                                                 \
    if (ff_AMediaFormat_getInt32(s->format, key, &value)) {                            \
        (name) = value;                                                                \
    } else if (mandatory) {                                                            \
        av_log(avctx, AV_LOG_ERROR, "Could not get %s from format %s\n", key, format); \
        ret = AVERROR_EXTERNAL;                                                        \
        goto fail;                                                                     \
    }                                                                                  \
} while (0)                                                                            \

static int mediacodec_dec_parse_format(AVCodecContext *avctx, MediaCodecDecContext *s)
{
    int ret = 0;
    int width = 0;
    int height = 0;
    int color_range = 0;
    int color_standard = 0;
    int color_transfer = 0;
    char *format = NULL;

    if (!s->format) {
        av_log(avctx, AV_LOG_ERROR, "Output MediaFormat is not set\n");
        return AVERROR(EINVAL);
    }

    format = ff_AMediaFormat_toString(s->format);
    if (!format) {
        return AVERROR_EXTERNAL;
    }
    av_log(avctx, AV_LOG_DEBUG, "Parsing MediaFormat %s\n", format);

    /* Mandatory fields */
    AMEDIAFORMAT_GET_INT32(s->width,  "width", 1);
    AMEDIAFORMAT_GET_INT32(s->height, "height", 1);

    AMEDIAFORMAT_GET_INT32(s->stride, "stride", 0);
    s->stride = s->stride > 0 ? s->stride : s->width;

    AMEDIAFORMAT_GET_INT32(s->slice_height, "slice-height", 0);

    if (strstr(s->codec_name, "OMX.Nvidia.") && s->slice_height == 0) {
        s->slice_height = FFALIGN(s->height, 16);
    } else if (strstr(s->codec_name, "OMX.SEC.avc.dec")) {
        s->slice_height = avctx->height;
        s->stride = avctx->width;
    } else if (s->slice_height == 0) {
        s->slice_height = s->height;
    }

    AMEDIAFORMAT_GET_INT32(s->color_format, "color-format", 1);
    avctx->pix_fmt = mcdec_map_color_format(avctx, s, s->color_format);
    if (avctx->pix_fmt == AV_PIX_FMT_NONE) {
        av_log(avctx, AV_LOG_ERROR, "Output color format is not supported\n");
        ret = AVERROR(EINVAL);
        goto fail;
    }

    /* Optional fields */
    AMEDIAFORMAT_GET_INT32(s->crop_top,    "crop-top",    0);
    AMEDIAFORMAT_GET_INT32(s->crop_bottom, "crop-bottom", 0);
    AMEDIAFORMAT_GET_INT32(s->crop_left,   "crop-left",   0);
    AMEDIAFORMAT_GET_INT32(s->crop_right,  "crop-right",  0);

    width = s->crop_right + 1 - s->crop_left;
    height = s->crop_bottom + 1 - s->crop_top;

    AMEDIAFORMAT_GET_INT32(s->display_width,  "display-width",  0);
    AMEDIAFORMAT_GET_INT32(s->display_height, "display-height", 0);

    if (s->display_width && s->display_height) {
        AVRational sar = av_div_q(
            (AVRational){ s->display_width, s->display_height },
            (AVRational){ width, height });
        ff_set_sar(avctx, sar);
    }

    AMEDIAFORMAT_GET_INT32(color_range, "color-range", 0);
    if (color_range)
        avctx->color_range = mcdec_get_color_range(color_range);

    AMEDIAFORMAT_GET_INT32(color_standard, "color-standard", 0);
    if (color_standard) {
        avctx->colorspace = mcdec_get_color_space(color_standard);
        avctx->color_primaries = mcdec_get_color_pri(color_standard);
    }

    AMEDIAFORMAT_GET_INT32(color_transfer, "color-transfer", 0);
    if (color_transfer)
        avctx->color_trc = mcdec_get_color_trc(color_transfer);

    av_log(avctx, AV_LOG_INFO,
        "Output crop parameters top=%d bottom=%d left=%d right=%d, "
        "resulting dimensions width=%d height=%d\n",
        s->crop_top, s->crop_bottom, s->crop_left, s->crop_right,
        width, height);

    av_freep(&format);
    return ff_set_dimensions(avctx, width, height);
fail:
    av_freep(&format);
    return ret;
}

static int mediacodec_dec_flush_codec(AVCodecContext *avctx, MediaCodecDecContext *s)
{
    FFAMediaCodec *codec = s->codec;
    int status;

    s->output_buffer_count = 0;

    s->draining = 0;
    s->flushing = 0;
    s->eos = 0;
    atomic_fetch_add(&s->serial, 1);
    atomic_init(&s->hw_buffer_count, 0);
    s->current_input_buffer = -1;

    status = ff_AMediaCodec_flush(codec);
    if (status < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to flush codec\n");
        return AVERROR_EXTERNAL;
    }

    return 0;
}

int ff_mediacodec_dec_init(AVCodecContext *avctx, MediaCodecDecContext *s,
                           const char *mime, FFAMediaFormat *format)
{
    int ret = 0;
    int status;
    int profile;

    enum AVPixelFormat pix_fmt;
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_MEDIACODEC,
        AV_PIX_FMT_NONE,
    };

    s->avctx = avctx;
    atomic_init(&s->refcount, 1);
    atomic_init(&s->hw_buffer_count, 0);
    atomic_init(&s->serial, 1);
    s->current_input_buffer = -1;

    pix_fmt = ff_get_format(avctx, pix_fmts);
    if (pix_fmt == AV_PIX_FMT_MEDIACODEC) {
        AVMediaCodecContext *user_ctx = avctx->hwaccel_context;

        if (avctx->hw_device_ctx) {
            AVHWDeviceContext *device_ctx = (AVHWDeviceContext*)(avctx->hw_device_ctx->data);
            if (device_ctx->type == AV_HWDEVICE_TYPE_MEDIACODEC) {
                if (device_ctx->hwctx) {
                    AVMediaCodecDeviceContext *mediacodec_ctx = (AVMediaCodecDeviceContext *)device_ctx->hwctx;
                    s->surface = ff_mediacodec_surface_ref(mediacodec_ctx->surface, avctx);
                    av_log(avctx, AV_LOG_INFO, "Using surface %p\n", s->surface);
                }
            }
        }

        if (!s->surface && user_ctx && user_ctx->surface) {
            s->surface = ff_mediacodec_surface_ref(user_ctx->surface, avctx);
            av_log(avctx, AV_LOG_INFO, "Using surface %p\n", s->surface);
        }
    }

    profile = ff_AMediaCodecProfile_getProfileFromAVCodecContext(avctx);
    if (profile < 0) {
        av_log(avctx, AV_LOG_WARNING, "Unsupported or unknown profile\n");
    }

    s->codec_name = ff_AMediaCodecList_getCodecNameByType(mime, profile, 0, avctx);
    if (!s->codec_name) {
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    av_log(avctx, AV_LOG_DEBUG, "Found decoder %s\n", s->codec_name);
    s->codec = ff_AMediaCodec_createCodecByName(s->codec_name);
    if (!s->codec) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create media decoder for type %s and name %s\n", mime, s->codec_name);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    status = ff_AMediaCodec_configure(s->codec, format, s->surface, NULL, 0);
    if (status < 0) {
        char *desc = ff_AMediaFormat_toString(format);
        av_log(avctx, AV_LOG_ERROR,
            "Failed to configure codec %s (status = %d) with format %s\n",
            s->codec_name, status, desc);
        av_freep(&desc);

        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    status = ff_AMediaCodec_start(s->codec);
    if (status < 0) {
        char *desc = ff_AMediaFormat_toString(format);
        av_log(avctx, AV_LOG_ERROR,
            "Failed to start codec %s (status = %d) with format %s\n",
            s->codec_name, status, desc);
        av_freep(&desc);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    s->format = ff_AMediaCodec_getOutputFormat(s->codec);
    if (s->format) {
        if ((ret = mediacodec_dec_parse_format(avctx, s)) < 0) {
            av_log(avctx, AV_LOG_ERROR,
                "Failed to configure context\n");
            goto fail;
        }
    }

    av_log(avctx, AV_LOG_DEBUG, "MediaCodec %p started successfully\n", s->codec);

    return 0;

fail:
    av_log(avctx, AV_LOG_ERROR, "MediaCodec %p failed to start\n", s->codec);
    ff_mediacodec_dec_close(avctx, s);
    return ret;
}


static int hevc_decode_extradata(AVCodecContext *avctx, uint8_t *buf, int length, int first)
{
    int ret, i;
    MediaCodecH264DecContext * s = (MediaCodecH264DecContext *)avctx->priv_data;

    ret = ff_hevc_decode_extradata(buf, length, &s->hevc_ps, &s->hevc_sei, &s->is_nalff,
                                   &s->nal_length_size, avctx->err_recognition,
                                   s->apply_defdispwin, avctx);
    if (ret < 0)
        return ret;

    /* export stream parameters from the first SPS */
//    for (i = 0; i < FF_ARRAY_ELEMS(s->hevc_ps.sps_list); i++) {
//        if (first && s->hevc_ps.sps_list[i]) {
//            const HEVCSPS *sps = (const HEVCSPS*)s->hevc_ps.sps_list[i]->data;
//            export_stream_params(s, sps);
//            break;
//        }
//    }
//
//    /* export stream parameters from SEI */
//    ret = export_stream_params_from_sei(s);
//    if (ret < 0)
//        return ret;

    return 0;
}

static int ff_mediacodec_parse_hevc_sei_data(AVCodecContext *avctx, AVPacket *pkt){

    int ret, i;
    MediaCodecH264DecContext * s = (MediaCodecH264DecContext *)avctx->priv_data;
    const uint8_t *buf = pkt->data;
    int buf_size       = pkt->size;

    buffer_size_t new_extradata_size;
    uint8_t *new_extradata;

    /* end of stream, output what is still in the buffers */
    if (buf_size == 0)
        return 0;

    new_extradata = av_packet_get_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA,
                                            &new_extradata_size);
    if (new_extradata && new_extradata_size > 0) {
        ret = hevc_decode_extradata(avctx, new_extradata, new_extradata_size, 0);
        if (ret < 0)
            return ret;
    }

    ret = ff_h2645_packet_split(&s->hevc_pkt, buf, buf_size, avctx, s->is_nalff,
                                s->nal_length_size, avctx->codec_id, 1, 0);


    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR,
               "Error splitting the input into NAL units.\n");
        return ret;
    }

    for (i = 0; i < s->hevc_pkt.nb_nals; i++) {
        H2645NAL *nal = &s->hevc_pkt.nals[i];

        if (avctx->skip_frame >= AVDISCARD_ALL ||
            (avctx->skip_frame >= AVDISCARD_NONREF) ||
            nal->nuh_layer_id > 0)
            continue;

        switch (nal->type) {
            case HEVC_NAL_SEI_PREFIX:
            case HEVC_NAL_SEI_SUFFIX:
                ret = ff_hevc_decode_nal_sei(&nal->gb, avctx, &s->hevc_sei, &s->hevc_ps, nal->type);
                break;
            default:
                break;
        }
//        if (ret >= 0 && s->overlap > 2)
//            ret = AVERROR_INVALIDDATA;
//        if (ret < 0) {
//            av_log(s->avctx, AV_LOG_WARNING,
//                   "Error parsing NAL unit #%d.\n", i);
//            goto fail;
//        }
    }


    return 0;
}


static int is_avcc_extradata(const uint8_t *buf, int buf_size)
{
    int cnt= buf[5]&0x1f;
    const uint8_t *p= buf+6;
    if (!cnt)
        return 0;
    while(cnt--){
        int nalsize= AV_RB16(p) + 2;
        if(nalsize > buf_size - (p-buf) || (p[2] & 0x9F) != 7)
            return 0;
        p += nalsize;
    }
    cnt = *(p++);
    if(!cnt)
        return 0;
    while(cnt--){
        int nalsize= AV_RB16(p) + 2;
        if(nalsize > buf_size - (p-buf) || (p[2] & 0x9F) != 8)
            return 0;
        p += nalsize;
    }
    return 1;
}


static int ff_mediacodec_parse_h264_sei_data(AVCodecContext *avctx, AVPacket *pkt) {
    int nals_needed = 0; ///< number of NALs that need decoding before the next frame thread starts
    int i, ret = 0;
    const uint8_t *buf = pkt->data;
    int buf_size       = pkt->size;
    MediaCodecH264DecContext *h = (MediaCodecH264DecContext *)avctx->priv_data;

    /* end of stream, output what is still in the buffers */
    if (buf_size == 0)
        return 0;

    if (av_packet_get_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, NULL)) {
        buffer_size_t side_size;
        uint8_t *side = av_packet_get_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, &side_size);
        ff_h264_decode_extradata(side, side_size,
                                 &h->h264_ps, &h->is_avc, &h->nal_length_size,
                                 avctx->err_recognition, avctx);
    }
    if (h->is_avc && buf_size >= 9 && buf[0]==1 && buf[2]==0 && (buf[4]&0xFC)==0xFC) {
        if (is_avcc_extradata(buf, buf_size))
            return ff_h264_decode_extradata(buf, buf_size,
                                            &h->h264_ps, &h->is_avc, &h->nal_length_size,
                                            avctx->err_recognition, avctx);
    }

//    buf_index = decode_nal_units(h, buf, buf_size);


    if (h->nal_length_size == 4) {
        if (buf_size > 8 && AV_RB32(buf) == 1 && AV_RB32(buf+5) > (unsigned)buf_size) {
            h->is_avc = 0;
        }else if(buf_size > 3 && AV_RB32(buf) > 1 && AV_RB32(buf) <= (unsigned)buf_size)
            h->is_avc = 1;
    }

    if (h->is_avc && buf_size >= 9 && buf[0]==1 && buf[2]==0 && (buf[4]&0xFC)==0xFC) {
        if (is_avcc_extradata(buf, buf_size))
            return ff_h264_decode_extradata(buf, buf_size,
                                            &h->h264_ps, &h->is_avc, &h->nal_length_size,
                                            avctx->err_recognition, avctx);
    }

    ret = ff_h2645_packet_split(&h->h264_pkt, buf, buf_size, avctx, h->is_avc, h->nal_length_size,
                                avctx->codec_id, 0, 0);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR,
               "Error splitting the input into NAL units.\n");
        return ret;
    }

//    if (avctx->active_thread_type & FF_THREAD_FRAME)
//        nals_needed = get_last_needed_nal(h);
//    if (nals_needed < 0)
//        return nals_needed;

    for (i = 0; i < h->h264_pkt.nb_nals; i++) {
        H2645NAL *nal = &h->h264_pkt.nals[i];
        int max_slice_ctx, err;

        if (avctx->skip_frame >= AVDISCARD_NONREF &&
            nal->ref_idc == 0 && nal->type != H264_NAL_SEI)
            continue;

        // FIXME these should stop being context-global variables
//        h->nal_ref_idc   = nal->ref_idc;
//        h->nal_unit_type = nal->type;

        switch (nal->type) {
            case H264_NAL_SEI:
                ff_h264_sei_uninit(&h->h264_sei);
//                ret =
                ff_h264_sei_decode(&h->h264_sei, &nal->gb, NULL, avctx);
//                h->has_recovery_point = h->has_recovery_point || h->sei.recovery_point.recovery_frame_cnt != -1;
//                if (avctx->debug & FF_DEBUG_GREEN_MD)
//                    debug_green_metadata(&h->sei.green_metadata, h->avctx);

//                if (ret < 0 && (h->avctx->err_recognition & AV_EF_EXPLODE))
//                    goto end;
                break;
            default:
                break;
//                av_log(avctx, AV_LOG_DEBUG, "Unknown NAL code: %d (%d bits)\n",
//                       nal->type, nal->size_bits);
        }
    }
    return 0;
}



int ff_mediacodec_dec_send(AVCodecContext *avctx, MediaCodecDecContext *s,
                           AVPacket *pkt, bool wait)
{
    int offset = 0;
    int need_draining = 0;
    uint8_t *data;
    size_t size;
    FFAMediaCodec *codec = s->codec;
    int status;
    int64_t input_dequeue_timeout_us = wait ? INPUT_DEQUEUE_TIMEOUT_US : 0;
    int64_t pts;

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

    //parse SEI DATA
    if (avctx->codec_id == AV_CODEC_ID_H264) {
        ff_mediacodec_parse_h264_sei_data(avctx, pkt);
    } else if (avctx->codec_id == AV_CODEC_ID_H265) {
//        ff_mediacodec_parse_hevc_sei_data(avctx, pkt);
    }

    while (offset < pkt->size || (need_draining && !s->draining)) {
        ssize_t index = s->current_input_buffer;
        if (index < 0) {
            index = ff_AMediaCodec_dequeueInputBuffer(codec, input_dequeue_timeout_us);
            if (ff_AMediaCodec_infoTryAgainLater(codec, index)) {
                av_log(avctx, AV_LOG_TRACE, "No input buffer available, try again later\n");
                break;
            }

            if (index < 0) {
                av_log(avctx, AV_LOG_ERROR, "Failed to dequeue input buffer (status=%zd)\n", index);
                return AVERROR_EXTERNAL;
            }
        }
        s->current_input_buffer = -1;

        data = ff_AMediaCodec_getInputBuffer(codec, index, &size);
        if (!data) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get input buffer\n");
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
            uint32_t flags = ff_AMediaCodec_getBufferFlagEndOfStream(codec);

            av_log(avctx, AV_LOG_DEBUG, "Sending End Of Stream signal\n");

            status = ff_AMediaCodec_queueInputBuffer(codec, index, 0, 0, pts, flags);
            if (status < 0) {
                av_log(avctx, AV_LOG_ERROR, "Failed to queue input empty buffer (status = %d)\n", status);
                return AVERROR_EXTERNAL;
            }

            av_log(avctx, AV_LOG_TRACE,
                   "Queued empty EOS input buffer %zd with flags=%d\n", index, flags);

            s->draining = 1;
            return 0;
        }

        size = FFMIN(pkt->size - offset, size);
        memcpy(data, pkt->data + offset, size);
        offset += size;

        status = ff_AMediaCodec_queueInputBuffer(codec, index, 0, size, pts, 0);
        if (status < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to queue input buffer (status = %d)\n", status);
            return AVERROR_EXTERNAL;
        }

        av_log(avctx, AV_LOG_TRACE,
               "Queued input buffer %zd size=%zd ts=%"PRIi64"\n", index, size, pts);
    }

    if (offset == 0)
        return AVERROR(EAGAIN);
    return offset;
}

int ff_mediacodec_dec_receive(AVCodecContext *avctx, MediaCodecDecContext *s,
                              AVFrame *frame, bool wait)
{
    int ret;
    uint8_t *data;
    ssize_t index;
    size_t size;
    FFAMediaCodec *codec = s->codec;
    FFAMediaCodecBufferInfo info = { 0 };
    int status;
    int64_t output_dequeue_timeout_us = OUTPUT_DEQUEUE_TIMEOUT_US;

    if (s->draining && s->eos) {
        return AVERROR_EOF;
    }

    if (s->draining) {
        /* If the codec is flushing or need to be flushed, block for a fair
         * amount of time to ensure we got a frame */
        output_dequeue_timeout_us = OUTPUT_DEQUEUE_BLOCK_TIMEOUT_US;
    } else if (s->output_buffer_count == 0 || !wait) {
        /* If the codec hasn't produced any frames, do not block so we
         * can push data to it as fast as possible, and get the first
         * frame */
        output_dequeue_timeout_us = 0;
    }

    index = ff_AMediaCodec_dequeueOutputBuffer(codec, &info, output_dequeue_timeout_us);
    if (index >= 0) {
        av_log(avctx, AV_LOG_TRACE, "Got output buffer %zd"
                " offset=%" PRIi32 " size=%" PRIi32 " ts=%" PRIi64
                " flags=%" PRIu32 "\n", index, info.offset, info.size,
                info.presentationTimeUs, info.flags);

        if (info.flags & ff_AMediaCodec_getBufferFlagEndOfStream(codec)) {
            s->eos = 1;
        }

        if (info.size) {
            if (s->surface) {
                if ((ret = mediacodec_wrap_hw_buffer(avctx, s, index, &info, frame)) < 0) {
                    av_log(avctx, AV_LOG_ERROR, "Failed to wrap MediaCodec buffer\n");
                    return ret;
                }
            } else {
                data = ff_AMediaCodec_getOutputBuffer(codec, index, &size);
                if (!data) {
                    av_log(avctx, AV_LOG_ERROR, "Failed to get output buffer\n");
                    return AVERROR_EXTERNAL;
                }

                if ((ret = mediacodec_wrap_sw_buffer(avctx, s, data, size, index, &info, frame)) < 0) {
                    av_log(avctx, AV_LOG_ERROR, "Failed to wrap MediaCodec buffer\n");
                    return ret;
                }
            }

            s->output_buffer_count++;
            return 0;
        } else {
            status = ff_AMediaCodec_releaseOutputBuffer(codec, index, 0);
            if (status < 0) {
                av_log(avctx, AV_LOG_ERROR, "Failed to release output buffer\n");
            }
        }

    } else if (ff_AMediaCodec_infoOutputFormatChanged(codec, index)) {
        char *format = NULL;

        if (s->format) {
            status = ff_AMediaFormat_delete(s->format);
            if (status < 0) {
                av_log(avctx, AV_LOG_ERROR, "Failed to delete MediaFormat %p\n", s->format);
            }
        }

        s->format = ff_AMediaCodec_getOutputFormat(codec);
        if (!s->format) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get output format\n");
            return AVERROR_EXTERNAL;
        }

        format = ff_AMediaFormat_toString(s->format);
        if (!format) {
            return AVERROR_EXTERNAL;
        }
        av_log(avctx, AV_LOG_INFO, "Output MediaFormat changed to %s\n", format);
        av_freep(&format);

        if ((ret = mediacodec_dec_parse_format(avctx, s)) < 0) {
            return ret;
        }

    } else if (ff_AMediaCodec_infoOutputBuffersChanged(codec, index)) {
        ff_AMediaCodec_cleanOutputBuffers(codec);
    } else if (ff_AMediaCodec_infoTryAgainLater(codec, index)) {
        if (s->draining) {
            av_log(avctx, AV_LOG_ERROR, "Failed to dequeue output buffer within %" PRIi64 "ms "
                                        "while draining remaining frames, output will probably lack frames\n",
                                        output_dequeue_timeout_us / 1000);
        } else {
            av_log(avctx, AV_LOG_TRACE, "No output buffer available, try again later\n");
        }
    } else {
        av_log(avctx, AV_LOG_ERROR, "Failed to dequeue output buffer (status=%zd)\n", index);
        return AVERROR_EXTERNAL;
    }

    return AVERROR(EAGAIN);
}

/*
* ff_mediacodec_dec_flush returns 0 if the flush cannot be performed on
* the codec (because the user retains frames). The codec stays in the
* flushing state.
*
* ff_mediacodec_dec_flush returns 1 if the flush can actually be
* performed on the codec. The codec leaves the flushing state and can
* process again packets.
*
* ff_mediacodec_dec_flush returns a negative value if an error has
* occurred.
*/
int ff_mediacodec_dec_flush(AVCodecContext *avctx, MediaCodecDecContext *s)
{
    if (!s->surface || atomic_load(&s->refcount) == 1) {
        int ret;

        /* No frames (holding a reference to the codec) are retained by the
         * user, thus we can flush the codec and returns accordingly */
        if ((ret = mediacodec_dec_flush_codec(avctx, s)) < 0) {
            return ret;
        }

        return 1;
    }

    s->flushing = 1;
    return 0;
}

int ff_mediacodec_dec_close(AVCodecContext *avctx, MediaCodecDecContext *s)
{
    ff_mediacodec_dec_unref(s);

    return 0;
}

int ff_mediacodec_dec_is_flushing(AVCodecContext *avctx, MediaCodecDecContext *s)
{
    return s->flushing;
}
