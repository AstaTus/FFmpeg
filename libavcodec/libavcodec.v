LIBAVCODEC_MAJOR {
    global:
        av_*;
		ff_jni_get_env;
        avcodec_*;
        avpriv_*;
        avsubtitle_free;
    	av_videotoolbox_get_context;
    local:
        *;
};
