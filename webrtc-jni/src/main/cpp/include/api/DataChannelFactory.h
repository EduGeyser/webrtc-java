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

#ifndef JNI_WEBRTC_API_DATA_CHANNEL_FACTORY_H_
#define JNI_WEBRTC_API_DATA_CHANNEL_FACTORY_H_

#include <jni.h>

namespace jni
{
	// The factory of a Java RTCDataChannel's peer connection. Releasing the
	// native channel needs the factory's threads, and the release can run
	// after dispose() returned, so dispose() keeps the factory in use until
	// then.
	namespace DataChannelFactory
	{
		// Gives a new Java data channel its factory, which may be null.
		void Set(JNIEnv * env, jobject channel, jobject factory);

		// Marks the channel's factory in use, as NativeHandle::Use() does,
		// and returns its handle for NativeHandle::Unuse(). Returns 0 when
		// the channel has no factory or the factory is disposed.
		jlong Use(JNIEnv * env, jobject channel);
	}
}

#endif