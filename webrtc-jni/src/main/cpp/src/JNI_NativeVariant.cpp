#include "JNI_NativeVariant.h"

JNIEXPORT jboolean JNICALL Java_io_github_sendablemetatype_webrtc_internal_NativeVariant_hasMediaNative
(JNIEnv * env, jclass caller)
{
#ifdef WEBRTC_DATA_CHANNELS_ONLY
	return JNI_FALSE;
#else
	return JNI_TRUE;
#endif
}
