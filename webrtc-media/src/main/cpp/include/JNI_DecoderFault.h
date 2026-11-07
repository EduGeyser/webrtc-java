/*
 * Copyright 2026 Alex Andres
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <jni.h>
/* Header for class io_github_sendablemetatype_webrtc_media_player_DecoderFault */

#ifndef _Included_io_github_sendablemetatype_webrtc_media_player_DecoderFault
#define _Included_io_github_sendablemetatype_webrtc_media_player_DecoderFault
#ifdef __cplusplus
extern "C" {
#endif
	/*
	 * Class:     io_github_sendablemetatype_webrtc_media_player_DecoderFault
	 * Method:    arm
	 * Signature: (ZI)V
	 */
	JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_media_player_DecoderFault_arm
	(JNIEnv *, jclass, jboolean, jint);

	/*
	 * Class:     io_github_sendablemetatype_webrtc_media_player_DecoderFault
	 * Method:    disarm
	 * Signature: ()V
	 */
	JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_media_player_DecoderFault_disarm
	(JNIEnv *, jclass);

	/*
	 * Class:     io_github_sendablemetatype_webrtc_media_player_DecoderFault
	 * Method:    retries
	 * Signature: ()I
	 */
	JNIEXPORT jint JNICALL Java_io_github_sendablemetatype_webrtc_media_player_DecoderFault_retries
	(JNIEnv *, jclass);

	/*
	 * Class:     io_github_sendablemetatype_webrtc_media_player_DecoderFault
	 * Method:    observerLifecycle
	 * Signature: (Ljava/lang/String;J)Z
	 */
	JNIEXPORT jboolean JNICALL Java_io_github_sendablemetatype_webrtc_media_player_DecoderFault_observerLifecycle
	(JNIEnv *, jclass, jstring, jlong);

#ifdef __cplusplus
}
#endif
#endif
