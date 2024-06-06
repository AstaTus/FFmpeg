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


#include <stdint.h>
#include <string.h>

#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/pixfmt.h"
#include "libavutil/internal.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "h264_parse.h"
#include "h264_ps.h"
#include "hevc_parse.h"
#include "hwconfig.h"
#include "internal.h"
#include "jni.h"
#include "oh_video_decoder_common.h"

typedef struct OHVideoDecoderH264DecContext {

    AVClass *avclass;

    OHVideoDecoderContext *ctx;

    AVPacket buffered_pkt;

    int delay_flush;

} OHVideoDecoderH264DecContext;

static av_cold int oh_video_decoder_close(AVCodecContext *avctx)
{
    OHVideoDecoderH264DecContext *s = avctx->priv_data;

    ff_oh_video_decoder_close(avctx, s->ctx);
    s->ctx = NULL;

    av_packet_unref(&s->buffered_pkt);

    return 0;
}


//#if CONFIG_H264_OH_DECODER || CONFIG_HEVC_OH_DECODER
static int h2645_ps_to_nalu(const uint8_t *src, int src_size, uint8_t **out, int *out_size)
{
    int i;
    int ret = 0;
    uint8_t *p = NULL;
    static const uint8_t nalu_header[] = { 0x00, 0x00, 0x00, 0x01 };

    if (!out || !out_size) {
        return AVERROR(EINVAL);
    }

    p = av_malloc(sizeof(nalu_header) + src_size);
    if (!p) {
        return AVERROR(ENOMEM);
    }

    *out = p;
    *out_size = sizeof(nalu_header) + src_size;

    memcpy(p, nalu_header, sizeof(nalu_header));
    memcpy(p + sizeof(nalu_header), src, src_size);

    /* Escape 0x00, 0x00, 0x0{0-3} pattern */
    for (i = 4; i < *out_size; i++) {
        if (i < *out_size - 3 &&
            p[i + 0] == 0 &&
            p[i + 1] == 0 &&
            p[i + 2] <= 3) {
            uint8_t *new;

            *out_size += 1;
            new = av_realloc(*out, *out_size);
            if (!new) {
                ret = AVERROR(ENOMEM);
                goto done;
            }
            *out = p = new;

            i = i + 2;
            memmove(p + i + 1, p + i, *out_size - (i + 1));
            p[i] = 0x03;
        }
    }
done:
    if (ret < 0) {
        av_freep(out);
        *out_size = 0;
    }

    return ret;
}
//#endif

//#if CONFIG_H264_OH_DECODER
static int h264_set_extradata(AVCodecContext *avctx, OHVideoDecoderContext *s)
{
    int i;
    int ret;

    H264ParamSets ps;
    const PPS *pps = NULL;
    const SPS *sps = NULL;
    int is_avc = 0;
    int nal_length_size = 0;

    memset(&ps, 0, sizeof(ps));

    ret = ff_h264_decode_extradata(avctx->extradata, avctx->extradata_size,
                                   &ps, &is_avc, &nal_length_size, 0, avctx);
    if (ret < 0) {
        goto done;
    }

    for (i = 0; i < MAX_PPS_COUNT; i++) {
        if (ps.pps_list[i]) {
            pps = ps.pps_list[i];
            break;
        }
    }

    if (pps) {
        if (ps.sps_list[pps->sps_id]) {
            sps = ps.sps_list[pps->sps_id];
        }
    }

    if (pps && sps) {
        uint8_t *data = NULL;
        int data_size = 0;

        avctx->profile = ff_h264_get_profile(sps);
        avctx->level = sps->level_idc;

        if ((ret = h2645_ps_to_nalu(sps->data, sps->data_size, &data, &data_size)) < 0) {
            goto done;
        }
        ff_oh_video_decoder_set_h264_extradata(s, data, data_size);
        av_freep(&data);

        if ((ret = h2645_ps_to_nalu(pps->data, pps->data_size, &data, &data_size)) < 0) {
            goto done;
        }
        ff_oh_video_decoder_set_h264_extradata(s, data, data_size);
        av_freep(&data);
    } else {
        const int warn = is_avc && (avctx->codec_tag == MKTAG('a','v','c','1') ||
                                    avctx->codec_tag == MKTAG('a','v','c','2'));
        av_log(avctx, warn ? AV_LOG_WARNING : AV_LOG_DEBUG,
               "Could not extract PPS/SPS from extradata\n");
        ret = 0;
    }

done:
    ff_h264_ps_uninit(&ps);

    return ret;
}
//#endif

