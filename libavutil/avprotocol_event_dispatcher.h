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

#define AVPROTOCOL_EVENT_READING            21
#define AVPROTOCOL_EVENT_WRITING            22

#define AVPROTOCOL_EVENT_START_RECONNECT    31
#define AVPROTOCOL_EVENT_END_RECONNECT      32

// list all protocol's event below

typedef struct AVHttpEvent{
    void    *obj;
    char     url[4096];
    int64_t  offset;
    int      error;
    int      http_code;
    int64_t  filesize;
} AVHttpEvent;

typedef struct AVTcpEvent{
    int  error;
    int  family;
    char ip[96];
    int  port;
    int  fd;
} AVTcpEvent;

typedef struct AVIOTrafficEvent {
    void   *obj;
    int    bytes;
} AVIOTrafficEvent;

//static const AVClass protocol_event_dispatcher_class = {
//        .class_name = "protocol_event_dispatcher",
//        .item_name  = av_default_item_name,
//        .option     = options,
//        .version    = LIBAVUTIL_VERSION_INT,
//};


typedef struct AVProtocolEventDispatcherContext AVProtocolEventDispatcherContext;
struct AVProtocolEventDispatcherContext {
    const AVClass *av_class;    /**< information for av_log(). Set by av_application_open(). */
    void *opaque;               /**< user data. */

    int (*on_protocol_event)(AVProtocolEventDispatcherContext *h, int event_type, const char * protocol_name, void * event);
};


int  av_protocol_event_context_alloc(AVProtocolEventDispatcherContext **ph, void *opaque);
int  av_protocol_event_context_open(AVProtocolEventDispatcherContext **ph, void *opaque);
void av_protocol_event_context_close(AVProtocolEventDispatcherContext *h);
void av_protocol_event_context_closep(AVProtocolEventDispatcherContext **ph);

void av_protocol_event_start_open(AVProtocolEventDispatcherContext *h, const char * protocol_name, void * event);
void av_protocol_event_end_open(AVProtocolEventDispatcherContext *h, const char * protocol_name, void * event);

void av_protocol_event_start_reconnect(AVProtocolEventDispatcherContext *h, const char * protocol_name, void * event);
void av_protocol_event_end_reconnect(AVProtocolEventDispatcherContext *h, const char * protocol_name, void * event);


void av_protocol_event_start_seek(AVProtocolEventDispatcherContext *h, const char * protocol_name, void * event);
void av_protocol_event_end_seek(AVProtocolEventDispatcherContext *h, const char * protocol_name, void * event);

void av_protocol_io_traffic_event_reading(AVProtocolEventDispatcherContext *h, const char * protocol_name, void * event);
void av_protocol_io_traffic_event_writing(AVProtocolEventDispatcherContext *h, const char * protocol_name, void * event);

#endif //AVUTIL_AVPROTOCOL_EVENT_H
