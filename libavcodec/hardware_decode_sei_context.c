//
// Created by 老干部 on 2024/7/9.
//

#include "hardware_decode_sei_context.h"
#include "libavutil/frame.h"
#include "h264.h"
#include "hevc.h"
#include "hevc_sei.h"
#include "h264_sei.h"
#include "codec_id.h"
#include "h264_parse.h"
#include "hevc_parse.h"
#include "h2645_parse.h"
#include "h264_ps.h"
#include "hevc_sei.h"
#include "hevc_ps.h"
#include "h2645_sei.h"


int copy_SEI_data_from_hardware_decode_SEI_context(void * context, AVFrame* frame) {
    HardwareDecodeSEIContext *h = (HardwareDecodeSEIContext *)context;

    for (int i = 0; i < h->h264_sei.common.unregistered.nb_buf_ref; i++) {
        H2645SEIUnregistered *unreg = &h->h264_sei.common.unregistered;

        if (unreg->buf_ref[i]) {
            AVFrameSideData *sd = av_frame_new_side_data_from_buf(frame,
                                                                  AV_FRAME_DATA_SEI_UNREGISTERED,
                                                                  unreg->buf_ref[i]);
            if (!sd)
                av_buffer_unref(&unreg->buf_ref[i]);
            unreg->buf_ref[i] = NULL;
        }
    }
    h->h264_sei.common.unregistered.nb_buf_ref = 0;
    return 0;

}

static int hevc_decode_extradata(HardwareDecodeSEIContext *context, AVCodecContext *avctx, uint8_t *buf, int length, int first)
{
    int ret, i;

    ret = ff_hevc_decode_extradata(buf, length, &context->hevc_ps, &context->hevc_sei, &context->is_nalff,
                                   &context->nal_length_size, avctx->err_recognition,
                                   context->apply_defdispwin, NULL);
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

int parse_hevc_sei_data(void * context, AVCodecContext *avctx, AVPacket *pkt){

    int ret, i;
    const uint8_t *buf = pkt->data;
    int buf_size       = pkt->size;

    size_t new_extradata_size;
    uint8_t *new_extradata;

    HardwareDecodeSEIContext * s = (HardwareDecodeSEIContext *)context;

    /* end of stream, output what is still in the buffers */
    if (buf_size == 0)
        return 0;

    new_extradata = av_packet_get_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA,
                                            &new_extradata_size);
    if (new_extradata && new_extradata_size > 0) {
        ret = hevc_decode_extradata(s, avctx, new_extradata, new_extradata_size, 0);
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


int parse_h264_sei_data(void * context, AVCodecContext *avctx, AVPacket *pkt) {
    int nals_needed = 0; ///< number of NALs that need decoding before the next frame thread starts
    int i, ret = 0;
    const uint8_t *buf = pkt->data;
    int buf_size       = pkt->size;
    HardwareDecodeSEIContext *h = (HardwareDecodeSEIContext *)context;

    /* end of stream, output what is still in the buffers */
    if (buf_size == 0)
        return 0;

    if (av_packet_get_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, NULL)) {
        size_t side_size;
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


