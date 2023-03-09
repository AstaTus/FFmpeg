/*
 * Android MediaCodec public API functions
 *
 * Copyright (c) 2016 Matthieu Bouron <matthieu.bouron stupeflix.com>
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

#include "config.h"

#include "libavutil/error.h"

#include "mediacodec.h"

#if CONFIG_MEDIACODEC

#include <jni.h>
#include <libavutil/avassert.h>

#include "libavcodec/avcodec.h"
#include "internal.h"

#include "libavutil/mem.h"
#include "bsf.h"
#include "decode.h"

#include "ffjni.h"
#include "mediacodecdec_common.h"
#include "version.h"
#include "mediacodecdec_internal.h"

AVMediaCodecContext *av_mediacodec_alloc_context(void)
{
    return av_mallocz(sizeof(AVMediaCodecContext));
}

int av_mediacodec_default_init(AVCodecContext *avctx, AVMediaCodecContext *ctx, void *surface)
{
    int ret = 0;
    JNIEnv *env = NULL;

    env = ff_jni_get_env(avctx);
    if (!env) {
        return AVERROR_EXTERNAL;
    }

    ctx->surface = (*env)->NewGlobalRef(env, surface);
    if (ctx->surface) {
        avctx->hwaccel_context = ctx;
    } else {
        av_log(avctx, AV_LOG_ERROR, "Could not create new global reference\n");
        ret = AVERROR_EXTERNAL;
    }

    return ret;
}

void av_mediacodec_default_free(AVCodecContext *avctx)
{
    JNIEnv *env = NULL;

    AVMediaCodecContext *ctx = avctx->hwaccel_context;

    if (!ctx) {
        return;
    }

    env = ff_jni_get_env(avctx);
    if (!env) {
        return;
    }

    if (ctx->surface) {
        (*env)->DeleteGlobalRef(env, ctx->surface);
        ctx->surface = NULL;
    }

    av_freep(&avctx->hwaccel_context);
}

int av_mediacodec_release_buffer(AVMediaCodecBuffer *buffer, int render)
{
    MediaCodecDecContext *ctx = buffer->ctx;
    int released = atomic_fetch_add(&buffer->released, 1);

    if (!released && (ctx->delay_flush || buffer->serial == atomic_load(&ctx->serial))) {
        atomic_fetch_sub(&ctx->hw_buffer_count, 1);
        av_log(ctx->avctx, AV_LOG_DEBUG,
               "Releasing output buffer %zd (%p) ts=%"PRId64" with render=%d [%d pending]\n",
               buffer->index, buffer, buffer->pts, render, atomic_load(&ctx->hw_buffer_count));
        return ff_AMediaCodec_releaseOutputBuffer(ctx->codec, buffer->index, render);
    }

    return 0;
}

int av_mediacodec_render_buffer_at_time(AVMediaCodecBuffer *buffer, int64_t time)
{
    MediaCodecDecContext *ctx = buffer->ctx;
    int released = atomic_fetch_add(&buffer->released, 1);

    if (!released && (ctx->delay_flush || buffer->serial == atomic_load(&ctx->serial))) {
        atomic_fetch_sub(&ctx->hw_buffer_count, 1);
        av_log(ctx->avctx, AV_LOG_DEBUG,
               "Rendering output buffer %zd (%p) ts=%"PRId64" with time=%"PRId64" [%d pending]\n",
               buffer->index, buffer, buffer->pts, time, atomic_load(&ctx->hw_buffer_count));
        return ff_AMediaCodec_releaseOutputBufferAtTime(ctx->codec, buffer->index, time);
    }

    return 0;
}


int av_mediacodec_support_codec(enum AVCodecID codec_id, enum AVPixelFormat pix_fmt, int is_encoder, int profile)
{
    const char *codec_mime = NULL;
    jobject codec_name = NULL;

    switch (codec_id) {
#if CONFIG_H264_MEDIACODEC_DECODER
        case AV_CODEC_ID_H264:
            codec_mime = "video/avc";
            break;
#endif
#if CONFIG_HEVC_MEDIACODEC_DECODER
        case AV_CODEC_ID_HEVC:
            codec_mime = "video/hevc";
            break;
#endif
#if CONFIG_MPEG2_MEDIACODEC_DECODER
            case AV_CODEC_ID_MPEG2VIDEO:
        codec_mime = "video/mpeg2";
        break;
#endif
#if CONFIG_MPEG4_MEDIACODEC_DECODER
            case AV_CODEC_ID_MPEG4:
        codec_mime = "video/mp4v-es",
        break;
#endif
#if CONFIG_VP8_MEDIACODEC_DECODER
            case AV_CODEC_ID_VP8:
        codec_mime = "video/x-vnd.on2.vp8";
        break;
#endif
#if CONFIG_VP9_MEDIACODEC_DECODER
            case AV_CODEC_ID_VP9:
        codec_mime = "video/x-vnd.on2.vp9";
        break;
#endif
        default:
            av_assert0(0);
    }

    if (codec_mime == NULL) {
        return AVERROR(EINVAL);
    }

    codec_name = ff_AMediaCodecList_getCodecNameByType(codec_mime, profile, is_encoder, NULL);

    if (codec_name == NULL) {
        return AVERROR(EINVAL);
    }

    return 0;
}



int av_mediacodec_send_packet(AVCodecContext *avctx, AVPacket *pkt, bool wait)
{
    AVCodecInternal *avci = avctx->internal;
    MediaCodecH264DecContext *s = avctx->priv_data;

    int ret;

    if (!avcodec_is_open(avctx) || !av_codec_is_decoder(avctx->codec))
        return AVERROR(EINVAL);

    if (avctx->internal->draining)
        return AVERROR_EOF;

    if (pkt && !pkt->size && pkt->data)
        return AVERROR(EINVAL);

    av_packet_unref(avci->buffer_pkt);
    if (pkt && (pkt->data || pkt->side_data_elems)) {
        ret = av_packet_ref(avci->buffer_pkt, pkt);
        if (ret < 0)
            return ret;
    }

    ret = av_bsf_send_packet(avci->bsf, avci->buffer_pkt);
    if (ret < 0) {
        av_packet_unref(avci->buffer_pkt);
        return ret;
    }

    //////////////////////////////////////////////////////////////////////////////
    /* fetch new packet or eof */
    ret = ff_decode_get_packet(avctx, &s->buffered_pkt);
    if (ret == AVERROR_EOF) {
        AVPacket null_pkt = { 0 };
        ret = ff_mediacodec_dec_send(avctx, s->ctx, &null_pkt, true);
        if (ret < 0)
            return ret;
        return 0;
    } else if (ret < 0) {
        return ret;
    }

    /* try to flush any buffered packet data */
    if (s->buffered_pkt.size > 0) {
        ret = ff_mediacodec_dec_send(avctx, s->ctx, &s->buffered_pkt, false);
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
            return 0;
        } else {
            return ret;
        }

