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

#include "api/IceUdpEndpoint.h"
#include "Exception.h"

#include "api/environment/environment_factory.h"
#include "api/units/time_delta.h"
#include "rtc_base/async_udp_socket.h"
#include "rtc_base/base64.h"
#include "rtc_base/byte_buffer.h"
#include "rtc_base/crypto_random.h"
#include "rtc_base/logging.h"
#include "rtc_base/message_digest.h"
#include "rtc_base/time_utils.h"
#include "rtc_base/zero_memory.h"

#include <algorithm>

namespace jni
{
	namespace
	{
		// A client keeps few addresses per connection: host, server reflexive and
		// relayed candidates, per address family.
		constexpr size_t kMaxRoutesPerPeer = 32;
		constexpr size_t kMaxTransactions = 65536;
		// Longer than a STUN transaction with all its retransmissions.
		constexpr int64_t kTransactionLifetimeMs = 40000;
		// ICE consent expires after 30 seconds without a response. A route that
		// received nothing for that long belongs to a dead connection, and another
		// peer may take its address over.
		constexpr int64_t kRouteIdleMs = 30000;
		constexpr size_t kStunHeaderSize = 20;
		// Binding requests are a few hundred bytes; larger ones are not held.
		constexpr size_t kMaxHeldPacketSize = 2048;
		constexpr size_t kDerivedPasswordBytes = 24;
		// As libjuice keeps a STUN binding alive: refreshed every 15 seconds, sent up to
		// five times, 500 ms apart at first and doubling, within the refresh.
		constexpr int64_t kMonitorPeriodMs = 15000;
		constexpr int64_t kMonitorRetransmitMs = 500;
		constexpr int kMonitorSends = 5;

		bool IsStun(std::span<const uint8_t> data)
		{
			return data.size() >= kStunHeaderSize && (data[0] & 0xC0) == 0
				&& data[4] == 0x21 && data[5] == 0x12 && data[6] == 0xA4 && data[7] == 0x42;
		}

		int StunType(std::span<const uint8_t> data)
		{
			return (data[0] << 8) | data[1];
		}

		std::string TransactionId(std::span<const uint8_t> data)
		{
			return std::string(reinterpret_cast<const char *>(data.data() + 8), webrtc::kStunTransactionIdLength);
		}
	}

	std::shared_ptr<IceUdpEndpoint> IceUdpEndpoint::Create(uint16_t port, std::vector<webrtc::IPAddress> addresses)
	{
		return std::shared_ptr<IceUdpEndpoint>(new IceUdpEndpoint(port, std::move(addresses)));
	}

	IceUdpEndpoint::IceUdpEndpoint(uint16_t port, std::vector<webrtc::IPAddress> addresses) :
		port_(port),
		addresses_(std::move(addresses)),
		env_(webrtc::CreateEnvironment()),
		thread_(webrtc::Thread::CreateWithSocketServer())
	{
		thread_->SetName("webrtc_jni_ice_udp_endpoint", nullptr);

		if (!thread_->Start()) {
			throw Exception("Start ICE UDP endpoint thread failed");
		}
	}

	IceUdpEndpoint::~IceUdpEndpoint()
	{
		// Tasks capture this without a reference, so the thread stops before the state
		// goes. Nobody holds the endpoint any more, so nothing posts after this.
		thread_->BlockingCall([this] {
			closed_ = true;
			osSockets_.clear();
			routes_.clear();
			sockets_.clear();
			sessionsByUfrag_.clear();
			sessions_.clear();
			peers_.clear();
			transactions_.clear();
			transactionOrder_.clear();
			pending_.clear();
			pendingByKey_.clear();
			pendingByUfrag_.clear();
			handler_ = nullptr;
			ClearDerivedPasswords();
			proxy_.reset();
			monitors_.clear();
			monitorTransactions_.clear();
		});
		thread_->Stop();
	}

	bool IceUdpEndpoint::Allows(const webrtc::IPAddress & address) const
	{
		return addresses_.empty() || std::find(addresses_.begin(), addresses_.end(), address) != addresses_.end();
	}

	void IceUdpEndpoint::Close()
	{
		thread_->BlockingCall([this] {
			closed_ = true;
			osSockets_.clear();
			routes_.clear();
			transactions_.clear();
			transactionOrder_.clear();
			pending_.clear();
			pendingByKey_.clear();
			pendingByUfrag_.clear();
			handler_ = nullptr;
			ClearDerivedPasswords();
			proxy_.reset();
			monitors_.clear();
			monitorTransactions_.clear();

			for (auto & [id, entry] : sockets_) {
				entry.routes.clear();
			}
			for (auto & [id, peer] : peers_) {
				peer.routes = 0;
			}
		});
	}

