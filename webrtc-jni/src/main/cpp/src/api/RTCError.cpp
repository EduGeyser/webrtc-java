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

#include "api/RTCError.h"
#include "JavaClasses.h"
#include "JavaEnums.h"
#include "JavaPrimitive.h"
#include "JavaString.h"
#include "JavaUtils.h"
#include "JNI_WebRTC.h"

namespace jni
{
	namespace RTCError
	{
		JavaLocalRef<jobject> toJava(JNIEnv * env, const webrtc::RTCError & error)
		{
			if (error.ok()) {
				return nullptr;
			}

			const auto javaClass = JavaClasses::get<JavaRTCErrorClass>(env);

			std::optional<uint16_t> causeCode = error.sctp_cause_code();

			jobject jError = env->NewObject(javaClass->cls, javaClass->ctor,
				JavaEnums::toJava(env, error.type()).get(),
				JavaEnums::toJava(env, error.error_detail()).get(),
				causeCode ? Integer::create(env, *causeCode).get() : nullptr,
				JavaString::toJava(env, error.message()).get());

			return JavaLocalRef<jobject>(env, jError);
		}

		JavaRTCErrorClass::JavaRTCErrorClass(JNIEnv * env)
		{
			cls = FindClass(env, PKG"RTCError");

			ctor = GetMethod(env, cls, "<init>", "(L" PKG "RTCErrorType;L" PKG "RTCErrorDetailType;" INTEGER_SIG STRING_SIG ")V");
		}
	}
}
