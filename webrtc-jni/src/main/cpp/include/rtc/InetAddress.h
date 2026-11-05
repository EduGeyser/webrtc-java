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

#ifndef JNI_WEBRTC_RTC_INET_ADDRESS_H_
#define JNI_WEBRTC_RTC_INET_ADDRESS_H_

#include "JavaClass.h"
#include "JavaRef.h"

#include "rtc_base/ip_address.h"

#include <jni.h>

namespace jni
{
	namespace InetAddress
	{
		class JavaInetAddressClass : public JavaClass
		{
			public:
				explicit JavaInetAddressClass(JNIEnv * env);

				jclass cls;
				jmethodID getAddress;
		};

		// The address of a java.net.InetAddress, from its raw bytes. An IPv6 scope is
		// left out. Throws a NullPointerException for null.
		webrtc::IPAddress toNative(JNIEnv * env, const JavaRef<jobject> & address);
	}
}

#endif