	void IceUdpEndpoint::Inject(const webrtc::SocketAddress & local, const webrtc::SocketAddress & remote,
		std::vector<uint8_t> data)
	{
		thread_->PostTask([this, local, remote, data = std::move(data)] {
			if (!closed_) {
				Receive(local, data, remote, std::nullopt);
			}
		});
	}

	uint64_t IceUdpEndpoint::AddPeer()
	{
		uint64_t id = nextId_++;

		thread_->PostTask([this, id] {
			peers_.emplace(id, PeerEntry());
		});

		return id;
	}

	void IceUdpEndpoint::RemovePeer(uint64_t peer)
	{
		thread_->PostTask([this, peer] {
			auto it = peers_.find(peer);

			if (it == peers_.end()) {
				return;
			}

			std::set<uint64_t> sessions = it->second.sessions;

			for (uint64_t session : sessions) {
				ForgetSession(session);
			}

			// Sockets without a session, if any, still hold routes of the peer.
			for (auto socket = sockets_.begin(); socket != sockets_.end();) {
				uint64_t id = socket->first;
				bool owned = socket->second.peer == peer;

				++socket;

				if (owned) {
					ForgetSocket(id);
				}
			}

			peers_.erase(peer);
		});
	}

	uint64_t IceUdpEndpoint::AddSession(uint64_t peer)
	{
		uint64_t id = nextId_++;

		thread_->PostTask([this, id, peer] {
			auto it = peers_.find(peer);

			if (it == peers_.end()) {
				return;
			}

			sessions_.emplace(id, SessionEntry { peer, {}, {}, {} });
			it->second.sessions.insert(id);
		});

		return id;
	}

	void IceUdpEndpoint::SetCredentials(uint64_t session, std::string ufrag, std::string password)
	{
		thread_->PostTask([this, session, ufrag = std::move(ufrag), password = std::move(password)] {
			auto it = sessions_.find(session);

			if (it == sessions_.end()) {
				return;
			}

			auto [first, last] = sessionsByUfrag_.equal_range(it->second.ufrag);

			for (auto index = first; index != last; ++index) {
				if (index->second == session) {
					sessionsByUfrag_.erase(index);
					break;
				}
			}

			it->second.ufrag = ufrag;
			it->second.password = password;
			sessionsByUfrag_.emplace(ufrag, session);
		});
	}

	void IceUdpEndpoint::RemoveSession(uint64_t session)
	{
		thread_->PostTask([this, session] {
			ForgetSession(session);
		});
	}

	std::optional<webrtc::SocketAddress> IceUdpEndpoint::OpenSocket(const webrtc::IPAddress & address)
	{
		if (!Allows(address)) {
			return std::nullopt;
		}

		return thread_->BlockingCall([this, &address] {
			return OpenSocketHere(address);
		});
	}

	std::optional<webrtc::SocketAddress> IceUdpEndpoint::OpenSocketHere(const webrtc::IPAddress & address)
	{
		if (closed_) {
			return std::nullopt;
		}

		webrtc::SocketAddress local(address, port_);

		if (osSockets_.find(local) != osSockets_.end()) {
			return local;
		}

		std::unique_ptr<webrtc::AsyncUDPSocket> socket = webrtc::AsyncUDPSocket::Create(env_, local, *thread_->socketserver());

		if (!socket) {
			RTC_LOG(LS_WARNING) << "ICE UDP endpoint cannot bind " << local.ToString();
			return std::nullopt;
		}

		socket->RegisterReceivedPacketCallback([this, local](webrtc::AsyncPacketSocket *, const webrtc::ReceivedIpPacket & packet) {
			Receive(local, packet.payload(), packet.source_address(), packet.arrival_time());
		});

		osSockets_.emplace(local, std::move(socket));

		return local;
	}

	uint64_t IceUdpEndpoint::AddVirtualSocket(uint64_t peer, uint64_t session, const webrtc::SocketAddress & local,
		std::shared_ptr<EndpointLink> link)
	{
		uint64_t id = nextId_++;

		thread_->PostTask([this, id, peer, session, local, link = std::move(link)] {
			if (peers_.find(peer) == peers_.end()) {
				return;
			}

			auto owner = sessions_.find(session);

			// Without its session the socket cannot be authenticated against, but still
			// receives on the routes its own traffic sets up.
			sockets_.emplace(id, SocketEntry { peer, owner != sessions_.end() ? session : 0, local, link, {} });

			if (owner != sessions_.end()) {
				owner->second.sockets.push_back(id);
			}
		});

		return id;
	}

