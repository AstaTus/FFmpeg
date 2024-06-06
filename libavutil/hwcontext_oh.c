/*
 * Harmony OH Video Decoder
 *
 * Copyright (c) 2015-2024 AstaTus <523182099 qq.com>
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

#include <dlfcn.h>

#include "buffer.h"
#include "common.h"
#include "hwcontext.h"
#include "hwcontext_internal.h"
#include "hwcontext_oh.h"

typedef struct OHDeviceContext {
    AVOHDeviceContext ctx;
} OHDeviceContext;


static int oh_device_create(AVHWDeviceContext *ctx, const char *device,
                            AVDictionary *opts, int flags)
{
    const AVDictionaryEntry *entry = NULL;
    OHDeviceContext *s = ctx->hwctx;
    AVOHDeviceContext *dev = &s->ctx;

    if (device && device[0]) {
        av_log(ctx, AV_LOG_ERROR, "Device selection unsupported.\n");
        return AVERROR_UNKNOWN;
    }

    while ((entry = av_dict_iterate(opts, entry))) {
        if (!strcmp(entry->key, "native_window"))
            sscanf(entry->value, "%p", &dev->native_window);
    }

    return 0;
}

static int oh_device_init(AVHWDeviceContext *ctx)
{

    return 0;
}

static void oh_device_uninit(AVHWDeviceContext *ctx)
{
}

const HWContextType ff_hwcontext_type_oh = {
        .type                 = AV_HWDEVICE_TYPE_OH,
        .name                 = "oh",

        .device_hwctx_size    = sizeof(OHDeviceContext),

        .device_create        = oh_device_create,
        .device_init          = oh_device_init,
        .device_uninit        = oh_device_uninit,

        .pix_fmts = (const enum AVPixelFormat[]){
                AV_PIX_FMT_OH,
                AV_PIX_FMT_NONE
        },
};
