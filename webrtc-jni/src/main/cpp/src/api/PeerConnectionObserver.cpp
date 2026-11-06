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

#include "api/PeerConnectionObserver.h"
#include "api/DataChannelFactory.h"
#include "api/MetricsSctpTransport.h"
#include "api/RTCCandidatePairChangeEvent.h"
#include "api/RTCIceCandidate.h"
#include "api/RTCPeerConnectionIceErrorEvent.h"
#include "api/WebRTCUtils.h"
#include "Exception.h"
#include "JavaArray.h"
#include "JavaClasses.h"
#include "JavaEnums.h"
#include "JavaFactories.h"
#include "JavaRuntimeException.h"
#include "JavaString.h"
#include "JavaUtils.h"
#include "JNI_WebRTC.h"

#include "api/dtls_transport_interface.h"
#include "api/ice_transport_interface.h"
#include "api/sctp_transport_interface.h"
#include "p2p/base/connection.h"
#include "p2p/base/p2p_transport_channel.h"
#include "pc/sctp_transport.h"

namespace jni
{
	PeerConnectionObserver::PeerConnectionObserver(JNIEnv * env, const JavaGlobalRef<jobject> & observer,
		const JavaGlobalRef<jobject> & factory) :
		observer(observer),
		factory(factory),
		javaClass(JavaClasses::get<JavaPeerConnectionObserverClass>(env))
	{
	}

	void PeerConnectionObserver::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState state)
	{
		JNIEnv * env = AttachCurrentThread();

		if (env == nullptr) {
			return;
		}

		try {
			auto jState = JavaEnums::toJava(env, state);

			env->CallVoidMethod(observer, javaClass->onConnectionChange, jState.get());
		}
		catch (...) {
			ThrowCxxJavaException(env);
		}

		ReportPendingException(env);
	}

	void PeerConnectionObserver::OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState state)
	{
		JNIEnv * env = AttachCurrentThread();

		if (env == nullptr) {
			return;
		}

		try {
			auto jState = JavaEnums::toJava(env, state);

			env->CallVoidMethod(observer, javaClass->onSignalingChange, jState.get());
		}
		catch (...) {
			ThrowCxxJavaException(env);
		}

		ReportPendingException(env);
	}

#ifndef WEBRTC_DATA_CHANNELS_ONLY
	void PeerConnectionObserver::OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver)
	{
		JNIEnv * env = AttachCurrentThread();

		if (env == nullptr) {
			return;
		}

		try {
			// transceiver is passed by value, so this call owns one reference on
			// it; transfer that reference into the Java wrapper, which disposes
			// it, instead of releasing it right back when transceiver goes out
			// of scope and leaving the wrapper's pointer unowned.
			auto jTransceiver = JavaFactories::createOwning(env, std::move(transceiver));

			env->CallVoidMethod(observer, javaClass->onTrack, jTransceiver.get());
		}
		catch (...) {
			ThrowCxxJavaException(env);
		}

		ReportPendingException(env);
	}

	void PeerConnectionObserver::OnAddTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver, const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>> & streams)
	{
		JNIEnv * env = AttachCurrentThread();

		if (env == nullptr) {
			return;
		}

		JavaLocalRef<jobjectArray> streamArray;

		try {
			streamArray = createObjectArray(env, streams);

			// receiver is passed by value, so this call owns one reference on
			// it; transfer that reference into the Java wrapper, which
			// disposes it, instead of releasing it right back when receiver
			// goes out of scope and leaving the wrapper's pointer unowned.
			auto jReceiver = JavaFactories::createOwning(env, std::move(receiver));

			env->CallVoidMethod(observer, javaClass->onAddTrack, jReceiver.get(), streamArray.get());
		}
		catch (...) {
			ThrowCxxJavaException(env);
		}

		ReportPendingException(env);
	}

	void PeerConnectionObserver::OnRemoveTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver)
	{
		JNIEnv * env = AttachCurrentThread();

		if (env == nullptr) {
			return;
		}

		try {
			// receiver is passed by value, so this call owns one reference on
			// it; transfer that reference into the Java wrapper, which disposes
			// it, instead of releasing it right back when receiver goes out of
			// scope and leaving the wrapper's pointer unowned.
			auto jReceiver = JavaFactories::createOwning(env, std::move(receiver));

			env->CallVoidMethod(observer, javaClass->onRemoveTrack, jReceiver.get());
		}
		catch (...) {
			ThrowCxxJavaException(env);
		}

		ReportPendingException(env);
	}