	void IceUdpEndpoint::RemoveVirtualSocket(uint64_t socket)
	{
		thread_->PostTask([this, socket] {
			ForgetSocket(socket);
		});
	}

	void IceUdpEndpoint::Send(uint64_t socket, const webrtc::SocketAddress & local, const webrtc::SocketAddress & remote,
		std::vector<uint8_t> data)
	{
		thread_->PostTask([this, socket, local, remote, data = std::move(data)] {
			if (closed_) {
				return;
			}

			auto it = osSockets_.find(local);

			if (it == osSockets_.end()) {
				return;
			}

			webrtc::SocketAddress target = remote;

			if (proxy_) {
				// Behind relays a client is reached only through the relay that routes it.
				std::optional<webrtc::SocketAddress> routed = proxy_->RouteOutbound(remote, data);

				if (!routed) {
					return;
				}
				target = *routed;
			}

			int64_t now = webrtc::TimeMillis();

			if (IsStun(data) && webrtc::IsStunRequestType(StunType(data))) {
				// A request goes to a STUN server as often as to a peer, and several peers
				// may ask the same server, so its response is routed by transaction.
				RecordTransaction(socket, data, now);
			}
			else if (routes_.find(RouteKey(local, remote)) == routes_.end()) {
				// The port only sends to addresses it has a connection with. Its traffic
				// opens a route but never takes one over: only an authenticated binding
				// request moves a route.
				MapRoute(socket, RouteKey(local, remote), now);
			}

			it->second->SendTo(data.data(), data.size(), target, webrtc::AsyncSocketPacketOptions());
		});
	}

	void IceUdpEndpoint::Receive(const webrtc::SocketAddress & local, std::span<const uint8_t> data,
		const webrtc::SocketAddress & from, std::optional<webrtc::Timestamp> arrival)
	{
		if (!proxy_) {
			Route(local, data, from, arrival, std::nullopt);
			return;
		}

		webrtc::SocketAddress client = from;
		webrtc::SocketAddress relay;

		switch (proxy_->OnReceive(data, client, relay, webrtc::TimeMillis())) {
			case ProxyRelays::Receive::kDrop:
				return;
			case ProxyRelays::Receive::kPass:
				Route(local, data, client, arrival, std::nullopt);
				return;
			case ProxyRelays::Receive::kClaimed:
				Route(local, data, client, arrival, relay);
				return;
		}
	}

	void IceUdpEndpoint::Route(const webrtc::SocketAddress & local, std::span<const uint8_t> data,
		const webrtc::SocketAddress & from, std::optional<webrtc::Timestamp> arrival,
		const std::optional<webrtc::SocketAddress> & relay)
	{
		RouteKey key(local, from);
		int64_t now = webrtc::TimeMillis();

		if (IsStun(data)) {
			int type = StunType(data);

			if (webrtc::IsStunSuccessResponseType(type) || webrtc::IsStunErrorResponseType(type)) {
				if (OnMonitorResponse(data, from, now)) {
					return;
				}

				// A response belongs to the socket that sent the request. During an ICE
				// restart that is not the socket the route names.
				ExpireTransactions(now);

				auto transaction = transactions_.find(TransactionId(data));

				if (transaction != transactions_.end()) {
					Deliver(transaction->second.socket, data, from, arrival);
					return;
				}
			}
			else if (type == webrtc::STUN_BINDING_REQUEST) {
				RouteBindingRequest(key, data, arrival, now, relay);
				return;
			}
		}

		auto route = routes_.find(key);

		if (route != routes_.end()) {
			route->second.lastReceiveMs = now;
			Deliver(route->second.socket, data, from, arrival);
		}
	}

