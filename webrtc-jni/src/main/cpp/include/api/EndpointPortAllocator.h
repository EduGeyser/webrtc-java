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

#ifndef JNI_WEBRTC_API_ENDPOINT_PORT_ALLOCATOR_H_
#define JNI_WEBRTC_API_ENDPOINT_PORT_ALLOCATOR_H_

#include "api/IceUdpEndpoint.h"

#include "api/environment/environment.h"
#include "api/packet_socket_factory.h"
#include "p2p/base/basic_packet_socket_factory.h"
#include "p2p/client/basic_port_allocator.h"
#include "p2p/client/turn_port_factory.h"
#include "rtc_base/network.h"
#include "rtc_base/thread.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jni
{
	// What a factory's peers need to build their own port allocator: the network
	// thread's network manager and real socket factory, and an environment with the
	// factory's field trials. Created with the factory and destroyed on its network
	// thread before that thread stops.
	class NetworkContext
	{
		public:
			NetworkContext(webrtc::Thread * networkThread, std::map<std::string, std::string> fieldTrials);
			~NetworkContext();

			webrtc::Thread * thread() const { return thread_; }
			const webrtc::Environment & env() const { return env_; }
			webrtc::NetworkManager * networkManager() const { return networkManager_.get(); }
			webrtc::PacketSocketFactory * socketFactory() const { return socketFactory_.get(); }

		private:
			webrtc::Thread * const thread_;
			const webrtc::Environment env_;
			std::unique_ptr<webrtc::BasicNetworkManager> networkManager_;
			std::unique_ptr<webrtc::BasicPacketSocketFactory> socketFactory_;
	};

	// Shows an allocator only the networks whose preferred address is allowed. Every
	// port of a network gathers on that address, TCP active candidates included, and
	// those open no socket while gathering, so refusing sockets alone lets them through.
	// Created on any thread; used and destroyed on the network thread.
	class AllowedNetworkManager : public webrtc::NetworkManager
	{
		public:
			AllowedNetworkManager(webrtc::NetworkManager & base, std::vector<webrtc::IPAddress> allowed);
			~AllowedNetworkManager() override;

			void StartUpdating() override;
			void StopUpdating() override;
			std::vector<const webrtc::Network *> GetNetworks() const override;
			EnumerationPermission enumeration_permission() const override;
			std::vector<const webrtc::Network *> GetAnyAddressNetworks() override;
			bool GetDefaultLocalAddress(int family, webrtc::IPAddress * address) const override;
			webrtc::MdnsResponderInterface * GetMdnsResponder() const override;
			void set_vpn_list(const std::vector<webrtc::NetworkMask> & vpn) override;

		private:
			bool Allows(const webrtc::IPAddress & address) const;
			std::vector<const webrtc::Network *> Filter(std::vector<const webrtc::Network *> networks) const;

			webrtc::NetworkManager & base_;
			const std::vector<webrtc::IPAddress> allowed_;
			bool subscribed_ = false;
	};

	// A peer's UDP socket on an IceUdpEndpoint, owned by one ICE session. Created, used
	// and destroyed on the peer's network thread.
	class VirtualSocket : public webrtc::AsyncPacketSocket, public EndpointReceiver
	{
		public:
			VirtualSocket(const webrtc::Environment & env, std::shared_ptr<IceUdpEndpoint> endpoint, uint64_t peer,
				uint64_t session, const webrtc::SocketAddress & local);
			~VirtualSocket() override;

			// AsyncPacketSocket implementation.
			webrtc::SocketAddress GetLocalAddress() const override;
			webrtc::SocketAddress GetRemoteAddress() const override;
			int Send(const void * data, size_t size, const webrtc::AsyncSocketPacketOptions & options) override;
			int SendTo(const void * data, size_t size, const webrtc::SocketAddress & address,
				const webrtc::AsyncSocketPacketOptions & options) override;
			int Close() override;
			State GetState() const override;
			int GetOption(webrtc::Socket::Option option, int * value) override;
			int SetOption(webrtc::Socket::Option option, int value) override;
			int GetError() const override;
			void SetError(int error) override;

			// EndpointReceiver implementation.
			void Receive(std::span<const uint8_t> data, const webrtc::SocketAddress & from,
				std::optional<webrtc::Timestamp> arrival) override;

		private:
			const webrtc::Environment env_;
			const std::shared_ptr<IceUdpEndpoint> endpoint_;
			const webrtc::SocketAddress local_;
			const std::shared_ptr<EndpointLink> link_;
			uint64_t id_ = 0;
			bool closed_ = false;
			int error_ = 0;
	};

	// Gives a peer's ICE ports virtual sockets on an endpoint, or real sockets limited
	// to the allowed local addresses. Everything other than UDP host sockets goes to
	// the network thread's real socket factory.
	class EndpointSocketFactory : public webrtc::PacketSocketFactory
	{
		public:
			EndpointSocketFactory(webrtc::PacketSocketFactory & real, std::shared_ptr<IceUdpEndpoint> endpoint,
				uint64_t peer, std::vector<webrtc::IPAddress> allowed);

			std::unique_ptr<webrtc::AsyncPacketSocket> CreateUdpSocket(const webrtc::Environment & env,
				const webrtc::SocketAddress & address, uint16_t minPort, uint16_t maxPort) override;
			std::unique_ptr<webrtc::AsyncListenSocket> CreateServerTcpSocket(const webrtc::Environment & env,
				const webrtc::SocketAddress & local, uint16_t minPort, uint16_t maxPort, int options) override;
			std::unique_ptr<webrtc::AsyncPacketSocket> CreateClientTcpSocket(const webrtc::Environment & env,
				const webrtc::SocketAddress & local, const webrtc::SocketAddress & remote,
				const webrtc::PacketSocketTcpOptions & options) override;
			std::unique_ptr<webrtc::AsyncDnsResolverInterface> CreateAsyncDnsResolver() override;

			// The ICE session the next UDP sockets belong to.
			void SetSession(uint64_t session) { session_ = session; }

		private:
			bool Allows(const webrtc::IPAddress & address) const;

			webrtc::PacketSocketFactory & real_;
			const std::shared_ptr<IceUdpEndpoint> endpoint_;
			const uint64_t peer_;
			const std::vector<webrtc::IPAddress> allowed_;
			uint64_t session_ = 0;
	};

	// TURN over UDP would share the endpoint's socket, where a relay's traffic cannot
	// be told apart from the peers'. Each TURN port gets its own real socket instead.
	class OwnSocketTurnPortFactory : public webrtc::TurnPortFactory
	{
		public:
			explicit OwnSocketTurnPortFactory(webrtc::PacketSocketFactory & real);

			using webrtc::TurnPortFactory::Create;

			std::unique_ptr<webrtc::Port> Create(webrtc::CreateRelayPortArgs && args,
				webrtc::AsyncPacketSocket * udpSocket) override;

		private:
			webrtc::PacketSocketFactory & real_;
	};

	// Owns what the allocator only points to, constructed before and destroyed after it.
	struct EndpointAllocatorParts
	{
		EndpointAllocatorParts(NetworkContext & context, std::shared_ptr<IceUdpEndpoint> endpoint,
			std::vector<webrtc::IPAddress> allowed);

		const std::shared_ptr<IceUdpEndpoint> endpoint_;
		const uint64_t peer_;
		// Only when some addresses are allowed, by the connection or its endpoint.
		const std::unique_ptr<AllowedNetworkManager> networks_;
		const std::unique_ptr<EndpointSocketFactory> socketFactory_;
		const std::unique_ptr<OwnSocketTurnPortFactory> relayFactory_;
	};

	// The port allocator of one peer connection that gathers on an IceUdpEndpoint, or
	// only on allowed local addresses.
	class EndpointPortAllocator : private EndpointAllocatorParts, public webrtc::BasicPortAllocator
	{
		public:
			EndpointPortAllocator(NetworkContext & context, std::shared_ptr<IceUdpEndpoint> endpoint,
				std::vector<webrtc::IPAddress> allowed, webrtc::TurnCustomizer * turnCustomizer);
			~EndpointPortAllocator() override;

			webrtc::PortAllocatorSession * CreateSessionInternal(absl::string_view contentName, int component,
				absl::string_view iceUfrag, absl::string_view icePwd) override;

		private:
			friend class EndpointSession;
	};

	// An ICE session on an IceUdpEndpoint. It keeps the endpoint up to date with its
	// local credentials and tags the UDP sockets it creates with itself, so that the
	// endpoint can hand each check to the session it is meant for.
	class EndpointSession : public webrtc::BasicPortAllocatorSession
	{
		public:
			EndpointSession(EndpointPortAllocator * allocator, absl::string_view contentName, int component,
				absl::string_view iceUfrag, absl::string_view icePwd);
			~EndpointSession() override;

			// An allocation sequence asks its session for the allocator right before it
			// creates the session's UDP socket, and the network thread runs nothing else
			// in between. That makes this the one place to learn which session a socket
			// belongs to; the socket factory itself is shared by all of them.
			webrtc::BasicPortAllocator * allocator() override;

		protected:
			// Called when a pooled session is taken and gets its final credentials.
			void UpdateIceParametersInternal() override;

		private:
			EndpointPortAllocator * const owner_;
			const uint64_t id_;
	};
}

#endif
