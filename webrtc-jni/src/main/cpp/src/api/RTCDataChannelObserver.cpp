/*
 * Copyright 2019 Alex Andres
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

#include "api/RTCDataChannelObserver.h"
#include "api/WebRTCUtils.h"
#include "JavaFactories.h"
#include "JavaUtils.h"
#include "JNI_WebRTC.h"

#include <utility>

namespace jni
{
	namespace
	{
		// How many network thread callbacks the calling thread is in. One can
		// run inside another, when the Java observer calls into its channel.
		thread_local int networkThreadCallbacks = 0;

		// Counts the calling thread as in a network thread callback for as
		// long as it lives, if the observer is called there.
		class CallbackScope
		{
			public:
				explicit CallbackScope(bool networkThread) :
					counted(networkThread)
				{
					if (counted) {
						++networkThreadCallbacks;
					}
				}

				~CallbackScope()
				{
					if (counted) {
						--networkThreadCallbacks;
					}
				}

				CallbackScope(const CallbackScope &) = delete;
				CallbackScope & operator=(const CallbackScope &) = delete;

			private:
				const bool counted;
		};
	}

	RTCDataChannelObserver::RTCDataChannelObserver(JNIEnv * env, JavaGlobalRef<jobject> observer) :
		observer(std::move(observer)),
		bufferFactory(std::make_unique<DataBufferFactory>(env, PKG"RTCDataChannelBuffer")),
		javaClass(JavaClasses::get<JavaRTCDataChannelObserverClass>(env)),
		networkThread(env->CallBooleanMethod(this->observer, javaClass->isOkToCallOnTheNetworkThread) == JNI_TRUE)
	{
	}

	bool RTCDataChannelObserver::InNetworkThreadCallback()
	{
		return networkThreadCallbacks > 0;
	}

	bool RTCDataChannelObserver::IsOkToCallOnTheNetworkThread()
	{
		return networkThread;
	}

	void RTCDataChannelObserver::Stop()
	{
		stopped.store(true, std::memory_order_release);
	}

	void RTCDataChannelObserver::OnStateChange()
	{
		if (stopped.load(std::memory_order_acquire)) {
			return;
		}

		JNIEnv * env = AttachCurrentThread();

		if (env == nullptr) {
			return;
		}

		CallbackScope scope(networkThread);

		env->CallVoidMethod(observer, javaClass->onStateChange);

		ReportPendingException(env);
	}

	void RTCDataChannelObserver::OnMessage(const webrtc::DataBuffer & buffer)
	{
		if (stopped.load(std::memory_order_acquire)) {
			return;
		}

		JNIEnv * env = AttachCurrentThread();

		if (env == nullptr) {
			return;
		}

		CallbackScope scope(networkThread);

		try {
			JavaLocalRef<jobject> jBuffer = bufferFactory->create(env, &buffer);

			env->CallVoidMethod(observer, javaClass->onMessage, jBuffer.get());
		}
		catch (...) {
			ThrowCxxJavaException(env);
		}

		ReportPendingException(env);
	}

	void RTCDataChannelObserver::OnBufferedAmountChange(uint64_t sent_data_size)
	{
		if (stopped.load(std::memory_order_acquire)) {
			return;
		}

		JNIEnv * env = AttachCurrentThread();

		if (env == nullptr) {
			return;
		}

		CallbackScope scope(networkThread);

		env->CallVoidMethod(observer, javaClass->onBufferedAmountChange, static_cast<jlong>(sent_data_size));

		ReportPendingException(env);
	}

	RTCDataChannelObserver::JavaRTCDataChannelObserverClass::JavaRTCDataChannelObserverClass(JNIEnv * env)
	{
		jclass cls = FindClass(env, PKG"RTCDataChannelObserver");

		onStateChange = GetMethod(env, cls, "onStateChange", "()V");
		onMessage = GetMethod(env, cls, "onMessage", "(L" PKG "RTCDataChannelBuffer;)V");
		onBufferedAmountChange = GetMethod(env, cls, "onBufferedAmountChange", "(J)V");
		isOkToCallOnTheNetworkThread = GetMethod(env, cls, "isOkToCallOnTheNetworkThread", "()Z");
	}
}