	void IceUdpEndpoint::RouteBindingRequest(const RouteKey & key, std::span<const uint8_t> data,
		std::optional<webrtc::Timestamp> arrival, int64_t nowMs, const std::optional<webrtc::SocketAddress> & relay)
	{
		webrtc::StunMessage message;
		webrtc::ByteBufferReader reader(data);

		if (!message.Read(&reader)) {
			return;
		}

		const webrtc::StunByteStringAttribute * username = message.GetByteString(webrtc::STUN_ATTR_USERNAME);

		if (username == nullptr) {
			return;
		}

		// USERNAME is "<local ufrag>:<remote ufrag>".
		absl::string_view name = username->string_view();
		size_t colon = name.find(':');

		if (colon == absl::string_view::npos || colon == 0) {
			return;
		}

		absl::string_view ufrag = name.substr(0, colon);
		auto route = routes_.find(key);

		if (route != routes_.end()) {
			auto socket = sockets_.find(route->second.socket);
			auto session = socket != sockets_.end() ? sessions_.find(socket->second.session) : sessions_.end();

			// The port checks the integrity itself, as it does on a port of its own.
			if (session != sessions_.end() && session->second.ufrag == ufrag) {
				// A relay new to a routed client routes it once the client's credentials
				// sign a datagram the relay brought, such as a consent check. The datagram
				// is passed on either way.
				if (relay && IntegrityCheck(message).Matches(session->second.password)) {
					proxy_->Commit(*relay, key.second, nowMs);
				}

				route->second.lastReceiveMs = nowMs;
				Deliver(route->second.socket, data, key.second, arrival);
				return;
			}
		}

		IntegrityCheck integrity(message);

		if (std::optional<uint64_t> session = Authenticate(integrity, ufrag)) {
			// A relay that could not route the client once a session takes it is refused
			// now.
			if (relay && !proxy_->Admits(*relay, key.second, nowMs)) {
				return;
			}

			std::optional<uint64_t> socket = SocketOf(sessions_.at(*session), key.first);

			if (!socket || !sockets_.at(*socket).ready) {
				// The session has no port here that could take it yet; it gets the request
				// once it has.
				Hold(key, name, ufrag, data, arrival, nowMs, false, relay);
				return;
			}

			auto held = pendingByKey_.find(PendingKey(key.first, key.second, std::string(name)));

			if (held != pendingByKey_.end()) {
				TakePending(held->second);
			}
			if ((!relay || proxy_->Commit(*relay, key.second, nowMs)) && Claim(*socket, key, nowMs)) {
				Deliver(*socket, data, key.second, arrival);
			}
			return;
		}

		if (!handler_ || (relay && !proxy_->Admits(*relay, key.second, nowMs))) {
			return;
		}
		if (derivedKeys_.empty() || VerifyDerived(integrity, ufrag)) {
			Hold(key, name, ufrag, data, arrival, nowMs, true, relay);
		}
	}

	bool IceUdpEndpoint::ClientAlive(const webrtc::SocketAddress & client) const
	{
		// A route that still receives belongs to a connection with ICE consent, which
		// only the client's own credentials renew.
		int64_t now = webrtc::TimeMillis();

		for (const auto & [local, socket] : osSockets_) {
			auto route = routes_.find(RouteKey(local, client));

			if (route != routes_.end() && now - route->second.lastReceiveMs < kRouteIdleMs) {
				return true;
			}
		}

		return false;
	}

	bool IceUdpEndpoint::SetProxyRelays(std::vector<std::string> relays)
	{
		return thread_->BlockingCall([this, &relays] {
			if (relays.empty()) {
				proxy_.reset();
				return true;
			}

			std::unique_ptr<ProxyRelays> proxy = ProxyRelays::Create(relays,
				[this](const webrtc::SocketAddress & client) { return ClientAlive(client); }, proxy_.get());

			if (!proxy) {
				return false;
			}

			proxy_ = std::move(proxy);
			return true;
		});
	}

	std::optional<webrtc::SocketAddress> IceUdpEndpoint::ProxyRelayOf(const webrtc::SocketAddress & client)
	{
		return thread_->BlockingCall([this, &client]() -> std::optional<webrtc::SocketAddress> {
			if (!proxy_) {
				return std::nullopt;
			}
			if (std::optional<webrtc::SocketAddress> relay = proxy_->RelayOf(client)) {
				return relay;
			}

			// A held request names its relay too, though the relay routes the client only
			// once a session takes it.
			for (const auto & [id, request] : pending_) {
				if (request.relay && request.remote == client) {
					return request.relay;
				}
			}

			return std::nullopt;
		});
	}

	bool IceUdpEndpoint::Claim(uint64_t socket, const RouteKey & key, int64_t nowMs)
	{
		auto route = routes_.find(key);

		if (route != routes_.end() && route->second.socket != socket) {
			auto previous = sockets_.find(route->second.socket);
			bool samePeer = previous != sockets_.end() && previous->second.peer == sockets_.at(socket).peer;

			// A new ICE session of the same peer takes its addresses along. Another peer
			// waits until the route is dead, so that a forged source address cannot cut
			// a live connection off.
			if (!samePeer && nowMs - route->second.lastReceiveMs < kRouteIdleMs) {
				return false;
			}
		}

		MapRoute(socket, key, nowMs);
		return true;
	}

