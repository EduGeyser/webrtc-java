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

#include "JNI_IceUdpEndpoint.h"
#include "api/IceUdpEndpoint.h"
#include "rtc/InetAddress.h"
#include "JavaArray.h"
#include "JavaRef.h"
#include "JavaUtils.h"

#include <memory>
#include <vector>

// The Java object holds a shared_ptr, as does every connection that gathers on the
// endpoint, so the endpoint lives until the last of them is gone.
using EndpointHandle = std::shared_ptr<jni::IceUdpEndpoint>;

JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_IceUdpEndpoint_initialize
(JNIEnv * env, jobject caller, jint port, jobjectArray jAddresses)
{
	try {
		std::vector<webrtc::IPAddress> addresses = jni::JavaArray::toNativeVector<webrtc::IPAddress>(env,
			jni::JavaLocalRef<jobjectArray>(env, jAddresses), &jni::InetAddress::toNative);

		SetHandle(env, caller, new EndpointHandle(jni::IceUdpEndpoint::Create(static_cast<uint16_t>(port), std::move(addresses))));
	}
	catch (...) {
		ThrowCxxJavaException(env);
	}
}

JNIEXPORT jint JNICALL Java_io_github_sendablemetatype_webrtc_IceUdpEndpoint_getPort
(JNIEnv * env, jobject caller)
{
	EndpointHandle * endpoint = GetHandle<EndpointHandle>(env, caller);
	CHECK_HANDLEV(endpoint, 0);

	return (*endpoint)->port();
}

JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_IceUdpEndpoint_inject
(JNIEnv * env, jobject caller, jobject jLocalAddress, jint localPort, jobject jRemoteAddress, jint remotePort,
	jbyteArray jData)
{
	EndpointHandle * endpoint = GetHandle<EndpointHandle>(env, caller);
	CHECK_HANDLE(endpoint);

	try {
		webrtc::SocketAddress local(jni::InetAddress::toNative(env, jni::JavaLocalRef<jobject>(env, jLocalAddress)), localPort);
		webrtc::SocketAddress remote(jni::InetAddress::toNative(env, jni::JavaLocalRef<jobject>(env, jRemoteAddress)), remotePort);

		std::vector<uint8_t> data(env->GetArrayLength(jData));
		env->GetByteArrayRegion(jData, 0, static_cast<jsize>(data.size()), reinterpret_cast<jbyte *>(data.data()));

		(*endpoint)->Inject(local, remote, std::move(data));
	}
	catch (...) {
		ThrowCxxJavaException(env);
	}
}

JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_IceUdpEndpoint_dispose
(JNIEnv * env, jobject caller)
{
	EndpointHandle * endpoint = GetHandle<EndpointHandle>(env, caller);
	CHECK_HANDLE(endpoint);

	SetHandle<std::nullptr_t>(env, caller, nullptr);

	try {
		(*endpoint)->Close();
	}
	catch (...) {
		ThrowCxxJavaException(env);
	}

	delete endpoint;
}
