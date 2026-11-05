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

#ifndef JNI_WEBRTC_API_ICE_UDP_ENDPOINT_H_
#define JNI_WEBRTC_API_ICE_UDP_ENDPOINT_H_

#include "absl/strings/string_view.h"
#include "api/environment/environment.h"
#include "api/task_queue/task_queue_base.h"
#include "api/transport/stun.h"
#include "rtc_base/async_packet_socket.h"
#include "rtc_base/ip_address.h"
#include "rtc_base/network/received_packet.h"
#include "rtc_base/socket_address.h"
#include "rtc_base/thread.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace jni
{
	// Receives the datagrams the endpoint routes to one virtual socket. Called on the
	// network thread that owns the socket.
	class EndpointReceiver
	{
		public:
			virtual ~EndpointReceiver() = default;

			virtual void Receive(std::span<const uint8_t> data, const webrtc::SocketAddress & from,
				std::optional<webrtc::Timestamp> arrival) = 0;
	};

	// Where the endpoint delivers a virtual socket's datagrams. The socket sets both
	// fields and clears them under the mutex when it is destroyed, on its network
	// thread. That thread may go right after, so the endpoint posts to it only under
	// the mutex, and a delivery posted earlier finds the receiver gone.
	struct EndpointLink
	{
		std::mutex mutex;
		EndpointReceiver * receiver = nullptr;
		webrtc::TaskQueueBase * thread = nullptr;
	};

	// One UDP port shared by many peer connections. The endpoint owns an OS socket per
	// local address, all on the same port, and routes each datagram to the ICE port it
	// belongs to. Peers use it through EndpointPortAllocator, whose ICE ports get
	// virtual sockets on these sockets.
	//
	// libwebrtc gives every ICE session its own UDP port, so a port only ever sees the
	// checks of its own session. Here all sessions share one port, so the endpoint
	// tells them apart: a binding request goes to the session whose local ufrag it
	// names and whose password its MESSAGE-INTEGRITY verifies against, a response goes
	// to the socket that sent the request, and everything else follows the route that
	// an authenticated binding request set up for its source address.
	//
	// All routing state lives on the endpoint's own thread. Peers call in from their
	// network threads; calls that change state are posted, so they never wait on it.
	class IceUdpEndpoint
	{
		public:
			// Starts the endpoint thread. An empty address list allows every local address.
			static std::shared_ptr<IceUdpEndpoint> Create(uint16_t port, std::vector<webrtc::IPAddress> addresses);

			~IceUdpEndpoint();

			IceUdpEndpoint(const IceUdpEndpoint &) = delete;
			IceUdpEndpoint & operator=(const IceUdpEndpoint &) = delete;

			uint16_t port() const { return port_; }

			bool Allows(const webrtc::IPAddress & address) const;

			// Closes the sockets and drops all routes. Peers keep running but neither send
			// nor receive through the endpoint any more.
			void Close();

			// Feeds a datagram into the routing as if the socket for local had received it.
			void Inject(const webrtc::SocketAddress & local, const webrtc::SocketAddress & remote,
				std::vector<uint8_t> data);

			// A peer connection. Its routes count against one limit, and another peer
			// cannot take them over while they are in use.
			uint64_t AddPeer();
			void RemovePeer(uint64_t peer);

			// An ICE session of a peer. Its local credentials authenticate the checks that
			// its sockets receive, and change when a pooled session is taken.
			uint64_t AddSession(uint64_t peer);
			void SetCredentials(uint64_t session, std::string ufrag, std::string password);
			void RemoveSession(uint64_t session);

			// Opens, or reuses, the socket for a local address. Returns its bound address,
			// or nothing when the address is not allowed, cannot be bound or the endpoint
			// is closed. Blocks on the endpoint thread.
			std::optional<webrtc::SocketAddress> OpenSocket(const webrtc::IPAddress & address);

			uint64_t AddVirtualSocket(uint64_t peer, uint64_t session, const webrtc::SocketAddress & local,
				std::shared_ptr<EndpointLink> link);
			void RemoveVirtualSocket(uint64_t socket);

			void Send(uint64_t socket, const webrtc::SocketAddress & local, const webrtc::SocketAddress & remote,
				std::vector<uint8_t> data);

		private:
			IceUdpEndpoint(uint16_t port, std::vector<webrtc::IPAddress> addresses);

			using RouteKey = std::pair<webrtc::SocketAddress, webrtc::SocketAddress>;

			struct RouteEntry
			{
				uint64_t socket;
				int64_t lastReceiveMs;
			};

			struct SocketEntry
			{
				uint64_t peer;
				uint64_t session;
				webrtc::SocketAddress local;
				std::shared_ptr<EndpointLink> link;
				std::set<RouteKey> routes;
			};

			struct SessionEntry
			{
				uint64_t peer;
				std::string ufrag;
				std::string password;
				std::vector<uint64_t> sockets;
			};

			struct PeerEntry
			{
				std::set<uint64_t> sessions;
				size_t routes = 0;
			};

			struct Transaction
			{
				uint64_t socket;
				int64_t expiresMs;
			};

			// Endpoint thread only.
			void Route(const webrtc::SocketAddress & local, std::span<const uint8_t> data,
				const webrtc::SocketAddress & from, std::optional<webrtc::Timestamp> arrival);
			void RouteBindingRequest(const RouteKey & key, std::span<const uint8_t> data,
				std::optional<webrtc::Timestamp> arrival, int64_t nowMs);
			std::optional<uint64_t> Authenticate(const webrtc::SocketAddress & local, webrtc::StunMessage & message,
				absl::string_view ufrag);
			void Deliver(uint64_t socket, std::span<const uint8_t> data, const webrtc::SocketAddress & from,
				std::optional<webrtc::Timestamp> arrival);
			void MapRoute(uint64_t socket, const RouteKey & key, int64_t nowMs);
			void UnmapRoute(std::map<RouteKey, RouteEntry>::iterator route);
			void RecordTransaction(uint64_t socket, std::span<const uint8_t> data, int64_t nowMs);
			void ExpireTransactions(int64_t nowMs);
			void ForgetSession(uint64_t session);
			void ForgetSocket(uint64_t socket);

			const uint16_t port_;
			const std::vector<webrtc::IPAddress> addresses_;
			const webrtc::Environment env_;
			std::unique_ptr<webrtc::Thread> thread_;
			std::atomic<uint64_t> nextId_ { 1 };

			bool closed_ = false;
			std::map<webrtc::SocketAddress, std::unique_ptr<webrtc::AsyncPacketSocket>> osSockets_;
			std::map<uint64_t, PeerEntry> peers_;
			std::map<uint64_t, SessionEntry> sessions_;
			std::multimap<std::string, uint64_t, std::less<>> sessionsByUfrag_;
			std::map<uint64_t, SocketEntry> sockets_;
			std::map<RouteKey, RouteEntry> routes_;
			std::map<std::string, Transaction> transactions_;
			std::deque<std::pair<int64_t, std::string>> transactionOrder_;
	};
}

#endif