	bool IceUdpEndpoint::IntegrityCheck::Matches(const std::string & password)
	{
		auto status = validated_
			? message_.RevalidateMessageIntegrity(password)
			: message_.ValidateMessageIntegrity(password);
		validated_ = true;

		return status == webrtc::StunMessage::IntegrityStatus::kIntegrityOk;
	}

	std::optional<uint64_t> IceUdpEndpoint::Authenticate(IntegrityCheck & integrity, absl::string_view ufrag)
	{
		auto [first, last] = sessionsByUfrag_.equal_range(ufrag);

		// Generated ufrags are short, so several sessions can share one; only the
		// password tells them apart.
		for (auto index = first; index != last; ++index) {
			if (integrity.Matches(sessions_.at(index->second).password)) {
				return index->second;
			}
		}

		return std::nullopt;
	}

	std::optional<uint64_t> IceUdpEndpoint::SocketOf(const SessionEntry & session, const webrtc::SocketAddress & local) const
	{
		// A session has one socket per local address; a newer one replaces an older.
		for (auto socket = session.sockets.rbegin(); socket != session.sockets.rend(); ++socket) {
			if (sockets_.at(*socket).local == local) {
				return *socket;
			}
		}

		return std::nullopt;
	}

	void IceUdpEndpoint::Deliver(uint64_t socket, std::span<const uint8_t> data, const webrtc::SocketAddress & from,
		std::optional<webrtc::Timestamp> arrival)
	{
		auto it = sockets_.find(socket);

		if (it == sockets_.end()) {
			return;
		}

		const std::shared_ptr<EndpointLink> & link = it->second.link;
		std::lock_guard<std::mutex> lock(link->mutex);

		if (link->thread == nullptr) {
			return;
		}

		// The task runs on the socket's thread, which also clears the receiver.
		link->thread->PostTask([link, bytes = std::vector<uint8_t>(data.begin(), data.end()), from, arrival] {
			if (link->receiver != nullptr) {
				link->receiver->Receive(bytes, from, arrival);
			}
		});
	}

	void IceUdpEndpoint::MapRoute(uint64_t socket, const RouteKey & key, int64_t nowMs)
	{
		auto entry = sockets_.find(socket);

		if (entry == sockets_.end()) {
			return;
		}

		auto existing = routes_.find(key);

		if (existing != routes_.end()) {
			if (existing->second.socket == socket) {
				existing->second.lastReceiveMs = nowMs;
				return;
			}
			UnmapRoute(existing);
		}

		auto peer = peers_.find(entry->second.peer);

		if (peer == peers_.end() || peer->second.routes >= kMaxRoutesPerPeer) {
			return;
		}

		routes_.emplace(key, RouteEntry { socket, nowMs });
		entry->second.routes.insert(key);
		peer->second.routes++;
	}

	void IceUdpEndpoint::UnmapRoute(std::map<RouteKey, RouteEntry>::iterator route)
	{
		auto socket = sockets_.find(route->second.socket);

		if (socket != sockets_.end()) {
			socket->second.routes.erase(route->first);

			auto peer = peers_.find(socket->second.peer);

			if (peer != peers_.end() && peer->second.routes > 0) {
				peer->second.routes--;
			}
		}

		routes_.erase(route);
	}

	void IceUdpEndpoint::RecordTransaction(uint64_t socket, std::span<const uint8_t> data, int64_t nowMs)
	{
		ExpireTransactions(nowMs);

		while (transactions_.size() >= kMaxTransactions && !transactionOrder_.empty()) {
			transactions_.erase(transactionOrder_.front().second);
			transactionOrder_.pop_front();
		}

		std::string id = TransactionId(data);
		int64_t expires = nowMs + kTransactionLifetimeMs;

		transactions_[id] = Transaction { socket, expires };
		transactionOrder_.emplace_back(expires, std::move(id));
	}

	void IceUdpEndpoint::ExpireTransactions(int64_t nowMs)
	{
		while (!transactionOrder_.empty() && transactionOrder_.front().first <= nowMs) {
			auto it = transactions_.find(transactionOrder_.front().second);

			// A retransmission renewed the entry; its newer order entry expires it.
			if (it != transactions_.end() && it->second.expiresMs == transactionOrder_.front().first) {
				transactions_.erase(it);
			}
			transactionOrder_.pop_front();
		}
	}

	void IceUdpEndpoint::ForgetSession(uint64_t session)
	{
		auto entry = sessions_.find(session);

		if (entry == sessions_.end()) {
			return;
		}

		auto [first, last] = sessionsByUfrag_.equal_range(entry->second.ufrag);

		for (auto index = first; index != last; ++index) {
			if (index->second == session) {
				sessionsByUfrag_.erase(index);
				break;
			}
		}

		auto peer = peers_.find(entry->second.peer);

		if (peer != peers_.end()) {
			peer->second.sessions.erase(session);
		}

		// The session's sockets go with it.
		std::vector<uint64_t> sockets = entry->second.sockets;

		for (uint64_t socket : sockets) {
			ForgetSocket(socket);
		}

		sessions_.erase(session);
	}

