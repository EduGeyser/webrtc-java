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

#include "api/RTCTransportMetrics.h"
#include "JavaClasses.h"
#include "JavaUtils.h"
#include "JNI_WebRTC.h"

namespace jni
{
	namespace RTCTransportMetrics
	{
		JavaLocalRef<jobject> toJava(JNIEnv * env, const TransportMetrics & metrics)
		{
			const auto javaClass = JavaClasses::get<JavaRTCTransportMetricsClass>(env);
			const dcsctp::Metrics sctp = metrics.sctp.value_or(dcsctp::Metrics());

			jobject object = env->CallStaticObjectMethod(javaClass->cls, javaClass->create,
				static_cast<jlong>(metrics.iceRoundTripTimeMs ? static_cast<int64_t>(*metrics.iceRoundTripTimeMs) : -1),
				static_cast<jboolean>(metrics.sctp.has_value()),
				static_cast<jlong>(sctp.srtt_ms),
				static_cast<jlong>(sctp.cwnd_bytes),
				static_cast<jlong>(sctp.unack_data_count),
				static_cast<jlong>(sctp.peer_rwnd_bytes),
				static_cast<jlong>(sctp.tx_packets_count),
				static_cast<jlong>(sctp.tx_messages_count),
				static_cast<jlong>(sctp.rtx_packets_count),
				static_cast<jlong>(sctp.rtx_bytes_count),
				static_cast<jlong>(sctp.rx_packets_count),
				static_cast<jlong>(sctp.rx_messages_count));

			return JavaLocalRef<jobject>(env, object);
		}

		JavaRTCTransportMetricsClass::JavaRTCTransportMetricsClass(JNIEnv * env)
		{
			cls = FindClass(env, PKG"RTCTransportMetrics");
			create = GetStaticMethod(env, cls, "create", "(JZJJJJJJJJJJ)L" PKG "RTCTransportMetrics;");
		}
	}
}
