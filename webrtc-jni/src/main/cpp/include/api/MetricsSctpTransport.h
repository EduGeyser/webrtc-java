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

#ifndef JNI_WEBRTC_API_METRICS_SCTP_TRANSPORT_H_
#define JNI_WEBRTC_API_METRICS_SCTP_TRANSPORT_H_

#include "api/environment/environment.h"
#include "api/transport/sctp_transport_factory_interface.h"
#include "media/sctp/dcsctp_transport.h"
#include "media/sctp/sctp_transport_factory.h"
#include "net/dcsctp/public/dcsctp_socket.h"
#include "rtc_base/thread.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace jni
{
	// The socket of a transport, which DcSctpTransport keeps to itself. A transport
	// drops its socket on a DTLS restart and makes a new one later.
	struct SctpSocketSlot
	{
		dcsctp::DcSctpSocketInterface * socket = nullptr;
		uint64_t generation = 0;
	};

	// libwebrtc's SCTP transport, whose association metrics libwebrtc does not
	// surface: srtt, congestion window, unacked data, the peer's receive window,
	// retransmissions and packet counts. Created, used and destroyed on the network
	// thread.
	class MetricsSctpTransport : public webrtc::DcSctpTransport
	{
		public:
			MetricsSctpTransport(const webrtc::Environment & env, webrtc::Thread * networkThread,
				webrtc::DtlsTransportInternal * transport);

			// Nothing before the association starts or after a DTLS restart.
			std::optional<dcsctp::Metrics> GetMetrics() const;

		private:
			MetricsSctpTransport(const webrtc::Environment & env, webrtc::Thread * networkThread,
				webrtc::DtlsTransportInternal * transport, std::shared_ptr<SctpSocketSlot> slot);

			const std::shared_ptr<SctpSocketSlot> slot_;
	};

	// Creates MetricsSctpTransports; otherwise the factory libwebrtc uses by default.
	class MetricsSctpTransportFactory : public webrtc::SctpTransportFactoryInterface
	{
		public:
			explicit MetricsSctpTransportFactory(webrtc::Thread * networkThread);

			std::unique_ptr<webrtc::SctpTransportInternal> CreateSctpTransport(const webrtc::Environment & env,
				webrtc::DtlsTransportInternal * transport) override;

			std::vector<uint8_t> GenerateConnectionToken(const webrtc::Environment & env) override;

		private:
			webrtc::Thread * const networkThread_;
			webrtc::SctpTransportFactory defaultFactory_;
	};
}

#endif
