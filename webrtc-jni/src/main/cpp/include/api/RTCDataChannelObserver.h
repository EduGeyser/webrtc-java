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

#ifndef JNI_WEBRTC_API_RTC_DATA_CHANNEL_OBSERVER_H_
#define JNI_WEBRTC_API_RTC_DATA_CHANNEL_OBSERVER_H_

#include "JavaClass.h"
#include "JavaRef.h"

#include "api/data_channel_interface.h"
#include <api/DataBufferFactory.h>

#include <jni.h>
#include <memory>

namespace jni
{
	// Calls a Java RTCDataChannelObserver. WebRTC calls it on its network
	// thread when the Java observer asked for that, and otherwise posts each
	// event to the signaling thread first.
	class RTCDataChannelObserver : public webrtc::DataChannelObserver
	{
		public:
			// Asks the Java observer once whether it may be called on the
			// network thread. If that throws, the exception is left pending
			// and the observer must not be registered.
			explicit RTCDataChannelObserver(JNIEnv * env, const JavaGlobalRef<jobject> & observer);
			~RTCDataChannelObserver() = default;

			// Whether the calling thread is in a callback of an observer that
			// WebRTC calls on its network thread. That thread must not wait
			// for the signaling thread, which may be waiting for it.
			static bool InNetworkThreadCallback();

			// DataChannelObserver implementation.
			void OnStateChange() override;
			void OnMessage(const webrtc::DataBuffer & buffer) override;
			void OnBufferedAmountChange(uint64_t sent_data_size) override;
			bool IsOkToCallOnTheNetworkThread() override;

		private:
			class JavaRTCDataChannelObserverClass : public JavaClass
			{
				public:
					explicit JavaRTCDataChannelObserverClass(JNIEnv * env);

					jmethodID onStateChange;
					jmethodID onMessage;
					jmethodID onBufferedAmountChange;
					jmethodID isOkToCallOnTheNetworkThread;
			};

		private:
			JavaGlobalRef<jobject> observer;

			std::unique_ptr<DataBufferFactory> bufferFactory;

			const std::shared_ptr<JavaRTCDataChannelObserverClass> javaClass;

			const bool networkThread;
	};
}

#endif