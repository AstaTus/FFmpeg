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

#include "oh_video_decoder_internal_buffer.h"
#include <libavutil/mem.h>
#include <unistd.h>
#include <libavutil/log.h>
OHVideoDecoderInternalBuffer * oh_video_decoder_internal_buffer_create(uint32_t index, OH_AVBuffer * buffer) {
    if (buffer == NULL) {
        return NULL;
    }
    OHVideoDecoderInternalBuffer *decoder_buffer = av_malloc(sizeof(*decoder_buffer));
    if (decoder_buffer == NULL)
        return NULL;
    decoder_buffer->buffer_index = index;
    decoder_buffer->buffer = buffer;
    return decoder_buffer;
}

void oh_video_decoder_internal_buffer_destroy(OHVideoDecoderInternalBuffer * buffer) {
    if(buffer == NULL)
        return;

//    av_freep(&(buffer->buffer));
    av_freep(&buffer);
}


static inline OHVideoDecoderInternalBufferQueueNode * queue_node_create(void) {
    OHVideoDecoderInternalBufferQueueNode *node = av_malloc(sizeof(*node));
    if(node != NULL) {
        node->next_node = NULL;
        node->prev_node = NULL;
        node->buffer = NULL;
    }
    return node;
}

static inline void queue_node_destroy(OHVideoDecoderInternalBufferQueueNode * node, int is_free_buffer) {
    if(node == NULL) {
        return;
    }

    if (is_free_buffer) {
        oh_video_decoder_internal_buffer_destroy(node->buffer);
    }
    av_freep(&node);
}

static inline OHVideoDecoderInternalBufferQueue * queue_create(void) {
    OHVideoDecoderInternalBufferQueue *queue = av_malloc(sizeof(*queue));
    if(queue != NULL) {
        queue->head_node = NULL;
        queue->tail_node = NULL;
        queue->length = 0;
    }
    return queue;
}

static inline void queue_clear(OHVideoDecoderInternalBufferQueue * queue) {

    if(queue == NULL) {
        return;
    }
    OHVideoDecoderInternalBufferQueueNode *node = queue->head_node;
    OHVideoDecoderInternalBufferQueueNode * temp_node = NULL;
    while (node != NULL) {
        temp_node = node;
        node = node->next_node;
        queue_node_destroy(temp_node, 1);
    }

    queue->head_node = NULL;
    queue->tail_node = NULL;
    queue->length = 0;

}

static inline void queue_destroy(OHVideoDecoderInternalBufferQueue * queue) {

    if(queue == NULL) {
        return;
    }
    queue_clear(queue);

    av_freep(&queue);
}


OHVideoDecoderInternalBuffersGroup * oh_video_decoder_internal_buffer_group_create(void) {

    OHVideoDecoderInternalBuffersGroup *group = av_malloc(sizeof(*group));
    if (group == NULL)
        return NULL;
    
    group->input_queue = queue_create();
    pthread_mutex_init(&group->input_queue_mutex, NULL);
    pthread_cond_init(&group->input_queue_cond, NULL);
    group->output_queue = queue_create();
    pthread_mutex_init(&group->output_queue_mutex, NULL);
    pthread_cond_init(&group->output_queue_cond, NULL);

    return group;
}

void oh_video_decoder_internal_buffer_group_destroy(OHVideoDecoderInternalBuffersGroup * group) {

    if(group == NULL)
        return;

    pthread_mutex_lock(&group->input_queue_mutex);
    queue_destroy(group->input_queue);
    pthread_mutex_unlock(&group->input_queue_mutex);

    pthread_mutex_lock(&group->output_queue_mutex);
    queue_destroy(group->output_queue);
    pthread_mutex_unlock(&group->output_queue_mutex);
    
    pthread_mutex_destroy(&group->input_queue_mutex);
    pthread_cond_destroy(&group->input_queue_cond);

    pthread_mutex_destroy(&group->output_queue_mutex);
    pthread_cond_destroy(&group->output_queue_cond);


    av_freep(&group);
}

void oh_video_decoder_internal_buffer_group_clear(OHVideoDecoderInternalBuffersGroup * group) {
    if(group == NULL)
        return;

    pthread_mutex_lock(&group->input_queue_mutex);
    queue_clear(group->input_queue);
    pthread_mutex_unlock(&group->input_queue_mutex);

    pthread_mutex_lock(&group->output_queue_mutex);
    queue_clear(group->output_queue);
    pthread_mutex_unlock(&group->output_queue_mutex);

}
static inline size_t queue_size(OHVideoDecoderInternalBufferQueue * queue) {
    if(queue == NULL) {
        return 0;
    }

    return queue->length;
}


