//
// Created by 老干部 on 2024/7/9.
//

#ifndef AVCODEC_HARDWARE_DECODE_SEI_CONTEXT_H
#define AVCODEC_HARDWARE_DECODE_SEI_CONTEXT_H

#include "h264_sei.h"
#include "mediacodec.h"
#include "h264_ps.h"
#include "hevc_sei.h"
#include "hevc_ps.h"

#include "h2645_parse.h"
#include "avcodec.h"

typedef struct HardwareDecodeSEIContext {
    /**
     * Used to parse AVC variant of H.264
     */
    int is_avc;           ///< this flag is != 0 if codec is avc1
    int nal_length_size;  ///< Number of bytes used for nal length (1, 2 or 4)

    H264SEIContext h264_sei;
    H2645Packet h264_pkt;
    H264ParamSets h264_ps;

    /**
 * Used to parse EVC variant of H.265
 */
    HEVCSEI hevc_sei;
    H2645Packet hevc_pkt;
    HEVCParamSets hevc_ps;
    int apply_defdispwin;
    int is_nalff;           ///< this flag is != 0 if bitstream is encapsulated
    ///< as a format defined in 14496-15


} HardwareDecodeSEIContext;

int copy_SEI_data_from_hardware_decode_SEI_context(void * context, AVFrame* frame);

int parse_hevc_sei_data(void * context, AVCodecContext *avctx, AVPacket *pkt);

int parse_h264_sei_data(void * context, AVCodecContext *avctx, AVPacket *pkt);

#endif //AVCODEC_HARDWARE_SEI_CONTEXT_H
