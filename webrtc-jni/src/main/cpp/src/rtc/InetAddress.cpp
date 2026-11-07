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

#include "rtc/InetAddress.h"
#include "JavaClasses.h"
#include "JavaNullPointerException.h"
#include "JavaUtils.h"
#include "JavaWrappedException.h"

#include <algorithm>
#include <cstring>

namespace jni
{
	namespace InetAddress
	{
		webrtc::IPAddress toNative(JNIEnv * env, const JavaRef<jobject> & address)
		{
			if (address.get() == nullptr) {
				throw JavaWrappedException(JavaLocalRef<jthrowable>(env,
					JavaNullPointerException(env, "InetAddress must not be null")));
			}

			const auto javaClass = JavaClasses::get<JavaInetAddressClass>(env);

			JavaLocalRef<jbyteArray> bytes(env, static_cast<jbyteArray>(env->CallObjectMethod(address.get(), javaClass->getAddress)));
			ExceptionCheck(env);

			jsize length = env->GetArrayLength(bytes.get());

			if (length == 4) {
				in_addr ipv4;
				env->GetByteArrayRegion(bytes.get(), 0, length, reinterpret_cast<jbyte *>(&ipv4));
				return webrtc::IPAddress(ipv4);
			}

			in6_addr ipv6;
			std::memset(&ipv6, 0, sizeof(ipv6));
			env->GetByteArrayRegion(bytes.get(), 0, std::min<jsize>(length, sizeof(ipv6)), reinterpret_cast<jbyte *>(&ipv6));
			return webrtc::IPAddress(ipv6);
		}

		JavaLocalRef<jobject> toJavaSocketAddress(JNIEnv * env, const webrtc::SocketAddress & address)
		{
			const auto javaClass = JavaClasses::get<JavaInetAddressClass>(env);
			const webrtc::IPAddress & ip = address.ipaddr();

			in_addr ipv4;
			in6_addr ipv6;
			const void * raw = &ipv6;
			jsize length = sizeof(ipv6);

			if (ip.family() == AF_INET) {
				ipv4 = ip.ipv4_address();
				raw = &ipv4;
				length = sizeof(ipv4);
			}
			else {
				ipv6 = ip.ipv6_address();
			}

			// Each step stops at a pending exception, which ExceptionCheck throws on.
			JavaLocalRef<jbyteArray> bytes(env, env->NewByteArray(length));
			ExceptionCheck(env);
			env->SetByteArrayRegion(bytes.get(), 0, length, static_cast<const jbyte *>(raw));

			JavaLocalRef<jobject> inetAddress(env, env->CallStaticObjectMethod(javaClass->cls, javaClass->getByAddress, bytes.get()));
			ExceptionCheck(env);

			JavaLocalRef<jobject> socketAddress(env, env->NewObject(javaClass->socketCls, javaClass->socketCtor,
				inetAddress.get(), static_cast<jint>(address.port())));
			ExceptionCheck(env);

			return socketAddress;
		}

		JavaInetAddressClass::JavaInetAddressClass(JNIEnv * env)
		{
			cls = FindClass(env, "java/net/InetAddress");
			getAddress = GetMethod(env, cls, "getAddress", "()[B");
			getByAddress = GetStaticMethod(env, cls, "getByAddress", "([B)Ljava/net/InetAddress;");
			socketCls = FindClass(env, "java/net/InetSocketAddress");
			socketCtor = GetMethod(env, socketCls, "<init>", "(Ljava/net/InetAddress;I)V");
		}
	}
}
