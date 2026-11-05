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

#include "api/ProxyRelays.h"

#include "api/transport/stun.h"
#include "rtc_base/byte_buffer.h"

#include <algorithm>
#include <charconv>
#include <cstring>

namespace jni
{
	namespace
	{
		constexpr size_t kMaxRanges = 4096;
		constexpr size_t kMaxRoutes = 16384;
		constexpr size_t kMaxClaims = 4096;
		// Well past ICE consent checks.
		constexpr int64_t kRouteTimeoutMs = 120000;
		// Past any connectivity check of the claimed client.
		constexpr int64_t kClaimTimeoutMs = 30000;
		// Before a full table of live clients is looked through again.
		constexpr int64_t kFullRecheckMs = 1000;

		constexpr int kHeaderAbsent = -1;
		constexpr int kHeaderInvalid = -2;

		constexpr uint8_t kSignature[12] = { 0x0D, 0x0A, 0x0D, 0x0A, 0x00, 0x0D, 0x0A, 0x51, 0x55, 0x49, 0x54, 0x0A };

		bool IsStun(std::span<const uint8_t> data)
		{
			return data.size() >= 20 && (data[0] & 0xC0) == 0
				&& data[4] == 0x21 && data[5] == 0x12 && data[6] == 0xA4 && data[7] == 0x42;
		}

		bool IsStunResponse(std::span<const uint8_t> data)
		{
			if (!IsStun(data)) {
				return false;
			}

			int type = (data[0] << 8) | data[1];

			return webrtc::IsStunSuccessResponseType(type) || webrtc::IsStunErrorResponseType(type);
		}

		// A STUN server lookup carries no credentials, unlike an ICE check.
		bool IsStunLookup(std::span<const uint8_t> data)
		{
			if (!IsStun(data) || ((data[0] << 8) | data[1]) != webrtc::STUN_BINDING_REQUEST) {
				return false;
			}

			webrtc::StunMessage message;
			webrtc::ByteBufferReader reader(data);

			return message.Read(&reader)
				&& message.GetByteString(webrtc::STUN_ATTR_USERNAME) == nullptr
				&& message.GetByteString(webrtc::STUN_ATTR_MESSAGE_INTEGRITY) == nullptr;
		}

		uint16_t ReadPort(std::span<const uint8_t> data, size_t offset)
		{
			return static_cast<uint16_t>((data[offset] << 8) | data[offset + 1]);
		}

		// Returns the header length and the declared client, or kHeaderAbsent or kHeaderInvalid.
		int ReadHeader(std::span<const uint8_t> data, webrtc::SocketAddress & client)
		{
			if (data.size() < 16 || !std::equal(std::begin(kSignature), std::end(kSignature), data.begin())) {
				return kHeaderAbsent;
			}

			size_t total = 16 + ((static_cast<size_t>(data[14]) << 8) | data[15]);

			// A LOCAL command, such as a health check, declares no client.
			if (total > data.size() || data[12] != 0x21) {
				return kHeaderInvalid;
			}

			if (data[13] == 0x12 && total >= 16 + 12) {
				in_addr address;
				std::memcpy(&address, data.data() + 16, sizeof(address));
				client = webrtc::SocketAddress(webrtc::IPAddress(address), ReadPort(data, 24));
			}
			else if (data[13] == 0x22 && total >= 16 + 36) {
				in6_addr address;
				std::memcpy(&address, data.data() + 16, sizeof(address));
				// So that a dual-stack relay's IPv4 clients stay IPv4, as on every receive path.
				client = webrtc::SocketAddress(webrtc::IPAddress(address).Normalized(), ReadPort(data, 48));
			}
			else {
				return kHeaderInvalid;
			}

			return static_cast<int>(total);
		}
	}

