/* JNI declarations for io.github.sendablemetatype.webrtc.media.audio.StandaloneVoiceActivityDetector. */
#ifndef JNI_STANDALONE_VOICE_ACTIVITY_DETECTOR_H_
#define JNI_STANDALONE_VOICE_ACTIVITY_DETECTOR_H_

#include <jni.h>

#ifdef __cplusplus
extern "C" {
#endif
JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_media_audio_StandaloneVoiceActivityDetector_initialize(JNIEnv*, jobject, jint);
JNIEXPORT jboolean JNICALL Java_io_github_sendablemetatype_webrtc_media_audio_StandaloneVoiceActivityDetector_processInternal(JNIEnv*, jobject, jshortArray, jint);
JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_media_audio_StandaloneVoiceActivityDetector_setModeInternal(JNIEnv*, jobject, jint);
JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_media_audio_StandaloneVoiceActivityDetector_resetInternal(JNIEnv*, jobject, jint);
JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_media_audio_StandaloneVoiceActivityDetector_dispose(JNIEnv*, jobject);
#ifdef __cplusplus
}
#endif
#endif
