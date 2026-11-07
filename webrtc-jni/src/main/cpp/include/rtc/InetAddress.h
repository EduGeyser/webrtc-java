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
#include "rtc_base/socket_address.h"

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
				jmethodID getByAddress;
				jclass socketCls;
				jmethodID socketCtor;
				jclass inet6Cls;
				jmethodID getScopeId;
				jmethodID getScopedByAddress;
		};

		// The address of a java.net.InetAddress, from its raw bytes. An IPv6 scope is
		// left out. Throws a NullPointerException for null.
		webrtc::IPAddress toNative(JNIEnv * env, const JavaRef<jobject> & address);

		// The address with the port, keeping the scope of an IPv6 address, which
		// binding a link-local address needs.
		webrtc::SocketAddress toNativeSocketAddress(JNIEnv * env, const JavaRef<jobject> & address, int port);

		// A java.net.InetSocketAddress, built from the raw address without a lookup. An
		// IPv6 address keeps its scope.
		JavaLocalRef<jobject> toJavaSocketAddress(JNIEnv * env, const webrtc::SocketAddress & address);
	}
}

#endif
