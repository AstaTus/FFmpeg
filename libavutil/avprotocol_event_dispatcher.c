
#include "libavutil/avprotocol_event_dispatcher.h"
#include "libavformat/network.h"
#include "libavutil/avstring.h"

int  av_protocol_event_context_alloc(AVProtocolEventDispatcherContext **ph, void *opaque)
{
    AVProtocolEventDispatcherContext * h = NULL;
    h = av_mallocz(sizeof(AVProtocolEventDispatcherContext));
    if (!h)
        return AVERROR(ENOMEM);

    h->opaque = opaque;

    *ph = h;
    return 0;
}

int  av_protocol_event_context_open(AVProtocolEventDispatcherContext **ph, void *opaque)
{
    int ret = av_protocol_event_context_alloc(ph, opaque);
    if (ret)
        return ret;

    return 0;
}

void av_protocol_event_context_close(AVProtocolEventDispatcherContext *h)
{
    av_free(h);
}


void av_protocol_event_context_closep(AVProtocolEventDispatcherContext **ph)
{
    if (!ph || !*ph)
        return;

    av_protocol_event_context_close(*ph);
    *ph = NULL;
}

void av_protocol_event_start_open(AVProtocolEventDispatcherContext *h, const char * protocol_name, void * event)
{
    if (h && h->on_protocol_event)
        h->on_protocol_event(h, AVPROTOCOL_EVENT_START_OPEN, protocol_name, event);
}

void av_protocol_event_end_open(AVProtocolEventDispatcherContext *h, const char * protocol_name, void * event)
{
    if (h && h->on_protocol_event)
        h->on_protocol_event(h, AVPROTOCOL_EVENT_END_OPEN, protocol_name, event);
}

void av_protocol_event_start_seek(AVProtocolEventDispatcherContext *h, const char * protocol_name, void * event)
{
    if (h && h->on_protocol_event)
        h->on_protocol_event(h, AVPROTOCOL_EVENT_START_SEEK, protocol_name, event);
}

void av_protocol_event_end_seek(AVProtocolEventDispatcherContext *h, const char * protocol_name, void * event)
{
    if (h && h->on_protocol_event)
        h->on_protocol_event(h, AVPROTOCOL_EVENT_END_SEEK, protocol_name, event);
}

void av_protocol_io_traffic_event_reading(AVProtocolEventDispatcherContext *h, const char * protocol_name, void * event)
{
    if (h && h->on_protocol_event)
        h->on_protocol_event(h, AVPROTOCOL_EVENT_READING, protocol_name, event);
}

void av_protocol_io_traffic_event_writing(AVProtocolEventDispatcherContext *h, const char * protocol_name, void * event)
{
    if (h && h->on_protocol_event)
        h->on_protocol_event(h, AVPROTOCOL_EVENT_WRITING, protocol_name, event);
}

void av_protocol_event_start_reconnect(AVProtocolEventDispatcherContext *h, const char * protocol_name, void * event)
{
    if (h && h->on_protocol_event)
        h->on_protocol_event(h, AVPROTOCOL_EVENT_START_RECONNECT, protocol_name, event);
}
void av_protocol_event_end_reconnect(AVProtocolEventDispatcherContext *h, const char * protocol_name, void * event)
{
    if (h && h->on_protocol_event)
        h->on_protocol_event(h, AVPROTOCOL_EVENT_END_RECONNECT, protocol_name, event);
}