	void IceUdpEndpoint::ForgetSocket(uint64_t socket)
	{
		auto entry = sockets_.find(socket);

		if (entry == sockets_.end()) {
			return;
		}

		auto peer = peers_.find(entry->second.peer);

		for (const RouteKey & key : entry->second.routes) {
			routes_.erase(key);
		}
		if (peer != peers_.end()) {
			peer->second.routes -= std::min(peer->second.routes, entry->second.routes.size());
		}

		auto session = sessions_.find(entry->second.session);

		if (session != sessions_.end()) {
			auto & sockets = session->second.sockets;
			sockets.erase(std::remove(sockets.begin(), sockets.end(), socket), sockets.end());
		}

		// Transactions of the socket stay until they expire; a late response finds the
		// socket gone and is dropped.
		sockets_.erase(entry);
	}

	void IceUdpEndpoint::SessionReady(uint64_t session, const webrtc::IPAddress & address)
	{
		thread_->PostTask([this, session, address] {
			auto entry = sessions_.find(session);

			if (entry == sessions_.end()) {
				return;
			}

			webrtc::SocketAddress local(address, port_);

			for (uint64_t socket : entry->second.sockets) {
				auto it = sockets_.find(socket);

				if (it != sockets_.end() && it->second.local == local) {
					it->second.ready = true;
				}
			}

			Replay(session, local, webrtc::TimeMillis());
		});
	}

	void IceUdpEndpoint::SetPendingLimits(size_t maxPending, int64_t timeoutMs)
	{
		thread_->PostTask([this, maxPending, timeoutMs] {
			maxPending_ = maxPending;
			pendingTimeoutMs_ = timeoutMs;
		});
	}

	void IceUdpEndpoint::SetPendingRequestHandler(PendingRequestHandler handler)
	{
		thread_->BlockingCall([this, &handler] {
			std::swap(handler_, handler);
		});
	}

	void IceUdpEndpoint::SetDerivedPasswords(std::string context, std::vector<std::pair<std::string, std::string>> keys)
	{
		thread_->BlockingCall([this, &context, &keys] {
			ClearDerivedPasswords();

			derivedContext_ = std::move(context);
			derivedKeys_ = std::move(keys);
		});
	}

	void IceUdpEndpoint::Reject(uint64_t request)
	{
		thread_->PostTask([this, request] {
			TakePending(request);
		});
	}

	bool IceUdpEndpoint::VerifyDerived(IntegrityCheck & integrity, absl::string_view ufrag) const
	{
		for (const auto & [prefix, key] : derivedKeys_) {
			if (ufrag.substr(0, prefix.size()) != prefix) {
				continue;
			}

			std::string input = derivedContext_;
			input.append(ufrag.data(), ufrag.size());

			uint8_t mac[32];

			if (webrtc::ComputeHmac(webrtc::DIGEST_SHA_256, key.data(), key.size(), input.data(), input.size(),
					mac, sizeof(mac)) != sizeof(mac)) {
				return false;
			}

			// 24 bytes encode to 32 characters without padding.
			std::string password = webrtc::Base64Encode(std::span<const uint8_t>(mac, kDerivedPasswordBytes));
			bool matches = integrity.Matches(password);

			webrtc::ExplicitZeroMemory(mac, sizeof(mac));
			webrtc::ExplicitZeroMemory(password.data(), password.size());
			webrtc::ExplicitZeroMemory(input.data(), input.size());

			return matches;
		}

		return false;
	}

