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
#include "NativeHandle.h"
#include "api/IceUdpEndpoint.h"
#include "api/WebRTCUtils.h"
#include "rtc/InetAddress.h"
#include "JavaArray.h"
#include "JavaBoundary.h"
#include "JavaRef.h"
#include "JavaString.h"
#include "JavaUtils.h"
#include "JNI_WebRTC.h"

#include "rtc_base/zero_memory.h"

#include <memory>
#include <optional>
#include <string>
#include <thread>
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
	return jni::CallFromJava(env, [&]() -> jint {
		jni::NativeLease<EndpointHandle> endpoint(env, caller);
		CHECK_HANDLEV(endpoint, 0);

		return (*endpoint)->port();
	});
}

JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_IceUdpEndpoint_inject
(JNIEnv * env, jobject caller, jobject jLocalAddress, jint localPort, jobject jRemoteAddress, jint remotePort,
	jbyteArray jData)
{
	jni::CallFromJava(env, [&]() {
		jni::NativeLease<EndpointHandle> endpoint(env, caller);
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
	});
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
	jni::CallFromJava(env, [&]() {
		jni::NativeLease<EndpointHandle> endpoint(env, caller);
		CHECK_HANDLE(endpoint);

		(*endpoint)->SetPendingLimits(static_cast<size_t>(maxPending), static_cast<int64_t>(timeoutMs));
	});
}

JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_IceUdpEndpoint_enablePendingRequests
(JNIEnv * env, jobject caller, jboolean enabled)
{
	jni::CallFromJava(env, [&]() {
		jni::NativeLease<EndpointHandle> endpoint(env, caller);
		CHECK_HANDLE(endpoint);

		if (!enabled) {
			(*endpoint)->SetPendingRequestHandler(nullptr);
			return;
		}

		jclass cls = env->GetObjectClass(caller);
		jmethodID method = env->GetMethodID(cls, "onPendingRequest",
			"(JLjava/net/InetSocketAddress;Ljava/net/InetSocketAddress;Ljava/lang/String;Ljava/lang/String;)V");
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
				jni::JavaLocalRef<jobject> local = jni::InetAddress::toJavaSocketAddress(env, request.local);
				jni::JavaLocalRef<jobject> remote = jni::InetAddress::toJavaSocketAddress(env, request.remote);

				env->CallVoidMethod(target->get(), method, static_cast<jlong>(request.id), local.get(), remote.get(),
					jni::JavaString::toJava(env, request.localUfrag).get(),
					jni::JavaString::toJava(env, request.remoteUfrag).get());
			}
			catch (...) {
				ThrowCxxJavaException(env);
			}

			jni::ReportPendingException(env);
		});
	});
}

JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_IceUdpEndpoint_setDerivedPasswords
(JNIEnv * env, jobject caller, jbyteArray jContext, jobjectArray jPrefixes, jobjectArray jKeys)
{
	jni::CallFromJava(env, [&]() {
		jni::NativeLease<EndpointHandle> endpoint(env, caller);
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
	});
}

JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_IceUdpEndpoint_rejectRequest
(JNIEnv * env, jobject caller, jlong request)
{
	jni::CallFromJava(env, [&]() {
		jni::NativeLease<EndpointHandle> endpoint(env, caller);
		CHECK_HANDLE(endpoint);

		(*endpoint)->Reject(static_cast<uint64_t>(request));
	});
}

JNIEXPORT jboolean JNICALL Java_io_github_sendablemetatype_webrtc_IceUdpEndpoint_applyProxyRelays
(JNIEnv * env, jobject caller, jobjectArray jRelays)
{
	return jni::CallFromJava(env, [&]() -> jboolean {
		jni::NativeLease<EndpointHandle> endpoint(env, caller);
		CHECK_HANDLEV(endpoint, JNI_FALSE);

		std::vector<std::string> relays;
		jsize count = env->GetArrayLength(jRelays);

		for (jsize i = 0; i < count; i++) {
			jni::JavaLocalRef<jstring> relay(env, static_cast<jstring>(env->GetObjectArrayElement(jRelays, i)));
			relays.push_back(jni::JavaString::toNative(env, relay));
		}

		return (*endpoint)->SetProxyRelays(std::move(relays)) ? JNI_TRUE : JNI_FALSE;
	});
}

JNIEXPORT jobject JNICALL Java_io_github_sendablemetatype_webrtc_IceUdpEndpoint_getProxyRelay
(JNIEnv * env, jobject caller, jobject jAddress, jint port)
{
	return jni::CallFromJava(env, [&]() -> jobject {
		jni::NativeLease<EndpointHandle> endpoint(env, caller);
		CHECK_HANDLEV(endpoint, nullptr);

		try {
			webrtc::SocketAddress client(jni::InetAddress::toNative(env, jni::JavaLocalRef<jobject>(env, jAddress)), port);
			std::optional<webrtc::SocketAddress> relay = (*endpoint)->ProxyRelayOf(client);

			return relay ? jni::InetAddress::toJavaSocketAddress(env, *relay).release() : nullptr;
		}
		catch (...) {
			ThrowCxxJavaException(env);
			return nullptr;
		}
	});
}

