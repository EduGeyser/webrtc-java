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

#include "api/EndpointPortAllocator.h"
#include "api/FieldTrialsView.h"

#include "api/candidate.h"
#include "api/environment/environment_factory.h"
#include "api/task_queue/task_queue_base.h"
#include "p2p/base/port_interface.h"

#include <algorithm>
#include <cerrno>
#include <optional>
#include <vector>

namespace jni
{
	namespace
	{
		// The local addresses a connection may gather on: its own list, narrowed to
		// what its endpoint allows. Nothing for every address.
		std::optional<std::vector<webrtc::IPAddress>> AllowedAddresses(const std::shared_ptr<IceUdpEndpoint> & endpoint,
			std::vector<webrtc::IPAddress> allowed)
		{
			if (endpoint == nullptr || endpoint->addresses().empty()) {
				return allowed.empty() ? std::nullopt : std::make_optional(std::move(allowed));
			}
			if (allowed.empty()) {
				return endpoint->addresses();
			}

			std::erase_if(allowed, [&endpoint](const webrtc::IPAddress & address) { return !endpoint->Allows(address); });

			return allowed;
		}

		webrtc::Environment CreateEnvironment(std::map<std::string, std::string> fieldTrials)
		{
			if (fieldTrials.empty()) {
				return webrtc::CreateEnvironment();
			}
			return webrtc::CreateEnvironment(std::make_unique<FieldTrialsView>(std::move(fieldTrials)));
		}
	}

	NetworkContext::NetworkContext(webrtc::Thread * networkThread, std::map<std::string, std::string> fieldTrials) :
		thread_(networkThread),
		env_(CreateEnvironment(std::move(fieldTrials)))
	{
		thread_->BlockingCall([this] {
			networkManager_ = std::make_unique<webrtc::BasicNetworkManager>(env_, thread_->socketserver());
			socketFactory_ = std::make_unique<webrtc::BasicPacketSocketFactory>(thread_->socketserver());
		});
	}

	NetworkContext::~NetworkContext()
	{
		thread_->BlockingCall([this] {
			networkManager_.reset();
			socketFactory_.reset();
		});
	}

	AllowedNetworkManager::AllowedNetworkManager(webrtc::NetworkManager & base, std::vector<webrtc::IPAddress> allowed) :
		base_(base),
		allowed_(std::move(allowed))
	{
	}

	AllowedNetworkManager::~AllowedNetworkManager()
	{
		if (subscribed_) {
			base_.UnsubscribeNetworksChanged(this);
			base_.UnsubscribeError(this);
		}
	}

	void AllowedNetworkManager::StartUpdating()
	{
		// Here rather than in the constructor, as the base manager's callbacks belong
		// to the network thread. Before starting, since starting may notify at once.
		if (!subscribed_) {
			base_.SubscribeNetworksChanged(this, [this] { NotifyNetworksChanged(); });
			base_.SubscribeError(this, [this] { NotifyError(); });
			subscribed_ = true;
		}

		base_.StartUpdating();
	}

	void AllowedNetworkManager::StopUpdating()
	{
		base_.StopUpdating();
	}

	std::vector<const webrtc::Network *> AllowedNetworkManager::GetNetworks() const
	{
		return Filter(base_.GetNetworks());
	}

	webrtc::NetworkManager::EnumerationPermission AllowedNetworkManager::enumeration_permission() const
	{
		return base_.enumeration_permission();
	}

	std::vector<const webrtc::Network *> AllowedNetworkManager::GetAnyAddressNetworks()
	{
		return Filter(base_.GetAnyAddressNetworks());
	}

	bool AllowedNetworkManager::GetDefaultLocalAddress(int family, webrtc::IPAddress * address) const
	{
		return base_.GetDefaultLocalAddress(family, address) && Allows(*address);
	}

	webrtc::MdnsResponderInterface * AllowedNetworkManager::GetMdnsResponder() const
	{
		return base_.GetMdnsResponder();
	}

	void AllowedNetworkManager::set_vpn_list(const std::vector<webrtc::NetworkMask> & vpn)
	{
		base_.set_vpn_list(vpn);
	}

	bool AllowedNetworkManager::Allows(const webrtc::IPAddress & address) const
	{
		return std::find(allowed_.begin(), allowed_.end(), address) != allowed_.end();
	}

	std::vector<const webrtc::Network *> AllowedNetworkManager::Filter(std::vector<const webrtc::Network *> networks) const
	{
		std::erase_if(networks, [this](const webrtc::Network * network) {
			return !Allows(webrtc::IPAddress(network->GetBestIP()));
		});

		return networks;
	}

