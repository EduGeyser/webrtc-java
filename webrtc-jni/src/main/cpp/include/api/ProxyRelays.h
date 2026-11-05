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

#ifndef JNI_WEBRTC_API_PROXY_RELAYS_H_
#define JNI_WEBRTC_API_PROXY_RELAYS_H_

#include "rtc_base/ip_address.h"
#include "rtc_base/socket_address.h"

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace jni
{
	// PROXY protocol v2 on an IceUdpEndpoint: the listed relays and the clients they
	// speak for. A relay prefixes a client's datagrams with a header that declares the
	// client, and sends them from one source port per client.
	//
	// A header alone never routes a client to its relay. It only claims the client,
	// so that the relay's datagrams count as the client's. Replies to the client go
	// back through the relay only once a datagram the relay brought authenticated for
	// one of the endpoint's sessions. That stops a relay from claiming arbitrary
	// clients. While relays are set, nothing leaves the endpoint straight except STUN
	// server lookups.
	//
	// Used on the endpoint thread only.
	class ProxyRelays
	{
		public:
			// Whether a session could still hear from the client.
			using Alive = std::function<bool(const webrtc::SocketAddress & client)>;

			enum class Receive
			{
				// Not a datagram the endpoint takes while relays are set.
				kDrop,
				// From a client, or a STUN response from anyone.
				kPass,
				// From a client whose relay does not route it yet.
				kClaimed
			};

			// Parses numeric addresses and CIDR ranges. Keeps the flows of previous whose
			// relays are still listed. Nothing for an invalid entry.
			static std::unique_ptr<ProxyRelays> Create(const std::vector<std::string> & relays, Alive alive,
				const ProxyRelays * previous);

			// Strips a listed relay's header and attributes the datagram to the client it
			// declares: source becomes the client. With kClaimed, relay is set for Commit.
			Receive OnReceive(std::span<const uint8_t> & data, webrtc::SocketAddress & source,
				webrtc::SocketAddress & relay, int64_t nowMs);

			// Whether Commit would route the client through the relay.
			bool Admits(const webrtc::SocketAddress & relay, const webrtc::SocketAddress & client, int64_t nowMs);

			// Routes the client through the relay, once a datagram the relay brought
			// authenticated. False unless admitted.
			bool Commit(const webrtc::SocketAddress & relay, const webrtc::SocketAddress & client, int64_t nowMs);

			// The relay that routes the client.
			std::optional<webrtc::SocketAddress> RelayOf(const webrtc::SocketAddress & client) const;

			// Where a datagram to the destination goes: the relay that routes it, the
			// destination itself for a STUN server lookup, or nowhere.
			std::optional<webrtc::SocketAddress> RouteOutbound(const webrtc::SocketAddress & destination,
				std::span<const uint8_t> data) const;

		private:
			struct Range
			{
				webrtc::IPAddress address;
				int prefix;
			};

			// One client behind a relay source port. A claimed flow only attributes the
			// relay's datagrams; a routed one also takes the client's replies.
			struct Flow
			{
				webrtc::SocketAddress client;
				int64_t lastSeenMs;
				bool routed;
				// Another client the relay's latest header declared, which takes over once
				// the routed one is gone, since a relay that prefixes only the first
				// datagram of a flow will not declare it again.
				std::optional<webrtc::SocketAddress> successor;
				std::list<webrtc::SocketAddress>::iterator age;
			};

			ProxyRelays(std::vector<Range> ranges, Alive alive);

			bool IsRelay(const webrtc::SocketAddress & source) const;
			std::map<webrtc::SocketAddress, Flow>::iterator Insert(const webrtc::SocketAddress & relay,
				const webrtc::SocketAddress & client, int64_t lastSeenMs, bool routed);
			void Remove(std::map<webrtc::SocketAddress, Flow>::iterator flow);
			void Touch(Flow & flow, int64_t nowMs);
			void Expire(int64_t nowMs);
			std::optional<webrtc::SocketAddress> DeadRoute(int64_t nowMs);
			bool Admits(const webrtc::SocketAddress & relay, const webrtc::SocketAddress & client, int64_t nowMs,
				std::optional<webrtc::SocketAddress> & evicted);

			const std::vector<Range> ranges_;
			const Alive alive_;
			// By relay source address and port.
			std::map<webrtc::SocketAddress, Flow> flows_;
			// Routed flows by client.
			std::map<webrtc::SocketAddress, webrtc::SocketAddress> relayByClient_;
			// Relay sources, least recently seen first, apart so that claims never take
			// the room or the place of a route.
			std::list<webrtc::SocketAddress> routeAges_;
			std::list<webrtc::SocketAddress> claimAges_;
			int64_t fullUntilMs_ = 0;
	};
}

#endif
