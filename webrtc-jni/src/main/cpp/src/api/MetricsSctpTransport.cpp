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

#include "api/MetricsSctpTransport.h"

#include "net/dcsctp/public/dcsctp_socket_factory.h"
#include "net/dcsctp/public/packet_observer.h"

namespace jni
{
	namespace
	{
		// Owned by the socket, so it goes with it and clears the slot, unless a newer
		// socket took the slot over. Passes packets on to the observer the transport
		// asked for, if any.
		class SlotObserver : public dcsctp::PacketObserver
		{
			public:
				SlotObserver(std::shared_ptr<SctpSocketSlot> slot, uint64_t generation,
					std::unique_ptr<dcsctp::PacketObserver> next) :
					slot_(std::move(slot)),
					generation_(generation),
					next_(std::move(next))
				{
				}

				~SlotObserver() override
				{
					if (slot_->generation == generation_) {
						slot_->socket = nullptr;
					}
				}

				void OnSentPacket(dcsctp::TimeMs now, std::span<const uint8_t> payload) override
				{
					if (next_) {
						next_->OnSentPacket(now, payload);
					}
				}

				void OnReceivedPacket(dcsctp::TimeMs now, std::span<const uint8_t> payload) override
				{
					if (next_) {
						next_->OnReceivedPacket(now, payload);
					}
				}

			private:
				const std::shared_ptr<SctpSocketSlot> slot_;
				const uint64_t generation_;
				const std::unique_ptr<dcsctp::PacketObserver> next_;
		};

		class SlotSocketFactory : public dcsctp::DcSctpSocketFactory
		{
			public:
				explicit SlotSocketFactory(std::shared_ptr<SctpSocketSlot> slot) :
					slot_(std::move(slot))
				{
				}

				std::unique_ptr<dcsctp::DcSctpSocketInterface> Create(absl::string_view logPrefix,
					dcsctp::DcSctpSocketCallbacks & callbacks, std::unique_ptr<dcsctp::PacketObserver> packetObserver,
					const dcsctp::DcSctpOptions & options) override
				{
					uint64_t generation = ++slot_->generation;
					auto observer = std::make_unique<SlotObserver>(slot_, generation, std::move(packetObserver));
					auto socket = dcsctp::DcSctpSocketFactory::Create(logPrefix, callbacks, std::move(observer), options);

					slot_->socket = socket.get();

					return socket;
				}

			private:
				const std::shared_ptr<SctpSocketSlot> slot_;
		};
	}

	MetricsSctpTransport::MetricsSctpTransport(const webrtc::Environment & env, webrtc::Thread * networkThread,
		webrtc::DtlsTransportInternal * transport) :
		MetricsSctpTransport(env, networkThread, transport, std::make_shared<SctpSocketSlot>())
	{
	}

	MetricsSctpTransport::MetricsSctpTransport(const webrtc::Environment & env, webrtc::Thread * networkThread,
		webrtc::DtlsTransportInternal * transport, std::shared_ptr<SctpSocketSlot> slot) :
		webrtc::DcSctpTransport(env, networkThread, transport, std::make_unique<SlotSocketFactory>(slot)),
		slot_(std::move(slot))
	{
	}

	std::optional<dcsctp::Metrics> MetricsSctpTransport::GetMetrics() const
	{
		if (slot_->socket == nullptr) {
			return std::nullopt;
		}

		return slot_->socket->GetMetrics();
	}

	MetricsSctpTransportFactory::MetricsSctpTransportFactory(webrtc::Thread * networkThread) :
		networkThread_(networkThread),
		defaultFactory_(networkThread)
	{
	}

	std::unique_ptr<webrtc::SctpTransportInternal> MetricsSctpTransportFactory::CreateSctpTransport(
		const webrtc::Environment & env, webrtc::DtlsTransportInternal * transport)
	{
		return std::make_unique<MetricsSctpTransport>(env, networkThread_, transport);
	}

	std::vector<uint8_t> MetricsSctpTransportFactory::GenerateConnectionToken(const webrtc::Environment & env)
	{
		return defaultFactory_.GenerateConnectionToken(env);
	}
}
