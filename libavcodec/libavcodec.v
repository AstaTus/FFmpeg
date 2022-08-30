LIBAVCODEC_MAJOR {
    global:
        av*;
	ff_jni_get_env;
	av_mediacodec_release_buffer;
	av_videotoolbox_get_context;
    local:
        *;
};