//#if CONFIG_HEVC_OH_DECODER
static int hevc_set_extradata(AVCodecContext *avctx, OHVideoDecoderContext *s)
{
    int i;
    int ret;

    HEVCParamSets ps;
    HEVCSEI sei;

    const HEVCVPS *vps = NULL;
    const HEVCPPS *pps = NULL;
    const HEVCSPS *sps = NULL;
    int is_nalff = 0;
    int nal_length_size = 0;

    uint8_t *vps_data = NULL;
    uint8_t *sps_data = NULL;
    uint8_t *pps_data = NULL;
    int vps_data_size = 0;
    int sps_data_size = 0;
    int pps_data_size = 0;

    memset(&ps, 0, sizeof(ps));
    memset(&sei, 0, sizeof(sei));

    ret = ff_hevc_decode_extradata(avctx->extradata, avctx->extradata_size,
                                   &ps, &sei, &is_nalff, &nal_length_size, 0, 1, avctx);
    if (ret < 0) {
        goto done;
    }

    for (i = 0; i < HEVC_MAX_VPS_COUNT; i++) {
        if (ps.vps_list[i]) {
            vps = ps.vps_list[i];
            break;
        }
    }

    for (i = 0; i < HEVC_MAX_PPS_COUNT; i++) {
        if (ps.pps_list[i]) {
            pps = ps.pps_list[i];
            break;
        }
    }

    if (pps) {
        if (ps.sps_list[pps->sps_id]) {
            sps = ps.sps_list[pps->sps_id];
        }
    }

    if (vps && pps && sps) {
        uint8_t *data;
        int data_size;

        avctx->profile = sps->ptl.general_ptl.profile_idc;
        avctx->level   = sps->ptl.general_ptl.level_idc;

        if ((ret = h2645_ps_to_nalu(vps->data, vps->data_size, &vps_data, &vps_data_size)) < 0 ||
            (ret = h2645_ps_to_nalu(sps->data, sps->data_size, &sps_data, &sps_data_size)) < 0 ||
            (ret = h2645_ps_to_nalu(pps->data, pps->data_size, &pps_data, &pps_data_size)) < 0) {
            goto done;
        }

        data_size = vps_data_size + sps_data_size + pps_data_size;
        data = av_mallocz(data_size);
        if (!data) {
            ret = AVERROR(ENOMEM);
            goto done;
        }

        memcpy(data                                , vps_data, vps_data_size);
        memcpy(data + vps_data_size                , sps_data, sps_data_size);
        memcpy(data + vps_data_size + sps_data_size, pps_data, pps_data_size);

        ff_oh_video_decoder_set_hevc_extradata(avctx, data, data_size);
        av_freep(&data);
    } else {
        const int warn = is_nalff && avctx->codec_tag == MKTAG('h','v','c','1');
        av_log(avctx, warn ? AV_LOG_WARNING : AV_LOG_DEBUG,
               "Could not extract VPS/PPS/SPS from extradata\n");
        ret = 0;
    }

done:
    ff_hevc_ps_uninit(&ps);

    av_freep(&vps_data);
    av_freep(&sps_data);
    av_freep(&pps_data);

    return ret;
}
//#endif