	void IceUdpEndpoint::Hold(const RouteKey & key, absl::string_view username, absl::string_view ufrag,
		std::span<const uint8_t> data, std::optional<webrtc::Timestamp> arrival, int64_t nowMs, bool announce,
		const std::optional<webrtc::SocketAddress> & relay)
	{
		if (data.size() > kMaxHeldPacketSize) {
			return;
		}

		PendingKey pendingKey(key.first, key.second, std::string(username));
		auto existing = pendingByKey_.find(pendingKey);

		if (existing != pendingByKey_.end()) {
			// A retransmission. The newest packet answers a transaction the client still
			// waits for; the request keeps its deadline and is not announced again.
			PendingRequest & request = pending_.at(existing->second);
			request.packet.assign(data.begin(), data.end());
			request.arrival = arrival;
			if (relay) {
				request.relay = relay;
			}
			return;
		}

		if (pending_.size() >= maxPending_) {
			return;
		}

		uint64_t id = nextId_++;

		pending_.emplace(id, PendingRequest { key.first, key.second, std::string(username), std::string(ufrag),
			std::vector<uint8_t>(data.begin(), data.end()), arrival, nowMs + pendingTimeoutMs_, announce, relay });
		pendingByKey_.emplace(std::move(pendingKey), id);
		pendingByUfrag_.emplace(std::string(ufrag), id);

		if (!sweepScheduled_) {
			sweepScheduled_ = true;
			thread_->PostDelayedTask([this] { SweepPending(); }, webrtc::TimeDelta::Seconds(1));
		}

		if (announce) {
			absl::string_view remoteUfrag = username.substr(ufrag.size() + 1);
			// A copy, since the handler may replace itself while it runs.
			PendingRequestHandler handler = handler_;

			handler(PendingRequestInfo { id, key.first, key.second, std::string(ufrag), std::string(remoteUfrag) });
		}
	}

	void IceUdpEndpoint::Replay(uint64_t sessionId, const webrtc::SocketAddress & local, int64_t nowMs)
	{
		const SessionEntry & session = sessions_.at(sessionId);
		std::vector<uint64_t> requests;
		auto [first, last] = pendingByUfrag_.equal_range(session.ufrag);

		for (auto index = first; index != last; ++index) {
			if (pending_.at(index->second).local == local) {
				requests.push_back(index->second);
			}
		}

		for (uint64_t id : requests) {
			const PendingRequest & request = pending_.at(id);
			webrtc::StunMessage message;
			webrtc::ByteBufferReader reader(request.packet);
			IntegrityCheck integrity(message);
			bool verified = message.Read(&reader) && integrity.Matches(session.password);

			// An announced request is verified once: if it fails, its sender has to retry.
			// Another session may share the ufrag of one held for a port that was not
			// ready, so that one waits for its own session.
			if (!verified && !request.announced) {
				continue;
			}

			std::optional<PendingRequest> taken = TakePending(id);

			if (!verified) {
				continue;
			}

			std::optional<uint64_t> socket = SocketOf(session, local);

			if (!socket || !sockets_.at(*socket).ready) {
				continue;
			}
			// A request a relay brought needs that relay to route the client, and the
			// relay to be listed still.
			if (taken->relay && (!proxy_ || !proxy_->Commit(*taken->relay, taken->remote, nowMs))) {
				continue;
			}
			if (Claim(*socket, RouteKey(local, taken->remote), nowMs)) {
				Deliver(*socket, taken->packet, taken->remote, taken->arrival);
			}
		}
	}

	std::optional<IceUdpEndpoint::PendingRequest> IceUdpEndpoint::TakePending(uint64_t request)
	{
		auto it = pending_.find(request);

		if (it == pending_.end()) {
			return std::nullopt;
		}

		PendingRequest taken = std::move(it->second);
		pending_.erase(it);
		pendingByKey_.erase(PendingKey(taken.local, taken.remote, taken.username));

		auto [first, last] = pendingByUfrag_.equal_range(taken.localUfrag);

		for (auto index = first; index != last; ++index) {
			if (index->second == request) {
				pendingByUfrag_.erase(index);
				break;
			}
		}

		return taken;
	}

	void IceUdpEndpoint::SweepPending()
	{
		sweepScheduled_ = false;

		int64_t now = webrtc::TimeMillis();

		for (auto it = pending_.begin(); it != pending_.end();) {
			uint64_t id = it->first;
			bool expired = it->second.expiresMs <= now;

			++it;

			if (expired) {
				TakePending(id);
			}
		}

		if (!pending_.empty()) {
			sweepScheduled_ = true;
			thread_->PostDelayedTask([this] { SweepPending(); }, webrtc::TimeDelta::Seconds(1));
		}
	}

	uint64_t IceUdpEndpoint::OpenStunMonitor(const webrtc::IPAddress & local, const webrtc::SocketAddress & server)
	{
		if (!Allows(local)) {
			return 0;
		}

		return thread_->BlockingCall([this, &local, &server]() -> uint64_t {
			std::optional<webrtc::SocketAddress> socket = OpenSocketHere(local);

			if (!socket) {
				return 0;
			}

			uint64_t id = nextId_++;
			StunMonitor monitor;
			monitor.local = *socket;
			monitor.server = server;

			monitors_.emplace(id, std::move(monitor));
			StartMonitorTransaction(id);

			return id;
		});
	}

