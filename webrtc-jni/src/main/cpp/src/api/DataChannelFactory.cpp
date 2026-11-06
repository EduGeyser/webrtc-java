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

#include "api/DataChannelFactory.h"
#include "JavaRef.h"
#include "JavaUtils.h"
#include "JNI_WebRTC.h"
#include "NativeHandle.h"

namespace jni
{
	namespace
	{
		const char * const kFactoryField = "factory";
		const char * const kFactorySignature = "L" PKG "PeerConnectionFactory;";
	}

	namespace DataChannelFactory
	{
		void Set(JNIEnv * env, jobject channel, jobject factory)
		{
			env->SetObjectField(channel, GetFieldID(env, channel, kFactoryField, kFactorySignature), factory);
		}

		jlong Use(JNIEnv * env, jobject channel)
		{
			JavaLocalRef<jobject> factory(env, env->GetObjectField(channel,
				GetFieldID(env, channel, kFactoryField, kFactorySignature)));

			return NativeHandle::Use(env, factory.get());
		}
	}
}