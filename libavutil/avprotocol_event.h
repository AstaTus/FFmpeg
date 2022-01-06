/*
 * copyright (c) 2021 老干部 <523182099@qq.com>
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

#ifndef AVUTIL_AVPROTOCOL_EVENT_H
#define AVUTIL_AVPROTOCOL_EVENT_H

#include "libavutil/log.h"


#define AVPROTOCOL_EVENT_START_OPEN         1
#define AVPROTOCOL_EVENT_END_OPEN           2

#define AVPROTOCOL_EVENT_START_SEEK         11
#define AVPROTOCOL_EVENT_END_SEEK           12

#define AVPROTOCOL_EVENT_READING_STATUS     21
#define AVPROTOCOL_EVENT_WRITING_STATUS     22

// list all protocol's event below

typedef struct AVHttpEvent{

} AVHttpEvent;

typedef struct AVTcpEvent{

} AVTcpEvent;


typedef struct AVProtocolEventContext AVProtocolEventContext;
struct AVProtocolEventContext {
    const AVClass *av_class;    /**< information for av_log(). Set by av_application_open(). */
    void *opaque;               /**< user data. */

    int (*on_protocol_event)(AVProtocolEventContext *h, int event_type, const char * protocol_name, void * event);
};

int  av_protocol_event_context_alloc(AVProtocolEventContext **ph, void *opaque);
int  av_protocol_event_context_open(AVProtocolEventContext **ph, void *opaque);
void av_protocol_event_context_close(AVProtocolEventContext *h);
void av_protocol_event_context_closep(AVProtocolEventContext **ph);

void av_protocol_event_start_open(AVProtocolEventContext *h, const char * protocol_name, void * event);
void av_protocol_event_end_open(AVProtocolEventContext *h, const char * protocol_name, void * event);

void av_protocol_event_start_seek(AVProtocolEventContext *h, const char * protocol_name, void * event);
void av_protocol_event_end_seek(AVProtocolEventContext *h, const char * protocol_name, void * event);

void av_protocol_event_reading_status(AVProtocolEventContext *h, const char * protocol_name, void * event);
void av_protocol_event_writing_status(AVProtocolEventContext *h, const char * protocol_name, void * event);

int av_create_http_event(AVHttpEvent ** ppevent);

int av_create_tcp_event(AVTcpEvent ** ppevent);

#endif //AVUTIL_AVPROTOCOL_EVENT_H