static inline size_t queue_push_buffer(OHVideoDecoderInternalBufferQueue * queue, OHVideoDecoderInternalBuffer* buffer) {

    if (buffer == NULL || queue == NULL) {
        return 0;
    }
    OHVideoDecoderInternalBufferQueueNode * node = queue_node_create();
    node->buffer = buffer;

    if(queue->head_node == NULL) {
        queue->head_node = queue->tail_node = node;
        queue->head_node->prev_node = NULL;
        queue->head_node->next_node = NULL;

    } else {
        queue->tail_node->next_node = node;
        node->prev_node = queue->tail_node;
        queue->tail_node = node;
        queue->tail_node->next_node = NULL;
    }
    queue->length += 1;
    size_t ret = queue->length;

    return ret;
}

static inline OHVideoDecoderInternalBuffer* queue_pop_buffer(OHVideoDecoderInternalBufferQueue * queue) {

    if(queue == NULL) {
        return NULL;
    }

    OHVideoDecoderInternalBuffer* ret = NULL;
    if(queue->head_node != NULL) {
        ret = queue->head_node->buffer;
        OHVideoDecoderInternalBufferQueueNode * remove_node = queue->head_node;
        if (queue->head_node == queue->tail_node) {
            queue->head_node = queue->tail_node = NULL;
        } else {
            queue->head_node = queue->head_node->next_node;
            queue->head_node->prev_node = NULL;
        }
        queue_node_destroy(remove_node, 0);
        queue->length -= 1;
    }


    return ret;
}


size_t oh_video_decoder_internal_buffer_group_push_input_buffer(OHVideoDecoderInternalBuffersGroup * group, OHVideoDecoderInternalBuffer* buffer) {

    if (group == NULL || buffer == NULL) {
        return 0;
    }
    pthread_mutex_lock(&group->input_queue_mutex);

    ssize_t ret = queue_push_buffer(group->input_queue, buffer);
    av_log(NULL, AV_LOG_DEBUG,
           "harmony-decoder:push_input_buffer index=%d\n",buffer->buffer_index);
    pthread_cond_signal(&group->input_queue_cond);

    pthread_mutex_unlock(&group->input_queue_mutex);

    return ret;
}


static inline OHVideoDecoderInternalBuffer* oh_video_decoder_internal_buffer_group_pop_buffer_impl(OHVideoDecoderInternalBufferQueue * queue, pthread_mutex_t * queue_mutex, pthread_cond_t * queue_cond, uint64_t millisecond) {

    pthread_mutex_lock(queue_mutex);

    size_t size = queue_size(queue);
    if (millisecond == 0) {
        while (size == 0) {
            pthread_cond_wait(queue_cond, queue_mutex);
            size = queue_size(queue);
        }
    } else {

        if (size == 0) {
            struct timespec t;
            t.tv_sec = millisecond / 1000;
            t.tv_nsec = (millisecond % 1000) * 1000000;
            pthread_cond_timedwait(queue_cond, queue_mutex, &t);
            size = queue_size(queue);
            if (size == 0) {
                pthread_mutex_unlock(queue_mutex);
                return NULL;
            }
        }
    }

    OHVideoDecoderInternalBuffer *internal_buffer = queue_pop_buffer(queue);

    pthread_mutex_unlock(queue_mutex);

    return internal_buffer;
}

OHVideoDecoderInternalBuffer* oh_video_decoder_internal_buffer_group_pop_input_buffer(OHVideoDecoderInternalBuffersGroup * group, uint64_t millisecond) {
    if (group == NULL) {
        return NULL;
    }
    return oh_video_decoder_internal_buffer_group_pop_buffer_impl(group->input_queue, &group->input_queue_mutex, &group->input_queue_cond, millisecond);
}

size_t oh_video_decoder_internal_buffer_group_push_output_buffer(OHVideoDecoderInternalBuffersGroup * group, OHVideoDecoderInternalBuffer* buffer) {
    if (group == NULL || buffer == NULL) {
        return 0;
    }

    pthread_mutex_lock(&group->output_queue_mutex);

    size_t ret = queue_push_buffer(group->output_queue, buffer);
    av_log(NULL, AV_LOG_DEBUG,
           "harmony-decoder:push_output_buffer index=%d\n",buffer->buffer_index);
    pthread_cond_signal(&group->output_queue_cond);

    pthread_mutex_unlock(&group->output_queue_mutex);

    return ret;
}

OHVideoDecoderInternalBuffer* oh_video_decoder_internal_buffer_group_pop_output_buffer(OHVideoDecoderInternalBuffersGroup * group, uint64_t millisecond) {
    if (group == NULL) {
        return NULL;
    }

    if (group == NULL) {
        return NULL;
    }
    return oh_video_decoder_internal_buffer_group_pop_buffer_impl(group->output_queue, &group->output_queue_mutex,
                                                                  &group->output_queue_cond, millisecond);
}
