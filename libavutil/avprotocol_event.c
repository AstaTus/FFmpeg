
#include "avprotocol_event.h"
#include "libavformat/network.h"
#include "libavutil/avstring.h"

int  av_protocol_event_context_alloc(AVProtocolEventContext **ph, void *opaque)
{
    AVProtocolEventContext * context = NULL;
    context = av_mallocz(sizeof(AVProtocolEventContext));
}

int  av_protocol_event_context_open(AVProtocolEventContext **ph, void *opaque)
{

}

void av_protocol_event_context_close(AVProtocolEventContext *h)
{

}

void av_protocol_event_context_closep(AVProtocolEventContext **ph)
{

}

void av_protocol_event_start_open(AVProtocolEventContext *h, const char * protocol_name, void * event)
{

}

void av_protocol_event_end_open(AVProtocolEventContext *h, const char * protocol_name, void * event)
{

}

void av_protocol_event_start_seek(AVProtocolEventContext *h, const char * protocol_name, void * event)
{

}

void av_protocol_event_end_seek(AVProtocolEventContext *h, const char * protocol_name, void * event)
{

}

void av_protocol_event_reading_status(AVProtocolEventContext *h, const char * protocol_name, void * event)
{

}

void av_protocol_event_writing_status(AVProtocolEventContext *h, const char * protocol_name, void * event)
{

}

int av_create_http_event(AVHttpEvent ** ppevent)
{

}


int av_create_tcp_event(AVTcpEvent ** ppevent)
{

}