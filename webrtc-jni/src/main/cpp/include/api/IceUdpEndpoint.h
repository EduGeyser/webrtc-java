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

#include "api/ProxyRelays.h"

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
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
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

	// A binding request that no session on the endpoint authenticated.
	struct PendingRequestInfo
	{
		uint64_t id;
		webrtc::SocketAddress local;
		webrtc::SocketAddress remote;
		std::string localUfrag;
		std::string remoteUfrag;
	};

	// Called on the endpoint thread. Must not block.
	using PendingRequestHandler = std::function<void(const PendingRequestInfo &)>;

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
	// A binding request is held, packet and all, when its session cannot take it yet:
	// its UDP port is not ready, or no session exists. In the second case a handler
	// learns about it and may create a connection with the request's local ufrag and a
	// password it verifies against. Once that session's port is ready, the endpoint
	// verifies the held request against it and replays it, so the client gets its
	// answer without retransmitting.
	//
	// All routing state lives on the endpoint's own thread. Peers call in from their
	// network threads; calls that change state are posted, so they never wait on it.
	class IceUdpEndpoint
	{
		public:
			struct StunReading
			{
				enum class State { kPending, kSucceeded, kFailed };

				webrtc::SocketAddress server;
				webrtc::SocketAddress local;
				// The address the server saw, from its latest answer.
				std::optional<webrtc::SocketAddress> mapped;
				// Of the latest transaction that completed.
				State state;
				uint64_t successfulResponses;
				uint64_t failedTransactions;
				// Counts changes of the mapped address, the first one included.
				uint64_t mappingRevision;
				std::optional<int64_t> lastSuccessAgeMs;
			};

			struct Stats
			{
				uint64_t datagramsReceived = 0;
				uint64_t bytesReceived = 0;
				uint64_t datagramsSent = 0;
				uint64_t bytesSent = 0;
				// Datagrams no route, transaction or session took.
				uint64_t unroutable = 0;
				// Binding requests that authenticated for no session and were not held.
				uint64_t unauthenticated = 0;
				// Checks not held, as too many were or they were too large.
				uint64_t pendingDropped = 0;
				uint64_t announced = 0;
				uint64_t replayed = 0;
				uint64_t expired = 0;
				uint64_t rejected = 0;
				// Datagrams the relay rules dropped on the way in.
				uint64_t relayDropped = 0;
				// Datagrams not sent, as no relay routes their destination.
				uint64_t relayFiltered = 0;
				// Current.
				uint64_t routes = 0;
				uint64_t sessions = 0;
				uint64_t pending = 0;
			};

			// Starts the endpoint thread. An empty address list allows every local address.
			static std::shared_ptr<IceUdpEndpoint> Create(uint16_t port, std::vector<webrtc::IPAddress> addresses);

			~IceUdpEndpoint();

			IceUdpEndpoint(const IceUdpEndpoint &) = delete;
			IceUdpEndpoint & operator=(const IceUdpEndpoint &) = delete;

			uint16_t port() const { return port_; }

			bool Allows(const webrtc::IPAddress & address) const;

			// Empty allows every local address.
			const std::vector<webrtc::IPAddress> & addresses() const { return addresses_; }

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

			// A session's UDP port on a local address can take packets.
			void SessionReady(uint64_t session, const webrtc::IPAddress & address);

			// How many requests are held at most, and for how long.
			void SetPendingLimits(size_t maxPending, int64_t timeoutMs);

			// Without a handler, requests for no session are dropped. Blocks until the
			// previous handler is no longer called.
			void SetPendingRequestHandler(PendingRequestHandler handler);

			// With keys, a request for no session is held only if its MESSAGE-INTEGRITY
			// verifies against a derived password: for a local ufrag that starts with a
			// key's prefix, the unpadded standard base64 of the first 24 bytes of
			// HMAC-SHA256(key, context || ufrag). Without keys every such request is held.
			void SetDerivedPasswords(std::string context, std::vector<std::pair<std::string, std::string>> keys);

			void Reject(uint64_t request);

			// Takes PROXY protocol v2 headers from relays at these numeric addresses or
			// CIDR ranges; see ProxyRelays. A new list keeps the clients of relays it
			// still lists; an empty one clears it. False if an entry is invalid.
			bool SetProxyRelays(std::vector<std::string> relays);

			// The relay a client comes through, routed or still held.
			std::optional<webrtc::SocketAddress> ProxyRelayOf(const webrtc::SocketAddress & client);

			// Keeps a binding with a STUN server alive from the endpoint's socket on a
			// local address, so that the NAT mapping of the shared port stays open and
			// known with no connection running: a binding request every 15 seconds,
			// retransmitted while unanswered. 0 if the address is not allowed or cannot
			// be bound.
			uint64_t OpenStunMonitor(const webrtc::IPAddress & local, const webrtc::SocketAddress & server);
			std::optional<StunReading> ReadStunMonitor(uint64_t monitor);
			void CloseStunMonitor(uint64_t monitor);

			Stats GetStats();

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
				bool ready = false;
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

			struct PendingRequest
			{
				webrtc::SocketAddress local;
				webrtc::SocketAddress remote;
				std::string username;
				std::string localUfrag;
				std::vector<uint8_t> packet;
				std::optional<webrtc::Timestamp> arrival;
				int64_t expiresMs;
				// Announced to the handler, so verified against a session only once.
				bool announced;
				// The PROXY protocol relay that brought it, which routes the client once a
				// session takes it.
				std::optional<webrtc::SocketAddress> relay;
			};

			using PendingKey = std::tuple<webrtc::SocketAddress, webrtc::SocketAddress, std::string>;

			struct StunMonitor
			{
				webrtc::SocketAddress local;
				webrtc::SocketAddress server;
				StunReading::State state = StunReading::State::kPending;
				std::optional<webrtc::SocketAddress> mapped;
				uint64_t successes = 0;
				uint64_t failures = 0;
				uint64_t revision = 0;
				std::optional<int64_t> lastSuccessMs;
				std::string transaction;
				bool answered = true;
			};

			// Checks MESSAGE-INTEGRITY against one password after another. libwebrtc
			// validates a message once; later passwords revalidate it.
			class IntegrityCheck
			{
				public:
					explicit IntegrityCheck(webrtc::StunMessage & message) : message_(message) {}

					bool Matches(const std::string & password);

				private:
					webrtc::StunMessage & message_;
					bool validated_ = false;
			};

			// Endpoint thread only. relay is the PROXY protocol relay that brought a
			// datagram, when it does not route the client yet.
			void Receive(const webrtc::SocketAddress & local, std::span<const uint8_t> data,
				const webrtc::SocketAddress & from, std::optional<webrtc::Timestamp> arrival);
			void Route(const webrtc::SocketAddress & local, std::span<const uint8_t> data,
				const webrtc::SocketAddress & from, std::optional<webrtc::Timestamp> arrival,
				const std::optional<webrtc::SocketAddress> & relay);
			void RouteBindingRequest(const RouteKey & key, std::span<const uint8_t> data,
				std::optional<webrtc::Timestamp> arrival, int64_t nowMs, const std::optional<webrtc::SocketAddress> & relay);
			bool ClientAlive(const webrtc::SocketAddress & client) const;
			std::optional<webrtc::SocketAddress> OpenSocketHere(const webrtc::IPAddress & address);
			void StartMonitorTransaction(uint64_t monitor);
			void SendMonitorRequest(uint64_t monitor, const std::string & transaction, int attempt);
			bool OnMonitorResponse(std::span<const uint8_t> data, const webrtc::SocketAddress & from, int64_t nowMs);
			std::optional<uint64_t> Authenticate(IntegrityCheck & integrity, absl::string_view ufrag);
			std::optional<uint64_t> SocketOf(const SessionEntry & session, const webrtc::SocketAddress & local) const;
			bool Claim(uint64_t socket, const RouteKey & key, int64_t nowMs);
			bool VerifyDerived(IntegrityCheck & integrity, absl::string_view ufrag) const;
			void Hold(const RouteKey & key, absl::string_view username, absl::string_view ufrag,
				std::span<const uint8_t> data, std::optional<webrtc::Timestamp> arrival, int64_t nowMs, bool announce,
				const std::optional<webrtc::SocketAddress> & relay);
			void Replay(uint64_t session, const webrtc::SocketAddress & local, int64_t nowMs);
			std::optional<PendingRequest> TakePending(uint64_t request);
			void SweepPending();
			void ClearDerivedPasswords();
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

			size_t maxPending_ = 1024;
			int64_t pendingTimeoutMs_ = 15000;
			PendingRequestHandler handler_;
			std::string derivedContext_;
			std::vector<std::pair<std::string, std::string>> derivedKeys_;
			std::map<uint64_t, PendingRequest> pending_;
			std::map<PendingKey, uint64_t> pendingByKey_;
			std::multimap<std::string, uint64_t, std::less<>> pendingByUfrag_;
			bool sweepScheduled_ = false;
			std::unique_ptr<ProxyRelays> proxy_;
			std::map<uint64_t, StunMonitor> monitors_;
			std::map<std::string, uint64_t> monitorTransactions_;
			// The counters; the current values are filled in on reading.
			Stats stats_;
	};
}

#endif
