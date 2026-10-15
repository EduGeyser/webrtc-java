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

#ifndef WEBRTC_JAVA_MEDIA_JAVA_EXCEPTION_H_
#define WEBRTC_JAVA_MEDIA_JAVA_EXCEPTION_H_

#include <jni.h>

namespace ffmpeg
{
	// Clears the Java exception pending on the calling thread, if any, and
	// reports it to the thread's uncaught exception handler, as the callbacks
	// of webrtc-java's native library do. This module does not link against
	// that library, so it has its own copy.
	void ReportPendingException(JNIEnv * env) noexcept;
}

#endif
