/*
 * Harmony Video Decoders H.264 / H.265
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


#ifndef AVCODEC_OH_VIDEO_DECODER_INTERNAL_BUFFER_H
#define AVCODEC_OH_VIDEO_DECODER_INTERNAL_BUFFER_H

#include <native_buffer/native_buffer.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <pthread.h>
#include <stdint.h>
typedef struct OHVideoDecoderInternalBuffer {

    uint32_t buffer_index;
    OH_AVBuffer * buffer;

} OHVideoDecoderInternalBuffer;

OHVideoDecoderInternalBuffer * oh_video_decoder_internal_buffer_create(uint32_t index, OH_AVBuffer * buffer);

void oh_video_decoder_internal_buffer_destroy(OHVideoDecoderInternalBuffer * buffer);

typedef struct OHVideoDecoderInternalBufferQueueNode {

    OHVideoDecoderInternalBuffer * buffer;
    struct OHVideoDecoderInternalBufferQueueNode * prev_node;
    struct OHVideoDecoderInternalBufferQueueNode * next_node;

} OHVideoDecoderInternalBufferQueueNode;

typedef struct OHVideoDecoderInternalBufferQueue {

    OHVideoDecoderInternalBufferQueueNode * head_node;
    OHVideoDecoderInternalBufferQueueNode * tail_node;
    size_t length;

}OHVideoDecoderInternalBufferQueue;


typedef struct OHVideoDecoderInternalBuffersGroup {
    pthread_mutex_t input_queue_mutex;
    pthread_cond_t input_queue_cond;
    OHVideoDecoderInternalBufferQueue * input_queue;


    pthread_mutex_t output_queue_mutex;
    pthread_cond_t output_queue_cond;
    OHVideoDecoderInternalBufferQueue * output_queue;
} OHVideoDecoderInternalBuffersGroup;

OHVideoDecoderInternalBuffersGroup * oh_video_decoder_internal_buffer_group_create(void);

void oh_video_decoder_internal_buffer_group_destroy(OHVideoDecoderInternalBuffersGroup * group);

void oh_video_decoder_internal_buffer_group_clear(OHVideoDecoderInternalBuffersGroup * group);

size_t oh_video_decoder_internal_buffer_group_push_input_buffer(OHVideoDecoderInternalBuffersGroup * group, OHVideoDecoderInternalBuffer* buffer);

OHVideoDecoderInternalBuffer* oh_video_decoder_internal_buffer_group_pop_input_buffer(OHVideoDecoderInternalBuffersGroup * group, uint64_t millisecond);

size_t oh_video_decoder_internal_buffer_group_push_output_buffer(OHVideoDecoderInternalBuffersGroup * group, OHVideoDecoderInternalBuffer* buffer);

OHVideoDecoderInternalBuffer* oh_video_decoder_internal_buffer_group_pop_output_buffer(OHVideoDecoderInternalBuffersGroup * group, uint64_t millisecond);

#endif //FFMPEG_OH_VIDEO_DECODER_BUFFER_H