static av_cold int oh_video_decoder_init(AVCodecContext *avctx)
{
    int ret;

    const char *codec_mime = NULL;

    OH_AVFormat *format = NULL;
    OHVideoDecoderH264DecContext *s = avctx->priv_data;

    switch (avctx->codec_id) {
        case AV_CODEC_ID_H264:
        codec_mime = OH_AVCODEC_MIMETYPE_VIDEO_AVC;

        break;
        case AV_CODEC_ID_HEVC:
            codec_mime = OH_AVCODEC_MIMETYPE_VIDEO_HEVC;
            break;
        default:
            av_assert0(0);
    }

    format = OH_AVFormat_Create();
    if (!format) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create oh format\n");
        ret = AVERROR_EXTERNAL;
        goto done;
    }

    OH_AVFormat_SetIntValue(format, OH_MD_KEY_WIDTH, avctx->width);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_HEIGHT, avctx->height);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);



    s->ctx = av_mallocz(sizeof(*s->ctx));
    if (!s->ctx) {
        av_log(avctx, AV_LOG_ERROR, "Failed to allocate MediaCodecDecContext\n");
        ret = AVERROR(ENOMEM);
        goto done;
    }

    s->ctx->delay_flush = s->delay_flush;

    if ((ret = ff_oh_video_decoder_init(avctx, s->ctx, codec_mime, format)) < 0) {
        s->ctx = NULL;
        goto done;
    }


    switch (avctx->codec_id) {

//#if CONFIG_H264_OH_DECODER
        case AV_CODEC_ID_H264:
        codec_mime = OH_AVCODEC_MIMETYPE_VIDEO_AVC;

        ret = h264_set_extradata(avctx, s->ctx);
        if (ret < 0)
            goto done;
        break;
//#endif
//#if CONFIG_HEVC_OH_DECODER
        case AV_CODEC_ID_HEVC:
        codec_mime = OH_AVCODEC_MIMETYPE_VIDEO_HEVC;

        ret = hevc_set_extradata(avctx, s->ctx);
        if (ret < 0)
            goto done;
        break;
//#endif
        default:
            av_assert0(0);
    }


    av_log(avctx, AV_LOG_INFO,
           "OH video decoder started successfully: codec = %s, ret = %d\n",
           s->ctx->codec_name, ret);

    done:
    if (format) {
        OH_AVFormat_Destroy(format);
    }

    if (ret < 0) {
        oh_video_decoder_close(avctx);
    }

    return ret;
}

static int oh_video_decoder_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    OHVideoDecoderH264DecContext *s = avctx->priv_data;
    int ret;
    ssize_t index;

    /* In delay_flush mode, wait until the user has released or rendered
       all retained frames. */
    if (s->delay_flush && ff_oh_video_decoder_is_flushing(avctx, s->ctx)) {
        if (!ff_oh_video_decoder_flush(avctx, s->ctx)) {
            return AVERROR(EAGAIN);
        }
    }

    /* poll for new frame */
    ret = ff_oh_video_decoder_receive(avctx, s->ctx, frame);
    if (ret != AVERROR(EAGAIN))
        return ret;

    /* feed decoder */
    while (1) {


        /* try to flush any buffered packet data */
        if (s->buffered_pkt.size > 0) {
            ret = ff_oh_video_decoder_send(avctx, s->ctx, &s->buffered_pkt);
            if (ret >= 0) {
                s->buffered_pkt.size -= ret;
                s->buffered_pkt.data += ret;
                if (s->buffered_pkt.size <= 0) {
                    av_packet_unref(&s->buffered_pkt);
                } else {
                    av_log(avctx, AV_LOG_WARNING,
                           "could not send entire packet in single input buffer (%d < %d)\n",
                           ret, s->buffered_pkt.size+ret);
                }
            } else if (ret < 0 && ret != AVERROR(EAGAIN)) {
                return ret;
            }

//            if (s->amlogic_mpeg2_api23_workaround && s->buffered_pkt.size <= 0) {
//                /* fallthrough to fetch next packet regardless of input buffer space */
//            } else {
                /* poll for space again */
                continue;
//            }
        }

        /* fetch new packet or eof */
        ret = ff_decode_get_packet(avctx, &s->buffered_pkt);
        if (ret == AVERROR_EOF) {
            AVPacket null_pkt = { 0 };
            ret = ff_oh_video_decoder_send(avctx, s->ctx, &null_pkt);
            if (ret < 0)
                return ret;
            return ff_oh_video_decoder_receive(avctx, s->ctx, frame);
        } else if (ret == AVERROR(EAGAIN)) {
            return ff_oh_video_decoder_receive(avctx, s->ctx, frame);
        } else if (ret < 0) {
            return ret;
        }
    }

    return AVERROR(EAGAIN);
}

