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

#ifndef JNI_WEBRTC_API_PEER_CONNECTION_OBSERVER_H_
#define JNI_WEBRTC_API_PEER_CONNECTION_OBSERVER_H_

#include "JavaClass.h"
#include "JavaRef.h"

#include "api/RTCTransportMetrics.h"
#include "api/peer_connection_interface.h"
#include "rtc_base/thread.h"

#include <atomic>
#include <jni.h>
#include <memory>
#include <optional>

namespace jni
{
	class PeerConnectionObserver : public webrtc::PeerConnectionObserver
	{
		public:
			// The factory is the Java PeerConnectionFactory that creates the
			// connection, which the data channels the remote peer opens get.
			// Both references are taken over, so the caller checks them.
			PeerConnectionObserver(JNIEnv * env, JavaGlobalRef<jobject> observer, JavaGlobalRef<jobject> factory);
			virtual ~PeerConnectionObserver() = default;

			// PeerConnectionObserver implementation.
			void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState state) override;
			void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState state) override;
#ifndef WEBRTC_DATA_CHANNELS_ONLY
			void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override;
			void OnAddTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver, const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>> & streams) override;
			void OnRemoveTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) override;
#endif
			void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) override;
			void OnRenegotiationNeeded() override;
			void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState state) override;
			void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState state) override;
			void OnIceCandidate(const webrtc::IceCandidateInterface * candidate) override;
			void OnIceCandidateError(const std::string & address, int port, const std::string & url, int error_code, const std::string & error_text) override;
			void OnIceCandidateRemoved(const webrtc::IceCandidate * candidate) override;
			void OnIceConnectionReceivingChange(bool receiving) override;
			void OnIceSelectedCandidatePairChanged(const webrtc::CandidatePairChangeEvent & event) override;

			// Lets the observer read the selected connection itself. Set once, right after the
			// peer connection was created and before it can report a candidate pair. The peer
			// connection is not referenced: it only calls this observer while it is alive.
			void setPeerConnection(webrtc::PeerConnectionInterface * peerConnection, webrtc::Thread * networkThread);

			// Of the transport that carries the data channels; nothing before it exists.
			// Blocks on the network thread.
			std::optional<TransportMetrics> transportMetrics();

		private:
			std::optional<webrtc::CandidatePair> selectedCandidatePair(const std::string & transportName);

			class JavaPeerConnectionObserverClass : public JavaClass
			{
				public:
					explicit JavaPeerConnectionObserverClass(JNIEnv * env);

					jmethodID onConnectionChange;
					jmethodID onSignalingChange;
#ifndef WEBRTC_DATA_CHANNELS_ONLY
					jmethodID onTrack;
					jmethodID onAddTrack;
					jmethodID onRemoveTrack;
#endif
					jmethodID onDataChannel;
					jmethodID onRenegotiationNeeded;
					jmethodID onIceConnectionChange;
					jmethodID onIceGatheringChange;
					jmethodID onIceCandidate;
					jmethodID onIceCandidateError;
					jmethodID onIceCandidateRemoved;
					jmethodID onIceConnectionReceivingChange;
					jmethodID onSelectedCandidatePairChanged;
			};

		private:
			JavaGlobalRef<jobject> observer;

			JavaGlobalRef<jobject> factory;

			const std::shared_ptr<JavaPeerConnectionObserverClass> javaClass;

			std::atomic<webrtc::PeerConnectionInterface *> peerConnection { nullptr };
			std::atomic<webrtc::Thread *> networkThread { nullptr };
	};
}

#endif