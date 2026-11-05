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

#ifndef JNI_WEBRTC_API_RTC_TRANSPORT_METRICS_H_
#define JNI_WEBRTC_API_RTC_TRANSPORT_METRICS_H_

#include "JavaClass.h"
#include "JavaRef.h"

#include "net/dcsctp/public/dcsctp_socket.h"

#include <cstdint>
#include <optional>

#include <jni.h>

namespace jni
{
	// What a connection's data channel transport reports without a stats report.
	struct TransportMetrics
	{
		// The selected candidate pair's latest STUN round trip time.
		std::optional<uint32_t> iceRoundTripTimeMs;
		// Nothing before the SCTP association starts.
		std::optional<dcsctp::Metrics> sctp;
	};

	namespace RTCTransportMetrics
	{
		class JavaRTCTransportMetricsClass : public JavaClass
		{
			public:
				explicit JavaRTCTransportMetricsClass(JNIEnv * env);

				jclass cls;
				jmethodID create;
		};

		JavaLocalRef<jobject> toJava(JNIEnv * env, const TransportMetrics & metrics);
	}
}

#endif