	VirtualSocket::VirtualSocket(const webrtc::Environment & env, std::shared_ptr<IceUdpEndpoint> endpoint, uint64_t peer,
		uint64_t session, const webrtc::SocketAddress & local) :
		env_(env),
		endpoint_(std::move(endpoint)),
		local_(local),
		link_(std::make_shared<EndpointLink>())
	{
		link_->receiver = this;
		link_->thread = webrtc::TaskQueueBase::Current();
		id_ = endpoint_->AddVirtualSocket(peer, session, local_, link_);
	}

	VirtualSocket::~VirtualSocket()
	{
		{
			std::lock_guard<std::mutex> lock(link_->mutex);
			link_->receiver = nullptr;
			link_->thread = nullptr;
		}
		endpoint_->RemoveVirtualSocket(id_);
	}

	webrtc::SocketAddress VirtualSocket::GetLocalAddress() const
	{
		return local_;
	}

	webrtc::SocketAddress VirtualSocket::GetRemoteAddress() const
	{
		return webrtc::SocketAddress();
	}

	int VirtualSocket::Send(const void *, size_t, const webrtc::AsyncSocketPacketOptions &)
	{
		// Unconnected, like the UDP socket it stands in for.
		error_ = ENOTCONN;
		return -1;
	}

	int VirtualSocket::SendTo(const void * data, size_t size, const webrtc::SocketAddress & address,
		const webrtc::AsyncSocketPacketOptions & options)
	{
		if (closed_) {
			error_ = ENOTCONN;
			return -1;
		}

		const uint8_t * bytes = static_cast<const uint8_t *>(data);
		endpoint_->Send(id_, local_, address, std::vector<uint8_t>(bytes, bytes + size));

		webrtc::SentPacketInfo sent(options.packet_id, env_.clock().TimeInMilliseconds(), options.info_signaled_after_sent);
		webrtc::CopySocketInformationToPacketInfo(size, *this, &sent.info);
		NotifySentPacket(this, sent);

		return static_cast<int>(size);
	}

	int VirtualSocket::Close()
	{
		closed_ = true;
		return 0;
	}

	webrtc::AsyncPacketSocket::State VirtualSocket::GetState() const
	{
		return closed_ ? STATE_CLOSED : STATE_BOUND;
	}

	int VirtualSocket::GetOption(webrtc::Socket::Option, int *)
	{
		return -1;
	}

	int VirtualSocket::SetOption(webrtc::Socket::Option, int)
	{
		// Socket options belong to the shared socket, which serves every peer alike.
		return 0;
	}

	int VirtualSocket::GetError() const
	{
		return error_;
	}

	void VirtualSocket::SetError(int error)
	{
		error_ = error;
	}

	void VirtualSocket::Receive(std::span<const uint8_t> data, const webrtc::SocketAddress & from,
		std::optional<webrtc::Timestamp> arrival)
	{
		if (!closed_) {
			NotifyPacketReceived(webrtc::ReceivedIpPacket(data, from, arrival));
		}
	}

	EndpointSocketFactory::EndpointSocketFactory(webrtc::PacketSocketFactory & real, std::shared_ptr<IceUdpEndpoint> endpoint,
		uint64_t peer, std::vector<webrtc::IPAddress> allowed) :
		real_(real),
		endpoint_(std::move(endpoint)),
		peer_(peer),
		allowed_(std::move(allowed))
	{
	}

	bool EndpointSocketFactory::Allows(const webrtc::IPAddress & address) const
	{
		return allowed_.empty() || std::find(allowed_.begin(), allowed_.end(), address) != allowed_.end();
	}

	std::unique_ptr<webrtc::AsyncPacketSocket> EndpointSocketFactory::CreateUdpSocket(const webrtc::Environment & env,
		const webrtc::SocketAddress & address, uint16_t minPort, uint16_t maxPort)
	{
		// The allocator asks once per network, for that network's best address. Refusing
		// it leaves the network without UDP candidates.
		if (!Allows(address.ipaddr())) {
			return nullptr;
		}
		if (endpoint_ == nullptr) {
			return real_.CreateUdpSocket(env, address, minPort, maxPort);
		}

		std::optional<webrtc::SocketAddress> local = endpoint_->OpenSocket(address.ipaddr());

		if (!local) {
			return nullptr;
		}

		return std::make_unique<VirtualSocket>(env, endpoint_, peer_, session_, *local);
	}

	std::unique_ptr<webrtc::AsyncListenSocket> EndpointSocketFactory::CreateServerTcpSocket(const webrtc::Environment & env,
		const webrtc::SocketAddress & local, uint16_t minPort, uint16_t maxPort, int options)
	{
		if (!Allows(local.ipaddr())) {
			return nullptr;
		}
		return real_.CreateServerTcpSocket(env, local, minPort, maxPort, options);
	}

