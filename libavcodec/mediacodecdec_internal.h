//
// Created by 老干部 on 2022/5/17.
// Email：chenjunqi@qiniu.com
// Phone: 18701855224
//

#ifndef AVCODEC_MEDIACODECDEC_INTERNAL_H
#define AVCODEC_MEDIACODECDEC_INTERNAL_H

#include "h264_sei.h"
#include "h2645_parse.h"
#include "mediacodec.h"
#include "mediacodecdec_common.h"
#include "h264_ps.h"
#include "hevc_sei.h"
#include "hevc_ps.h"

typedef struct MediaCodecH264DecContext {

    AVClass *avclass;

    MediaCodecDecContext *ctx;

    AVPacket buffered_pkt;

    /**
     * Used to parse AVC variant of H.264
     */
    int is_avc;           ///< this flag is != 0 if codec is avc1
    int nal_length_size;  ///< Number of bytes used for nal length (1, 2 or 4)

    H264SEIContext h264_sei;
    H2645Packet h264_pkt;
    H264ParamSets h264_ps;



    int delay_flush;
    int amlogic_mpeg2_api23_workaround;


    /**
     * Used to parse EVC variant of H.265
     */
    HEVCSEI hevc_sei;
    H2645Packet hevc_pkt;
    HEVCParamSets hevc_ps;
    int apply_defdispwin;
    int is_nalff;           ///< this flag is != 0 if bitstream is encapsulated
    ///< as a format defined in 14496-15

} MediaCodecH264DecContext;

//typedef struct MediaCodecHEVCDecContext {
//
//    AVClass *avclass;
//
//    MediaCodecDecContext *ctx;
//
//    AVPacket buffered_pkt;
//
//    /**
//     * Used to parse AVC variant of H.264
//     */
//    int is_avc;           ///< this flag is != 0 if codec is avc1
//    int nal_length_size;  ///< Number of bytes used for nal length (1, 2 or 4)
//
//
//
//    int delay_flush;
//    int amlogic_mpeg2_api23_workaround;
//
//} MediaCodecHEVCDecContext;

#endif //AVCODEC_MEDIACODECDEC_INTERNAL_H
