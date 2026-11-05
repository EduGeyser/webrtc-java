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
#include "api/WebRTCUtils.h"
#include "rtc/InetAddress.h"
#include "JavaArray.h"
#include "JavaRef.h"
#include "JavaString.h"
#include "JavaUtils.h"

#include "rtc_base/zero_memory.h"

#include <memory>
#include <string>
#include <utility>
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

namespace
{
	std::string CopyBytes(JNIEnv * env, jbyteArray array)
	{
		std::string bytes(env->GetArrayLength(array), '\0');
		env->GetByteArrayRegion(array, 0, static_cast<jsize>(bytes.size()), reinterpret_cast<jbyte *>(bytes.data()));
		return bytes;
	}
}

JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_IceUdpEndpoint_setPendingLimits
(JNIEnv * env, jobject caller, jint maxPending, jlong timeoutMs)
{
	EndpointHandle * endpoint = GetHandle<EndpointHandle>(env, caller);
	CHECK_HANDLE(endpoint);

	(*endpoint)->SetPendingLimits(static_cast<size_t>(maxPending), static_cast<int64_t>(timeoutMs));
}

JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_IceUdpEndpoint_enablePendingRequests
(JNIEnv * env, jobject caller, jboolean enabled)
{
	EndpointHandle * endpoint = GetHandle<EndpointHandle>(env, caller);
	CHECK_HANDLE(endpoint);

	if (!enabled) {
		(*endpoint)->SetPendingRequestHandler(nullptr);
		return;
	}

	jclass cls = env->GetObjectClass(caller);
	jmethodID method = env->GetMethodID(cls, "onPendingRequest", "(J[BI[BILjava/lang/String;Ljava/lang/String;)V");
	env->DeleteLocalRef(cls);

	if (method == nullptr) {
		return;
	}

	auto target = std::make_shared<jni::JavaGlobalRef<jobject>>(env, caller);

	(*endpoint)->SetPendingRequestHandler([target, method](const jni::PendingRequestInfo & request) {
		JNIEnv * env = AttachCurrentThread();

		if (env == nullptr) {
			return;
		}

		try {
			jni::JavaLocalRef<jbyteArray> local = jni::InetAddress::toJavaBytes(env, request.local.ipaddr());
			jni::JavaLocalRef<jbyteArray> remote = jni::InetAddress::toJavaBytes(env, request.remote.ipaddr());

			env->CallVoidMethod(target->get(), method, static_cast<jlong>(request.id),
				local.get(), static_cast<jint>(request.local.port()),
				remote.get(), static_cast<jint>(request.remote.port()),
				jni::JavaString::toJava(env, request.localUfrag).get(),
				jni::JavaString::toJava(env, request.remoteUfrag).get());
		}
		catch (...) {
			ThrowCxxJavaException(env);
		}

		jni::ReportPendingException(env);
	});
}

JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_IceUdpEndpoint_setDerivedPasswords
(JNIEnv * env, jobject caller, jbyteArray jContext, jobjectArray jPrefixes, jobjectArray jKeys)
{
	EndpointHandle * endpoint = GetHandle<EndpointHandle>(env, caller);
	CHECK_HANDLE(endpoint);

	std::string context = CopyBytes(env, jContext);
	std::vector<std::pair<std::string, std::string>> keys;
	jsize count = env->GetArrayLength(jPrefixes);

	for (jsize i = 0; i < count; i++) {
		jni::JavaLocalRef<jstring> prefix(env, static_cast<jstring>(env->GetObjectArrayElement(jPrefixes, i)));
		jni::JavaLocalRef<jbyteArray> key(env, static_cast<jbyteArray>(env->GetObjectArrayElement(jKeys, i)));

		keys.emplace_back(jni::JavaString::toNative(env, prefix), CopyBytes(env, key.get()));
	}

	// The endpoint takes the buffers over; nothing is left here to wipe.
	(*endpoint)->SetDerivedPasswords(std::move(context), std::move(keys));
}

JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_IceUdpEndpoint_rejectRequest
(JNIEnv * env, jobject caller, jlong request)
{
	EndpointHandle * endpoint = GetHandle<EndpointHandle>(env, caller);
	CHECK_HANDLE(endpoint);

	(*endpoint)->Reject(static_cast<uint64_t>(request));
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
