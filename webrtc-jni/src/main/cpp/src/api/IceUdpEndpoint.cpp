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
#include "rtc_base/async_udp_socket.h"
#include "rtc_base/byte_buffer.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"

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
				Route(local, data, remote, std::nullopt);
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

		return thread_->BlockingCall([this, &address]() -> std::optional<webrtc::SocketAddress> {
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
				Route(local, packet.payload(), packet.source_address(), packet.arrival_time());
			});

			osSockets_.emplace(local, std::move(socket));

			return local;
		});
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

			it->second->SendTo(data.data(), data.size(), remote, webrtc::AsyncSocketPacketOptions());
		});
	}

	void IceUdpEndpoint::Route(const webrtc::SocketAddress & local, std::span<const uint8_t> data,
		const webrtc::SocketAddress & from, std::optional<webrtc::Timestamp> arrival)
	{
		RouteKey key(local, from);
		int64_t now = webrtc::TimeMillis();

		if (IsStun(data)) {
			int type = StunType(data);

			if (webrtc::IsStunSuccessResponseType(type) || webrtc::IsStunErrorResponseType(type)) {
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
				RouteBindingRequest(key, data, arrival, now);
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
		std::optional<webrtc::Timestamp> arrival, int64_t nowMs)
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
				route->second.lastReceiveMs = nowMs;
				Deliver(route->second.socket, data, key.second, arrival);
				return;
			}
		}

		std::optional<uint64_t> socket = Authenticate(key.first, message, ufrag);

		if (!socket) {
			return;
		}

		if (route != routes_.end() && route->second.socket != *socket) {
			auto previous = sockets_.find(route->second.socket);
			bool samePeer = previous != sockets_.end() && previous->second.peer == sockets_.at(*socket).peer;

			// A new ICE session of the same peer takes its addresses along. Another peer
			// waits until the route is dead, so that a forged source address cannot cut
			// a live connection off.
			if (!samePeer && nowMs - route->second.lastReceiveMs < kRouteIdleMs) {
				return;
			}
		}

		MapRoute(*socket, key, nowMs);
		Deliver(*socket, data, key.second, arrival);
	}

	std::optional<uint64_t> IceUdpEndpoint::Authenticate(const webrtc::SocketAddress & local, webrtc::StunMessage & message,
		absl::string_view ufrag)
	{
		auto [first, last] = sessionsByUfrag_.equal_range(ufrag);
		bool validated = false;

		// Generated ufrags are short, so several sessions can share one; only the
		// password tells them apart.
		for (auto index = first; index != last; ++index) {
			const SessionEntry & session = sessions_.at(index->second);

			auto status = validated
				? message.RevalidateMessageIntegrity(session.password)
				: message.ValidateMessageIntegrity(session.password);
			validated = true;

			if (status != webrtc::StunMessage::IntegrityStatus::kIntegrityOk) {
				continue;
			}

			// A session has one socket per local address; a newer one replaces an older.
			for (auto socket = session.sockets.rbegin(); socket != session.sockets.rend(); ++socket) {
				if (sockets_.at(*socket).local == local) {
					return *socket;
				}
			}
			return std::nullopt;
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
}