	std::unique_ptr<ProxyRelays> ProxyRelays::Create(const std::vector<std::string> & relays, Alive alive,
		const ProxyRelays * previous)
	{
		if (relays.size() > kMaxRanges) {
			return nullptr;
		}

		std::vector<Range> ranges;

		for (const std::string & entry : relays) {
			size_t slash = entry.find('/');
			webrtc::IPAddress address;

			if (!webrtc::IPFromString(entry.substr(0, slash), &address)) {
				return nullptr;
			}

			int bits = address.family() == AF_INET ? 32 : 128;
			int prefix = bits;

			if (slash != std::string::npos) {
				const char * first = entry.data() + slash + 1;
				const char * last = entry.data() + entry.size();
				auto [end, error] = std::from_chars(first, last, prefix);

				if (first == last || error != std::errc() || end != last || prefix < 0 || prefix > bits) {
					return nullptr;
				}
			}

			ranges.push_back(Range { webrtc::TruncateIP(address, prefix), prefix });
		}

		std::unique_ptr<ProxyRelays> proxy(new ProxyRelays(std::move(ranges), std::move(alive)));

		// Oldest first, so that the age order carries over.
		if (previous != nullptr) {
			for (const auto * ages : { &previous->routeAges_, &previous->claimAges_ }) {
				for (const webrtc::SocketAddress & relay : *ages) {
					const Flow & flow = previous->flows_.at(relay);

					if (proxy->IsRelay(relay)) {
						proxy->Insert(relay, flow.client, flow.lastSeenMs, flow.routed)->second.successor = flow.successor;
					}
				}
			}
		}

		return proxy;
	}

	ProxyRelays::ProxyRelays(std::vector<Range> ranges, Alive alive) :
		ranges_(std::move(ranges)),
		alive_(std::move(alive))
	{
	}

	ProxyRelays::Receive ProxyRelays::OnReceive(std::span<const uint8_t> & data, webrtc::SocketAddress & source,
		webrtc::SocketAddress & relay, int64_t nowMs)
	{
		Expire(nowMs);

		webrtc::SocketAddress client;
		int header = ReadHeader(data, client);

		if (header == kHeaderInvalid) {
			return Receive::kDrop;
		}

		auto flow = flows_.find(source);

		if (header == kHeaderAbsent) {
			// Some relays only prefix the first datagram of a flow. Behind relays no peer
			// comes straight here; a STUN response stays, as it only matches a request
			// the endpoint sent, such as one to a STUN server.
			if (flow == flows_.end()) {
				return IsStunResponse(data) ? Receive::kPass : Receive::kDrop;
			}
		}
		else {
			// A header from anywhere else is forged. A flow implies a listed relay, since
			// a new list keeps only the flows of relays it lists.
			if (flow == flows_.end() && !IsRelay(source)) {
				return Receive::kDrop;
			}

			data = data.subspan(header);

			if (data.empty()) {
				return Receive::kDrop;
			}

			if (flow != flows_.end() && flow->second.routed) {
				if (flow->second.client != client) {
					// The routed client keeps the relay while it lives, see Admits.
					flow->second.successor = client;
					relay = source;
					source = client;
					return Receive::kClaimed;
				}
				flow->second.successor.reset();
			}
			else if (flow != flows_.end()) {
				flow->second.client = client;
			}
			else {
				if (claimAges_.size() >= kMaxClaims) {
					Remove(flows_.find(claimAges_.front()));
				}
				flow = Insert(source, client, nowMs, false);
			}
		}

		Touch(flow->second, nowMs);

		bool vacated = flow->second.routed && flow->second.successor && !alive_(flow->second.client);

		if (!flow->second.routed || vacated) {
			relay = source;
			source = vacated ? *flow->second.successor : flow->second.client;
			return Receive::kClaimed;
		}

		source = flow->second.client;
		return Receive::kPass;
	}

	bool ProxyRelays::Admits(const webrtc::SocketAddress & relay, const webrtc::SocketAddress & client, int64_t nowMs)
	{
		std::optional<webrtc::SocketAddress> evicted;

		return Admits(relay, client, nowMs, evicted);
	}

	bool ProxyRelays::Admits(const webrtc::SocketAddress & relay, const webrtc::SocketAddress & client, int64_t nowMs,
		std::optional<webrtc::SocketAddress> & evicted)
	{
		evicted.reset();

		if (!IsRelay(relay)) {
			return false;
		}

		// A relay port passes to another client only once the one it routes is gone.
		// ICE consent keeps a client alive, and that takes the client's own credentials.
		auto held = flows_.find(relay);
		bool routed = held != flows_.end() && held->second.routed;

		if (routed && held->second.client != client && alive_(held->second.client)) {
			return false;
		}

		// Taking the relay's route or moving the client's own leaves the table no fuller.
		if (routed || relayByClient_.contains(client) || routeAges_.size() < kMaxRoutes) {
			return true;
		}

		evicted = DeadRoute(nowMs);

		return evicted.has_value();
	}