static void oh_video_decoder_flush(AVCodecContext *avctx)
{
    OHVideoDecoderH264DecContext *s = avctx->priv_data;

    av_packet_unref(&s->buffered_pkt);

    ff_oh_video_decoder_flush(avctx, s->ctx);
}

static const AVCodecHWConfigInternal *const oh_video_decoder_hw_configs[] = {
        &(const AVCodecHWConfigInternal) {
                .public          = {
                        .pix_fmt     = AV_PIX_FMT_OH,
                        .methods     = AV_CODEC_HW_CONFIG_METHOD_AD_HOC |
                                       AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX,
                        .device_type = AV_HWDEVICE_TYPE_OH,
                },
                .hwaccel         = NULL,
        },
        NULL
};

#define OFFSET(x) offsetof(OHVideoDecoderH264DecContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption ff_oh_video_decoder_options[] = {
        { "delay_flush", "Delay flush until hw output buffers are returned to the decoder",
          OFFSET(delay_flush), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, VD },
        { NULL }
};

#define DECLARE_OH_VCLASS(short_name)                   \
static const AVClass ff_##short_name##_oh_decoder_class = { \
    .class_name = #short_name "_oh",              \
    .item_name  = av_default_item_name,                         \
    .option     = ff_oh_video_decoder_options,                  \
    .version    = LIBAVUTIL_VERSION_INT,                        \
};

#define DECLARE_OH_VDEC(short_name, full_name, codec_id, bsf)                          \
DECLARE_OH_VCLASS(short_name)                                                          \
const FFCodec ff_ ## short_name ## _oh_decoder = {                                     \
    .p.name         = #short_name "_oh",                                               \
    CODEC_LONG_NAME(full_name " Harmony OH Decoder"),                                  \
    .p.type         = AVMEDIA_TYPE_VIDEO,                                                      \
    .p.id           = codec_id,                                                                \
    .p.priv_class   = &ff_##short_name##_oh_decoder_class,                                 \
    .priv_data_size = sizeof(OHVideoDecoderH264DecContext),                                        \
    .init           = oh_video_decoder_init,                                                  \
    FF_CODEC_RECEIVE_FRAME_CB(oh_video_decoder_receive_frame),                                       \
    .flush          = oh_video_decoder_flush,                                                 \
    .close          = oh_video_decoder_close,                                                 \
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_HARDWARE, \
    .caps_internal  = FF_CODEC_CAP_NOT_INIT_THREADSAFE,                                        \
    .bsfs           = bsf,                                                                     \
    .hw_configs     = oh_video_decoder_hw_configs,                                                   \
    .p.wrapper_name = "oh",                                                            \
};                                                                                             \

//#if CONFIG_H264_OH_DECODER
DECLARE_OH_VDEC(h264, "H.264", AV_CODEC_ID_H264, "h264_mp4toannexb")
//#endif

//#if CONFIG_HEVC_OH_DECODER
DECLARE_OH_VDEC(hevc, "H.265", AV_CODEC_ID_HEVC, "hevc_mp4toannexb")
//#endif