//        if (s->amlogic_mpeg2_api23_workaround && s->buffered_pkt.size <= 0) {
//            /* fallthrough to fetch next packet regardless of input buffer space */
//        } else {
//            /* poll for space again */
//            continue;
//        }
    }

    return AVERROR(EAGAIN);
}

int av_mediacodec_receive_frame(AVCodecContext * avctx, AVFrame * pframe, bool wait)
{
    MediaCodecH264DecContext *s = avctx->priv_data;

    return ff_mediacodec_dec_receive(avctx, s->ctx, pframe, wait);
}

#else

#include <stdlib.h>

AVMediaCodecContext *av_mediacodec_alloc_context(void)
{
    return NULL;
}

int av_mediacodec_default_init(AVCodecContext *avctx, AVMediaCodecContext *ctx, void *surface)
{
    return AVERROR(ENOSYS);
}

void av_mediacodec_default_free(AVCodecContext *avctx)
{
}

int av_mediacodec_release_buffer(AVMediaCodecBuffer *buffer, int render)
{
    return AVERROR(ENOSYS);
}

int av_mediacodec_render_buffer_at_time(AVMediaCodecBuffer *buffer, int64_t time)
{
    return AVERROR(ENOSYS);
}
#if CONFIG_MEDIACODEC
int av_mediacodec_send_packet(AVCodecContext *avctx, AVPacket *pkt, bool wait)
{
    return AVERROR(ENOSYS);
}

int av_mediacodec_receive_frame(AVCodecContext * avctx, AVFrame * pframe, bool wait)
{
    return AVERROR(ENOSYS);
}
#endif

#endif