JNIEXPORT jlong JNICALL Java_io_github_sendablemetatype_webrtc_IceUdpEndpoint_openStunMonitor
(JNIEnv * env, jobject caller, jobject jLocal, jobject jServer, jint port)
{
	return jni::CallFromJava(env, [&]() -> jlong {
		jni::NativeLease<EndpointHandle> endpoint(env, caller);
		CHECK_HANDLEV(endpoint, 0);

		try {
			webrtc::IPAddress local = jni::InetAddress::toNative(env, jni::JavaLocalRef<jobject>(env, jLocal));
			webrtc::SocketAddress server(jni::InetAddress::toNative(env, jni::JavaLocalRef<jobject>(env, jServer)), port);

			return static_cast<jlong>((*endpoint)->OpenStunMonitor(local, server));
		}
		catch (...) {
			ThrowCxxJavaException(env);
			return 0;
		}
	});
}

JNIEXPORT jobject JNICALL Java_io_github_sendablemetatype_webrtc_IceUdpEndpoint_readStunMonitor
(JNIEnv * env, jobject caller, jlong monitor)
{
	return jni::CallFromJava(env, [&]() -> jobject {
		jni::NativeLease<EndpointHandle> endpoint(env, caller);
		CHECK_HANDLEV(endpoint, nullptr);

		std::optional<jni::IceUdpEndpoint::StunReading> reading = (*endpoint)->ReadStunMonitor(static_cast<uint64_t>(monitor));

		if (!reading) {
			return nullptr;
		}

		jclass cls = env->GetObjectClass(caller);
		jmethodID factory = env->GetStaticMethodID(cls, "stunReading",
			"(Ljava/net/InetSocketAddress;Ljava/net/InetSocketAddress;Ljava/net/InetSocketAddress;IJJJJ)L" PKG "IceUdpEndpoint$StunReading;");

		if (factory == nullptr) {
			env->DeleteLocalRef(cls);
			return nullptr;
		}

		try {
			jni::JavaLocalRef<jobject> server = jni::InetAddress::toJavaSocketAddress(env, reading->server);
			jni::JavaLocalRef<jobject> local = jni::InetAddress::toJavaSocketAddress(env, reading->local);
			jni::JavaLocalRef<jobject> mapped = reading->mapped
				? jni::InetAddress::toJavaSocketAddress(env, *reading->mapped)
				: jni::JavaLocalRef<jobject>(env, static_cast<jobject>(nullptr));

			jobject result = env->CallStaticObjectMethod(cls, factory, server.get(), local.get(), mapped.get(),
				static_cast<jint>(reading->state),
				static_cast<jlong>(reading->successfulResponses),
				static_cast<jlong>(reading->failedTransactions),
				static_cast<jlong>(reading->mappingRevision),
				static_cast<jlong>(reading->lastSuccessAgeMs.value_or(-1)));

			env->DeleteLocalRef(cls);
			return result;
		}
		catch (...) {
			env->DeleteLocalRef(cls);
			ThrowCxxJavaException(env);
			return nullptr;
		}
	});
}

JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_IceUdpEndpoint_closeStunMonitor
(JNIEnv * env, jobject caller, jlong monitor)
{
	jni::CallFromJava(env, [&]() {
		jni::NativeLease<EndpointHandle> endpoint(env, caller);
		CHECK_HANDLE(endpoint);

		(*endpoint)->CloseStunMonitor(static_cast<uint64_t>(monitor));
	});
}

JNIEXPORT jobject JNICALL Java_io_github_sendablemetatype_webrtc_IceUdpEndpoint_getStats
(JNIEnv * env, jobject caller)
{
	return jni::CallFromJava(env, [&]() -> jobject {
		jni::NativeLease<EndpointHandle> endpoint(env, caller);
		CHECK_HANDLEV(endpoint, nullptr);

		jni::IceUdpEndpoint::Stats stats = (*endpoint)->GetStats();

		jclass cls = env->GetObjectClass(caller);
		jmethodID factory = env->GetStaticMethodID(cls, "stats", "(JJJJJJJJJJJJJJJJ)L" PKG "IceUdpEndpoint$Stats;");
		jobject result = nullptr;

		if (factory != nullptr) {
			result = env->CallStaticObjectMethod(cls, factory,
				static_cast<jlong>(stats.datagramsReceived), static_cast<jlong>(stats.bytesReceived),
				static_cast<jlong>(stats.datagramsSent), static_cast<jlong>(stats.bytesSent),
				static_cast<jlong>(stats.unroutable), static_cast<jlong>(stats.unauthenticated),
				static_cast<jlong>(stats.pendingDropped), static_cast<jlong>(stats.announced),
				static_cast<jlong>(stats.replayed), static_cast<jlong>(stats.expired),
				static_cast<jlong>(stats.rejected), static_cast<jlong>(stats.relayDropped),
				static_cast<jlong>(stats.relayFiltered), static_cast<jlong>(stats.routes),
				static_cast<jlong>(stats.sessions), static_cast<jlong>(stats.pending));
		}

		env->DeleteLocalRef(cls);

		return result;
	});
}

JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_IceUdpEndpoint_dispose
(JNIEnv * env, jobject caller)
{
	jni::CallFromJava(env, [&]() {
		EndpointHandle * endpoint = jni::TakeHandle<EndpointHandle>(env, caller);

		if (endpoint == nullptr) {
			return;
		}

		jni::ReleaseHandle(env, endpoint, [endpoint](bool) {
			// A call from the request handler runs on the endpoint's thread, which
			// cannot stop itself.
			if ((*endpoint)->IsCurrent()) {
				std::thread([endpoint]() {
					(*endpoint)->Close();
					delete endpoint;
				}).detach();
				return;
			}

			(*endpoint)->Close();
			delete endpoint;
		});
	});
}