#endif

	void PeerConnectionObserver::OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel)
	{
		JNIEnv * env = AttachCurrentThread();

		if (env == nullptr) {
			return;
		}

		try {
			auto jDataChannel = JavaFactories::createOwning(env, std::move(channel), [&](jobject jChannel) {
				DataChannelFactory::Set(env, jChannel, factory);
			});

			env->CallVoidMethod(observer, javaClass->onDataChannel, jDataChannel.get());
		}
		catch (...) {
			ThrowCxxJavaException(env);
		}

		ReportPendingException(env);
	}

	void PeerConnectionObserver::OnRenegotiationNeeded()
	{
		JNIEnv * env = AttachCurrentThread();

		if (env == nullptr) {
			return;
		}

		env->CallVoidMethod(observer, javaClass->onRenegotiationNeeded);

		ReportPendingException(env);
	}

	void PeerConnectionObserver::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState state)
	{
		JNIEnv * env = AttachCurrentThread();

		if (env == nullptr) {
			return;
		}

		try {
			auto jState = JavaEnums::toJava(env, state);

			env->CallVoidMethod(observer, javaClass->onIceConnectionChange, jState.get());
		}
		catch (...) {
			ThrowCxxJavaException(env);
		}

		ReportPendingException(env);
	}

	void PeerConnectionObserver::OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState state)
	{
		JNIEnv * env = AttachCurrentThread();

		if (env == nullptr) {
			return;
		}

		try {
			auto jState = JavaEnums::toJava(env, state);

			env->CallVoidMethod(observer, javaClass->onIceGatheringChange, jState.get());
		}
		catch (...) {
			ThrowCxxJavaException(env);
		}

		ReportPendingException(env);
	}

	void PeerConnectionObserver::OnIceCandidate(const webrtc::IceCandidateInterface * candidate)
	{
		JNIEnv * env = AttachCurrentThread();

		if (env == nullptr) {
			return;
		}

		try {
			JavaLocalRef<jobject> jCandidate = RTCIceCandidate::toJava(env, candidate);

			env->CallVoidMethod(observer, javaClass->onIceCandidate, jCandidate.get());
		}
		catch (...) {
			ThrowCxxJavaException(env);
		}

		ReportPendingException(env);
	}

	void PeerConnectionObserver::OnIceCandidateError(const std::string & address, int port, const std::string & url, int error_code, const std::string & error_text)
	{
		JNIEnv * env = AttachCurrentThread();

		if (env == nullptr) {
			return;
		}

		try {
			JavaLocalRef<jobject> event = RTCPeerConnectionIceErrorEvent::toJava(env, address, port, url, error_code, error_text);

			env->CallVoidMethod(observer, javaClass->onIceCandidateError, event.get());
		}
		catch (...) {
			ThrowCxxJavaException(env);
		}

		ReportPendingException(env);
	}

	void PeerConnectionObserver::OnIceCandidateRemoved(const webrtc::IceCandidate * candidate)
	{
		JNIEnv * env = AttachCurrentThread();

		if (env == nullptr || candidate == nullptr) {
			return;
		}

		try {
			JavaLocalRef<jobject> jCandidate = RTCIceCandidate::toJava(env, candidate);

			if (!env->ExceptionCheck()) {
				env->CallVoidMethod(observer, javaClass->onIceCandidateRemoved, jCandidate.get());
			}
		}
		catch (...) {
			ThrowCxxJavaException(env);
		}

		ReportPendingException(env);
	}

	void PeerConnectionObserver::OnIceConnectionReceivingChange(bool receiving)
	{
		JNIEnv * env = AttachCurrentThread();

		if (env == nullptr) {
			return;
		}

		env->CallVoidMethod(observer, javaClass->onIceConnectionReceivingChange, receiving);

		ReportPendingException(env);
	}

	void PeerConnectionObserver::setPeerConnection(webrtc::PeerConnectionInterface * pc, webrtc::Thread * thread)
	{
		peerConnection.store(pc);
		networkThread.store(thread);
	}

	std::optional<webrtc::CandidatePair> PeerConnectionObserver::selectedCandidatePair(const std::string & transportName)
	{
		webrtc::PeerConnectionInterface * pc = peerConnection.load();
		webrtc::Thread * thread = networkThread.load();

		if (pc == nullptr || thread == nullptr) {
			return std::nullopt;
		}

		// The transports and the selected connection belong to the network thread. Callbacks run
		// on the signaling thread, which may block on the network thread, as WebRTC itself does.
		return thread->BlockingCall([pc, &transportName]() -> std::optional<webrtc::CandidatePair> {
			webrtc::scoped_refptr<webrtc::DtlsTransportInterface> dtls = pc->LookupDtlsTransportByMid(transportName);

			if (dtls == nullptr) {
				// A data channel only connection has one transport, the SCTP one.
				webrtc::scoped_refptr<webrtc::SctpTransportInterface> sctp = pc->GetSctpTransport();
				dtls = sctp ? sctp->dtls_transport() : nullptr;
			}
			if (dtls == nullptr) {
				return std::nullopt;
			}

			webrtc::scoped_refptr<webrtc::IceTransportInterface> ice = dtls->ice_transport();
			auto channel = ice ? dynamic_cast<const webrtc::P2PTransportChannel *>(ice->internal()) : nullptr;
			const webrtc::Connection * connection = channel ? channel->selected_connection() : nullptr;

			if (connection == nullptr) {
				return std::nullopt;
			}

			webrtc::CandidatePair pair;
			pair.local = connection->local_candidate();
			pair.remote = connection->remote_candidate();

			return pair;
		});
	}

	std::optional<TransportMetrics> PeerConnectionObserver::transportMetrics()
	{
		webrtc::PeerConnectionInterface * pc = peerConnection.load();
		webrtc::Thread * thread = networkThread.load();

		if (pc == nullptr || thread == nullptr) {
			return std::nullopt;
		}

		return thread->BlockingCall([pc]() -> std::optional<TransportMetrics> {
			webrtc::scoped_refptr<webrtc::SctpTransportInterface> sctp = pc->GetSctpTransport();

			if (sctp == nullptr) {
				return std::nullopt;
			}

			TransportMetrics metrics;

			// The peer connection hands out its own SctpTransport, which holds the
			// transport our factory created.
			auto transport = dynamic_cast<webrtc::SctpTransport *>(sctp.get());
			auto internal = transport ? dynamic_cast<MetricsSctpTransport *>(transport->internal()) : nullptr;

			if (internal != nullptr) {
				metrics.sctp = internal->GetMetrics();
			}

			webrtc::scoped_refptr<webrtc::DtlsTransportInterface> dtls = sctp->dtls_transport();
			webrtc::scoped_refptr<webrtc::IceTransportInterface> ice = dtls ? dtls->ice_transport() : nullptr;
			auto channel = ice ? dynamic_cast<const webrtc::P2PTransportChannel *>(ice->internal()) : nullptr;
			const webrtc::Connection * selected = channel ? channel->selected_connection() : nullptr;

			if (selected != nullptr) {
				// Its stats are not const; the channel's own list gives the same connection
				// without a cast.
				for (webrtc::Connection * connection : channel->connections()) {
					if (connection == selected) {
						metrics.iceRoundTripTimeMs = connection->stats().current_round_trip_time_ms;
						break;
					}
				}
			}

			return metrics;
		});
	}

	void PeerConnectionObserver::OnIceSelectedCandidatePairChanged(const webrtc::CandidatePairChangeEvent & event)
	{
		JNIEnv * env = AttachCurrentThread();

		if (env == nullptr) {
			return;
		}

		try {
			// WebRTC hands out a copy of the pair sanitized for browser privacy: a peer reflexive
			// remote candidate, learned from a connectivity check instead of signaling, arrives
			// without its address. Report the selected connection's own candidates instead, so the
			// application learns the address it is actually connected to. The sanitized copy is
			// only kept when the connection cannot be read, for example while closing.
			webrtc::CandidatePairChangeEvent reported = event;

			if (std::optional<webrtc::CandidatePair> pair = selectedCandidatePair(event.transport_name)) {
				reported.selected_candidate_pair = *pair;
			}

			JavaLocalRef<jobject> jEvent = RTCCandidatePairChangeEvent::toJava(env, reported);

			if (!env->ExceptionCheck()) {
				env->CallVoidMethod(observer, javaClass->onSelectedCandidatePairChanged, jEvent.get());
			}
		}
		catch (...) {
			ThrowCxxJavaException(env);
		}

		ReportPendingException(env);
	}

	PeerConnectionObserver::JavaPeerConnectionObserverClass::JavaPeerConnectionObserverClass(JNIEnv * env)
	{
		jclass cls = FindClass(env, PKG"PeerConnectionObserver");

		onConnectionChange = GetMethod(env, cls, "onConnectionChange", "(L" PKG "RTCPeerConnectionState;)V");
		onSignalingChange = GetMethod(env, cls, "onSignalingChange", "(L" PKG "RTCSignalingState;)V");
#ifndef WEBRTC_DATA_CHANNELS_ONLY
		onTrack = GetMethod(env, cls, "onTrack", "(L" PKG "RTCRtpTransceiver;)V");
		onAddTrack = GetMethod(env, cls, "onAddTrack", "(L" PKG "RTCRtpReceiver;[L" PKG_MEDIA "MediaStream;)V");
		onRemoveTrack = GetMethod(env, cls, "onRemoveTrack", "(L" PKG "RTCRtpReceiver;)V");
#endif
		onDataChannel = GetMethod(env, cls, "onDataChannel", "(L" PKG "RTCDataChannel;)V");
		onRenegotiationNeeded = GetMethod(env, cls, "onRenegotiationNeeded", "()V");
		onIceConnectionChange = GetMethod(env, cls, "onIceConnectionChange", "(L" PKG "RTCIceConnectionState;)V");
		onIceGatheringChange = GetMethod(env, cls, "onIceGatheringChange", "(L" PKG "RTCIceGatheringState;)V");
		onIceCandidate = GetMethod(env, cls, "onIceCandidate", "(L" PKG "RTCIceCandidate;)V");
		onIceCandidateError = GetMethod(env, cls, "onIceCandidateError", "(L" PKG "RTCPeerConnectionIceErrorEvent;)V");
		onIceCandidateRemoved = GetMethod(env, cls, "onIceCandidateRemoved", "(L" PKG "RTCIceCandidate;)V");
		onIceConnectionReceivingChange = GetMethod(env, cls, "onIceConnectionReceivingChange", "(Z)V");
		onSelectedCandidatePairChanged = GetMethod(env, cls, "onSelectedCandidatePairChanged", "(L" PKG "RTCCandidatePairChangeEvent;)V");
	}
}