	std::optional<IceUdpEndpoint::StunReading> IceUdpEndpoint::ReadStunMonitor(uint64_t monitor)
	{
		return thread_->BlockingCall([this, monitor]() -> std::optional<StunReading> {
			auto it = monitors_.find(monitor);

			if (it == monitors_.end()) {
				return std::nullopt;
			}

			const StunMonitor & m = it->second;
			std::optional<int64_t> age;

			if (m.lastSuccessMs) {
				age = webrtc::TimeMillis() - *m.lastSuccessMs;
			}

			return StunReading { m.server, m.local, m.mapped, m.state, m.successes, m.failures, m.revision, age };
		});
	}

	void IceUdpEndpoint::CloseStunMonitor(uint64_t monitor)
	{
		thread_->PostTask([this, monitor] {
			auto it = monitors_.find(monitor);

			if (it != monitors_.end()) {
				monitorTransactions_.erase(it->second.transaction);
				monitors_.erase(it);
			}
		});
	}

	void IceUdpEndpoint::StartMonitorTransaction(uint64_t monitor)
	{
		auto it = monitors_.find(monitor);

		if (it == monitors_.end() || closed_) {
			return;
		}

		StunMonitor & m = it->second;

		// A transaction has until the next refresh to be answered.
		if (!m.answered) {
			m.failures++;
			m.state = StunReading::State::kFailed;
		}

		monitorTransactions_.erase(m.transaction);
		m.transaction = webrtc::CreateRandomString(webrtc::kStunTransactionIdLength);
		m.answered = false;
		monitorTransactions_.emplace(m.transaction, monitor);

		SendMonitorRequest(monitor, m.transaction, 0);

		thread_->PostDelayedTask([this, monitor] { StartMonitorTransaction(monitor); },
			webrtc::TimeDelta::Millis(kMonitorPeriodMs));
	}

	void IceUdpEndpoint::SendMonitorRequest(uint64_t monitor, const std::string & transaction, int attempt)
	{
		auto it = monitors_.find(monitor);

		if (it == monitors_.end() || closed_ || it->second.transaction != transaction || it->second.answered) {
			return;
		}

		auto socket = osSockets_.find(it->second.local);

		if (socket == osSockets_.end()) {
			return;
		}

		// No credentials, so that relays and the server take it for the lookup it is.
		webrtc::StunMessage request(webrtc::STUN_BINDING_REQUEST, transaction);
		webrtc::ByteBufferWriter buffer;
		request.Write(&buffer);

		socket->second->SendTo(buffer.Data(), buffer.Length(), it->second.server, webrtc::AsyncSocketPacketOptions());

		if (attempt + 1 < kMonitorSends) {
			thread_->PostDelayedTask([this, monitor, transaction, attempt] {
				SendMonitorRequest(monitor, transaction, attempt + 1);
			}, webrtc::TimeDelta::Millis(kMonitorRetransmitMs << attempt));
		}
	}

	bool IceUdpEndpoint::OnMonitorResponse(std::span<const uint8_t> data, const webrtc::SocketAddress & from, int64_t nowMs)
	{
		auto transaction = monitorTransactions_.find(TransactionId(data));

		if (transaction == monitorTransactions_.end()) {
			return false;
		}

		StunMonitor & m = monitors_.at(transaction->second);
		webrtc::StunMessage response;
		webrtc::ByteBufferReader reader(data);

		if (from != m.server || m.answered || !response.Read(&reader)) {
			return true;
		}

		m.answered = true;

		const webrtc::StunAddressAttribute * address = nullptr;

		if (response.type() == webrtc::STUN_BINDING_RESPONSE) {
			address = response.GetAddress(webrtc::STUN_ATTR_XOR_MAPPED_ADDRESS);

			if (address == nullptr) {
				address = response.GetAddress(webrtc::STUN_ATTR_MAPPED_ADDRESS);
			}
		}

		if (address == nullptr) {
			m.failures++;
			m.state = StunReading::State::kFailed;
			return true;
		}

		if (m.mapped != address->GetAddress()) {
			m.mapped = address->GetAddress();
			m.revision++;
		}

		m.successes++;
		m.lastSuccessMs = nowMs;
		m.state = StunReading::State::kSucceeded;

		return true;
	}

	void IceUdpEndpoint::ClearDerivedPasswords()
	{
		for (auto & [prefix, key] : derivedKeys_) {
			webrtc::ExplicitZeroMemory(key.data(), key.size());
		}
		webrtc::ExplicitZeroMemory(derivedContext_.data(), derivedContext_.size());

		derivedKeys_.clear();
		derivedContext_.clear();
	}
}