	std::unique_ptr<webrtc::AsyncPacketSocket> EndpointSocketFactory::CreateClientTcpSocket(const webrtc::Environment & env,
		const webrtc::SocketAddress & local, const webrtc::SocketAddress & remote,
		const webrtc::PacketSocketTcpOptions & options)
	{
		if (!Allows(local.ipaddr())) {
			return nullptr;
		}
		return real_.CreateClientTcpSocket(env, local, remote, options);
	}

	std::unique_ptr<webrtc::AsyncDnsResolverInterface> EndpointSocketFactory::CreateAsyncDnsResolver()
	{
		return real_.CreateAsyncDnsResolver();
	}

	OwnSocketTurnPortFactory::OwnSocketTurnPortFactory(webrtc::PacketSocketFactory & real) :
		real_(real)
	{
	}

	std::unique_ptr<webrtc::Port> OwnSocketTurnPortFactory::Create(webrtc::CreateRelayPortArgs && args,
		webrtc::AsyncPacketSocket *)
	{
		args.socket_factory = &real_;

		return webrtc::TurnPortFactory::Create(std::move(args), 0, 0);
	}

	EndpointAllocatorParts::EndpointAllocatorParts(NetworkContext & context, std::shared_ptr<IceUdpEndpoint> endpoint,
		std::vector<webrtc::IPAddress> allowed) :
		endpoint_(std::move(endpoint)),
		peer_(endpoint_ ? endpoint_->AddPeer() : 0),
		networks_([&] {
			std::optional<std::vector<webrtc::IPAddress>> addresses = AllowedAddresses(endpoint_, allowed);

			return addresses ? std::make_unique<AllowedNetworkManager>(*context.networkManager(), std::move(*addresses)) : nullptr;
		}()),
		socketFactory_(std::make_unique<EndpointSocketFactory>(*context.socketFactory(), endpoint_, peer_, std::move(allowed))),
		relayFactory_(endpoint_ ? std::make_unique<OwnSocketTurnPortFactory>(*context.socketFactory()) : nullptr)
	{
	}

	EndpointPortAllocator::EndpointPortAllocator(NetworkContext & context, std::shared_ptr<IceUdpEndpoint> endpoint,
		std::vector<webrtc::IPAddress> allowed, webrtc::TurnCustomizer * turnCustomizer) :
		EndpointAllocatorParts(context, std::move(endpoint), std::move(allowed)),
		webrtc::BasicPortAllocator(context.env(),
			networks_ ? static_cast<webrtc::NetworkManager *>(networks_.get()) : context.networkManager(),
			socketFactory_.get(), turnCustomizer, relayFactory_.get())
	{
	}

	EndpointPortAllocator::~EndpointPortAllocator()
	{
		if (endpoint_ != nullptr) {
			endpoint_->RemovePeer(peer_);
		}
	}

	webrtc::PortAllocatorSession * EndpointPortAllocator::CreateSessionInternal(absl::string_view contentName, int component,
		absl::string_view iceUfrag, absl::string_view icePwd)
	{
		if (endpoint_ == nullptr) {
			return webrtc::BasicPortAllocator::CreateSessionInternal(contentName, component, iceUfrag, icePwd);
		}

		CheckRunOnValidThreadAndInitialized();

		return new EndpointSession(this, contentName, component, iceUfrag, icePwd);
	}

	EndpointSession::EndpointSession(EndpointPortAllocator * allocator, absl::string_view contentName, int component,
		absl::string_view iceUfrag, absl::string_view icePwd) :
		webrtc::BasicPortAllocatorSession(allocator, contentName, component, iceUfrag, icePwd),
		owner_(allocator),
		id_(allocator->endpoint_->AddSession(allocator->peer_))
	{
		owner_->endpoint_->SetCredentials(id_, std::string(iceUfrag), std::string(icePwd));
		// Until a sequence asks, sockets belong to the newest session.
		owner_->socketFactory_->SetSession(id_);

		// The transport channel takes the port further on in the same notification. A
		// replay goes through the endpoint thread and back, so it finds the port in place.
		SubscribePortReady(this, [this](webrtc::PortAllocatorSession *, webrtc::PortInterface * port) {
			if (port->GetProtocol() == webrtc::PROTO_UDP && port->Type() == webrtc::IceCandidateType::kHost) {
				owner_->endpoint_->SessionReady(id_, port->Network()->GetBestIP());
			}
		});
	}

	EndpointSession::~EndpointSession()
	{
		owner_->endpoint_->RemoveSession(id_);
	}

	webrtc::BasicPortAllocator * EndpointSession::allocator()
	{
		owner_->socketFactory_->SetSession(id_);

		return webrtc::BasicPortAllocatorSession::allocator();
	}

	void EndpointSession::UpdateIceParametersInternal()
	{
		owner_->endpoint_->SetCredentials(id_, std::string(ice_ufrag()), std::string(ice_pwd()));

		webrtc::BasicPortAllocatorSession::UpdateIceParametersInternal();
	}
}