	bool ProxyRelays::Commit(const webrtc::SocketAddress & relay, const webrtc::SocketAddress & client, int64_t nowMs)
	{
		std::optional<webrtc::SocketAddress> evicted;

		if (!Admits(relay, client, nowMs, evicted)) {
			return false;
		}

		auto held = flows_.find(relay);

		if (held != flows_.end() && held->second.routed && held->second.client == client) {
			Touch(held->second, nowMs);
			return true;
		}

		std::optional<webrtc::SocketAddress> previousRelay = RelayOf(client);

		if (held != flows_.end()) {
			Remove(held);
		}
		// A client has one relay, so it moves.
		for (const auto & other : { previousRelay, evicted }) {
			if (other) {
				auto flow = flows_.find(*other);

				if (flow != flows_.end()) {
					Remove(flow);
				}
			}
		}

		Insert(relay, client, nowMs, true);
		return true;
	}

	std::optional<webrtc::SocketAddress> ProxyRelays::RelayOf(const webrtc::SocketAddress & client) const
	{
		auto it = relayByClient_.find(client);

		if (it == relayByClient_.end()) {
			return std::nullopt;
		}

		return it->second;
	}

	std::optional<webrtc::SocketAddress> ProxyRelays::RouteOutbound(const webrtc::SocketAddress & destination,
		std::span<const uint8_t> data) const
	{
		if (std::optional<webrtc::SocketAddress> relay = RelayOf(destination)) {
			return relay;
		}

		if (IsStunLookup(data)) {
			return destination;
		}

		return std::nullopt;
	}

	bool ProxyRelays::IsRelay(const webrtc::SocketAddress & source) const
	{
		const webrtc::IPAddress & address = source.ipaddr();

		return std::any_of(ranges_.begin(), ranges_.end(), [&address](const Range & range) {
			return address.family() == range.address.family()
				&& webrtc::TruncateIP(address, range.prefix) == range.address;
		});
	}

	std::map<webrtc::SocketAddress, ProxyRelays::Flow>::iterator ProxyRelays::Insert(const webrtc::SocketAddress & relay,
		const webrtc::SocketAddress & client, int64_t lastSeenMs, bool routed)
	{
		auto & ages = routed ? routeAges_ : claimAges_;
		auto age = ages.insert(ages.end(), relay);
		auto flow = flows_.emplace(relay, Flow { client, lastSeenMs, routed, std::nullopt, age }).first;

		if (routed) {
			relayByClient_[client] = relay;
		}

		return flow;
	}

	void ProxyRelays::Remove(std::map<webrtc::SocketAddress, Flow>::iterator flow)
	{
		if (flow == flows_.end()) {
			return;
		}

		if (flow->second.routed) {
			auto route = relayByClient_.find(flow->second.client);

			if (route != relayByClient_.end() && route->second == flow->first) {
				relayByClient_.erase(route);
			}
		}

		(flow->second.routed ? routeAges_ : claimAges_).erase(flow->second.age);
		flows_.erase(flow);
	}

	void ProxyRelays::Touch(Flow & flow, int64_t nowMs)
	{
		auto & ages = flow.routed ? routeAges_ : claimAges_;

		flow.lastSeenMs = nowMs;
		ages.splice(ages.end(), ages, flow.age);
	}

	void ProxyRelays::Expire(int64_t nowMs)
	{
		while (!routeAges_.empty() && flows_.at(routeAges_.front()).lastSeenMs + kRouteTimeoutMs < nowMs) {
			Remove(flows_.find(routeAges_.front()));
		}
		while (!claimAges_.empty() && flows_.at(claimAges_.front()).lastSeenMs + kClaimTimeoutMs < nowMs) {
			Remove(flows_.find(claimAges_.front()));
		}
	}

	std::optional<webrtc::SocketAddress> ProxyRelays::DeadRoute(int64_t nowMs)
	{
		// Refreshing keeps the routes of clients that are gone, since a relay that
		// prefixes only the first datagram of a flow names a successor once. A full
		// table alone gives them up.
		if (nowMs < fullUntilMs_) {
			return std::nullopt;
		}

		for (const webrtc::SocketAddress & relay : routeAges_) {
			if (!alive_(flows_.at(relay).client)) {
				return relay;
			}
		}

		// So that a flood of joins does not look through every route each.
		fullUntilMs_ = nowMs + kFullRecheckMs;

		return std::nullopt;
	}
}
