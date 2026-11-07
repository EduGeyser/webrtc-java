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

package io.github.sendablemetatype.webrtc;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.fail;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import java.io.IOException;
import java.io.UncheckedIOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.Inet4Address;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.NetworkInterface;
import java.net.SocketException;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Base64;
import java.util.Collection;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

class IceUdpEndpointTests extends TestBase {

	private static final SecureRandom RANDOM = new SecureRandom();

	private IceUdpEndpoint endpoint;

	private final List<Peer> peers = new ArrayList<>();

	private final BlockingQueue<IceUdpEndpoint.PendingRequest> requests = new LinkedBlockingQueue<>();

	// The test instance serves the whole class (see TestBase), so each test
	// sets these up anew.
	private ExecutorService requestExecutor;


	@BeforeEach
	void createEndpoint() throws Exception {
		requestExecutor = Executors.newSingleThreadExecutor();
		endpoint = new IceUdpEndpoint(freePort());
	}

	@AfterEach
	void dispose() {
		peers.forEach(Peer::close);
		peers.clear();
		requests.clear();
		endpoint.dispose();
		requestExecutor.shutdownNow();
	}

	@Test
	void socketsAreBoundAsConnectionsGatherAndFreedOnDispose() throws Exception {
		IceUdpEndpoint.Sockets before = endpoint.getSockets();

		assertTrue(before.bound().isEmpty(), before.toString());
		assertTrue(before.failed().isEmpty(), before.toString());

		// The connection holds the native endpoint past its dispose.
		List<InetAddress> hosts = endpointHostAddresses();
		IceUdpEndpoint.Sockets after = endpoint.getSockets();

		for (InetAddress host : hosts) {
			assertTrue(after.bound().contains(new InetSocketAddress(host, endpoint.getPort())), host + ": " + after);
		}
		for (InetSocketAddress local : after.bound()) {
			assertThrows(SocketException.class, () -> new DatagramSocket(local).close(), local + " is free");
		}
		after.failed().forEach(IceUdpEndpointTests::assertUnavailable);

		endpoint.dispose();

		assertThrows(IllegalStateException.class, endpoint::getSockets);

		for (InetSocketAddress local : after.bound()) {
			assertDoesNotThrow(() -> new DatagramSocket(local).close(), local + " is still bound");
		}
	}

	@Test
	void portTakenOnAnAddressIsReportedWhenAConnectionGathersThere() throws Exception {
		int port = endpoint.getPort();
		List<InetAddress> hosts = gatheredAddresses();
		// The first address connections gather on, whatever its family.
		InetSocketAddress taken = new InetSocketAddress(hosts.get(0), port);
		BlockingQueue<IceUdpEndpoint.BindFailure> failures = new LinkedBlockingQueue<>();

		endpoint.setBindFailureHandler(failures::add, requestExecutor);

		try (DatagramSocket holder = new DatagramSocket(taken)) {
			// None, when the taken address is the runner's only one.
			List<InetAddress> gathered = toAddresses(hostsGatheredOn(endpoint));
			IceUdpEndpoint.BindFailure reported = awaitFailure(failures, taken);
			IceUdpEndpoint.Sockets sockets = endpoint.getSockets();

			assertFalse(gathered.contains(taken.getAddress()), "Gathered on " + taken);
			assertTrue(reported.failure().getMessage().startsWith("Cannot bind "), reported.toString());
			assertTrue(sockets.failed().stream().anyMatch(failure -> failure.address().equals(taken)), sockets.toString());
			assertFalse(sockets.bound().contains(taken), sockets.toString());

			// The other addresses are bound as ever, unless the runner keeps one from it.
			for (InetAddress host : hosts.subList(1, hosts.size())) {
				InetSocketAddress local = new InetSocketAddress(host, port);

				if (sockets.bound().contains(local)) {
					assertTrue(gathered.contains(host), "No candidate on " + local);
				}
				else {
					sockets.failed().stream()
							.filter(failure -> failure.address().equals(local))
							.findFirst()
							.ifPresentOrElse(IceUdpEndpointTests::assertUnavailable,
									() -> fail(local + " was not tried: " + sockets));
				}
			}
		}

		// Free now, so the next connection that gathers there binds it.
		assertTrue(endpointHostAddresses().contains(taken.getAddress()), "No candidate on " + taken);

		IceUdpEndpoint.Sockets sockets = endpoint.getSockets();

		assertTrue(sockets.bound().contains(taken), sockets.toString());
		assertTrue(sockets.failed().stream().noneMatch(failure -> failure.address().equals(taken)), sockets.toString());
	}

	@Test
	void listedLinkLocalAddressIsBoundInItsScope() throws Exception {
		Inet6Address linkLocal = bindableLinkLocalAddress();

		assumeTrue(linkLocal != null, "No bindable link-local IPv6 address with a scope");

		int port = endpoint.getPort();
		InetSocketAddress local = new InetSocketAddress(linkLocal, port);
		// Without a scope, the address takes the one it is listed with.
		InetAddress unscoped = InetAddress.getByAddress(linkLocal.getAddress());

		endpoint.dispose();
		endpoint = new IceUdpEndpoint(port, linkLocal);

		try (DatagramSocket server = new DatagramSocket(new InetSocketAddress(linkLocal, 0));
				IceUdpEndpoint.StunMonitor monitor = endpoint.monitorStun(unscoped,
						(InetSocketAddress) server.getLocalSocketAddress())) {
			IceUdpEndpoint.Sockets sockets = endpoint.getSockets();

			// Addresses are equal whatever their scope, so the scope is compared apart.
			assertEquals(List.of(local), sockets.bound(), sockets.toString());
			assertEquals(List.of(linkLocal.getScopeId()), scopes(sockets.bound()), sockets.toString());
			assertEquals(List.of(linkLocal.getScopeId()), scopes(List.of(monitor.read().local())));
			assertThrows(SocketException.class, () -> new DatagramSocket(local).close(), local + " is free");
		}
	}

	@Test
	void sameLinkLocalAddressInTwoScopesIsTwoSockets() throws Exception {
		Inet6Address linkLocal = bindableLinkLocalAddress();

		assumeTrue(linkLocal != null, "No bindable link-local IPv6 address with a scope");

		// No interface has this index, so nothing binds in that scope.
		int missingScope = Collections.list(NetworkInterface.getNetworkInterfaces()).stream()
				.mapToInt(NetworkInterface::getIndex)
				.max()
				.orElse(0) + 1;
		Inet6Address elsewhere = Inet6Address.getByAddress(null, linkLocal.getAddress(), missingScope);
		BlockingQueue<IceUdpEndpoint.BindFailure> failures = new LinkedBlockingQueue<>();

		endpoint.setBindFailureHandler(failures::add, requestExecutor);

		try (DatagramSocket server = new DatagramSocket(new InetSocketAddress(linkLocal, 0));
				IceUdpEndpoint.StunMonitor monitor = endpoint.monitorStun(linkLocal,
						(InetSocketAddress) server.getLocalSocketAddress())) {
			InetSocketAddress serverAddress = (InetSocketAddress) server.getLocalSocketAddress();

			assertThrows(IllegalStateException.class, () -> endpoint.monitorStun(elsewhere, serverAddress));

			IceUdpEndpoint.Sockets sockets = endpoint.getSockets();
			IceUdpEndpoint.BindFailure reported = failures.poll(10, TimeUnit.SECONDS);

			assertEquals(List.of(linkLocal.getScopeId()), scopes(sockets.bound()), sockets.toString());
			assertEquals(List.of(missingScope),
					scopes(sockets.failed().stream().map(IceUdpEndpoint.BindFailure::address).toList()),
					sockets.toString());
			assertNotNull(reported, "No failure reported");
			assertEquals(List.of(missingScope), scopes(List.of(reported.address())));
		}
	}

	@Test
	void connectionsShareThePortAndKeepTheirTraffic() throws Exception {
		Pair first = connect(serverConfig());
		Pair second = connect(serverConfig());

		for (Peer server : List.of(first.server, second.server)) {
			List<RTCIceCandidate> candidates = server.awaitCandidates();

			assertFalse(candidates.isEmpty(), "No candidates");

			for (RTCIceCandidate candidate : candidates) {
				assertEquals(endpoint.getPort(), port(candidate), candidate.sdp);
			}
		}

		first.client.send("first");
		second.client.send("second");

		assertEquals("first", first.server.nextMessage());
		assertEquals("second", second.server.nextMessage());

		first.server.send("back");

		assertEquals("back", first.client.nextMessage());

		IceUdpEndpoint.Stats stats = endpoint.getStats();

		assertTrue(stats.datagramsReceived() > 0 && stats.datagramsSent() > 0, stats.toString());
		assertTrue(stats.sessions() >= 2 && stats.routes() >= 2, stats.toString());
	}

	@Test
	void iceRestartMovesToTheNewSession() throws Exception {
		Pair pair = connect(serverConfig());

		pair.client.connection.restartIce();
		negotiate(pair.client, pair.server);

		// The client moves to a pair of the new session once the server's new
		// session answers its checks on the shared port.
		assertTrue(pair.client.pairChanges.tryAcquire(2, 20, TimeUnit.SECONDS), "The client kept its old pair");

		// The client now sends on the new pair only. The server, which is
		// controlled, moves over once data arrives on it, which takes the endpoint
		// handing that data to the new session.
		pair.client.send("after restart");

		assertEquals("after restart", pair.server.nextMessage());
		assertTrue(pair.server.pairChanges.tryAcquire(2, 20, TimeUnit.SECONDS), "The server kept its old pair");
		assertTrue(pair.server.lastPair.getLocal().sdp.contains(" generation 1 "), pair.server.lastPair.getLocal().sdp);
	}

	@Test
	void pooledSessionGetsItsChecks() throws Exception {
		RTCConfiguration config = serverConfig();
		// The pooled sessions gather on the same addresses at once, and the one taken
		// gets the credentials of the answer only then.
		config.iceCandidatePoolSize = 3;

		Pair pair = connect(config);

		pair.client.send("pooled");

		assertEquals("pooled", pair.server.nextMessage());
	}

	@Test
	void heldCheckReachesTheConnectionCreatedForIt() throws Exception {
		endpoint.setPendingRequestHandler(requests::add, requestExecutor);

		List<String> hosts = endpointHosts();
		String ufrag = "held" + randomIceChars(12);
		Client client = offerToEndpoint(hosts, ufrag, randomIceChars(24));

		IceUdpEndpoint.PendingRequest request = requests.poll(10, TimeUnit.SECONDS);

		assertNotNull(request, "The check was not passed on");
		assertEquals(ufrag, request.localUfrag);
		assertEquals(client.ufrag, request.remoteUfrag);
		assertEquals(endpoint.getPort(), request.localAddress.getPort());

		// Only now does a session with the request's credentials exist.
		client.answerOnServer();

		assertTrue(client.peer.channelOpen.await(20, TimeUnit.SECONDS), "Data channel did not open");
		assertTrue(client.server.channelOpen.await(20, TimeUnit.SECONDS), "Remote data channel did not open");

		// Checks from other client addresses are requests of their own, but the
		// retransmissions of one are not passed on again.
		List<IceUdpEndpoint.PendingRequest> passedOn = new ArrayList<>(List.of(request));
		requests.drainTo(passedOn);

		assertEquals(passedOn.size(), passedOn.stream()
				.map(held -> held.localAddress + "|" + held.remoteAddress)
				.distinct()
				.count(), passedOn.toString());
	}

	@Test
	void derivedPasswordsLetOnlyAuthenticChecksThrough() throws Exception {
		byte[] context = "test-ice-v1\0audience\0".getBytes(StandardCharsets.US_ASCII);
		byte[] key = randomIceChars(43).getBytes(StandardCharsets.US_ASCII);

		endpoint.setDerivedPasswords(context, Map.of("TST1", key));
		endpoint.setPendingRequestHandler(requests::add, requestExecutor);

		List<String> hosts = endpointHosts();

		String forgedUfrag = "TST1" + randomIceChars(12);
		offerToEndpoint(hosts, forgedUfrag, randomIceChars(32));

		assertNull(requests.poll(3, TimeUnit.SECONDS), "A check with a wrong password was passed on");
		assertTrue(endpoint.getStats().unauthenticated() > 0, endpoint.getStats().toString());

		String ufrag = "TST1" + randomIceChars(12);
		Client client = offerToEndpoint(hosts, ufrag, derivedPassword(key, context, ufrag));

		IceUdpEndpoint.PendingRequest request = requests.poll(10, TimeUnit.SECONDS);

		assertNotNull(request, "An authentic check was not passed on");
		assertEquals(ufrag, request.localUfrag);

		client.answerOnServer();

		assertTrue(client.peer.channelOpen.await(20, TimeUnit.SECONDS), "Data channel did not open");
	}

	@Test
	void relayedClientIsSeenAsItself() throws Exception {
		String host = endpointHosts().stream()
				.filter(candidate -> candidate.indexOf(':') < 0)
				.findFirst()
				.orElseThrow(() -> new AssertionError("No IPv4 host candidate on the endpoint"));
		InetAddress hostAddress = InetAddress.getByName(host);

		try (TestRelay relay = new TestRelay(hostAddress, new InetSocketAddress(hostAddress, endpoint.getPort()))) {
			endpoint.setProxyRelays(host);

			Peer client = new Peer(new RTCConfiguration());
			Peer server = new Peer(serverConfig());

			client.channel = client.connection.createDataChannel("data", new RTCDataChannelInit());
			client.channel.registerObserver(client.channelObserver(client.channel));

			negotiate(client, server);

			// The client knows only the relay; the server learns the client from its checks.
			String candidate = "candidate:1 1 udp 2122260223 " + host + " " + relay.port() + " typ host";
			client.connection.addIceCandidate(new RTCIceCandidate("0", 0, candidate), new TestAddIceObserver());

			assertTrue(client.channelOpen.await(20, TimeUnit.SECONDS), "Data channel did not open");
			assertTrue(server.channelOpen.await(20, TimeUnit.SECONDS), "Remote data channel did not open");

			RTCCandidatePairChangeEvent pair = server.lastPair;
			InetSocketAddress seen = new InetSocketAddress(InetAddress.getByName(pair.remoteAddress), pair.remotePort);

			assertTrue(relay.clients().contains(seen), "The server sees " + seen + ", not a relayed client");
			assertEquals(relay.backAddress(seen), endpoint.getProxyRelay(seen));
		}
	}

	@Test
	void proxyRelaysMustBeAddresses() {
		assertThrows(IllegalArgumentException.class, () -> endpoint.setProxyRelays("relay.example"));
		assertThrows(IllegalArgumentException.class, () -> endpoint.setProxyRelays("10.0.0.0/33"));
	}

	@Test
	void stunMonitorLearnsThePortsMapping() throws Exception {
		InetAddress loopback = InetAddress.getByName("127.0.0.1");

		try (TestStunServer server = new TestStunServer(loopback);
				IceUdpEndpoint.StunMonitor monitor = endpoint.monitorStun(server.address())) {
			long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10);
			IceUdpEndpoint.StunReading reading = monitor.read();

			while (reading.state() != IceUdpEndpoint.StunReading.State.SUCCEEDED && System.nanoTime() < deadline) {
				Thread.sleep(50);
				reading = monitor.read();
			}

			assertEquals(IceUdpEndpoint.StunReading.State.SUCCEEDED, reading.state());
			assertEquals(new InetSocketAddress(loopback, endpoint.getPort()), reading.local());
			// No NAT in between, so the server sees the endpoint's own address.
			assertEquals(reading.local(), reading.mapped());
			assertEquals(1, reading.mappingRevision());
			assertTrue(reading.lastSuccessAge().isPresent());
		}
	}

	@Test
	void allowedAddressesLimitGathering() throws Exception {
		RTCConfiguration config = new RTCConfiguration();
		// TEST-NET-1, which no host has.
		config.portAllocatorConfig.allowedAddresses.add(InetAddress.getByName("192.0.2.1"));

		Peer peer = new Peer(config);
		peer.connection.createDataChannel("data", new RTCDataChannelInit());

		TestSetDescObserver observer = new TestSetDescObserver();
		peer.connection.setLocalDescription(observer);
		observer.get(5, TimeUnit.SECONDS);

		assertTrue(peer.awaitCandidates().isEmpty(), "Gathered on an address that is not allowed");
	}

	/*
	 * The host addresses connections gather on the endpoint, as a server would
	 * publish them.
	 */
	private List<String> endpointHosts() throws Exception {
		List<String> hosts = hostsGatheredOn(endpoint);

		assertFalse(hosts.isEmpty(), "No host candidates on the endpoint");

		return hosts;
	}

	/*
	 * The host addresses a new connection gathers on an endpoint; none if it
	 * cannot bind any.
	 */
	private List<String> hostsGatheredOn(IceUdpEndpoint on) throws Exception {
		Peer probe = new Peer(serverConfig(on));
		probe.connection.createDataChannel("data", new RTCDataChannelInit());
		setLocal(probe);

		return probe.awaitCandidates().stream()
				.map(candidate -> candidate.sdp.split(" ")[4])
				.distinct()
				.toList();
	}

	/*
	 * A client that offers to a server connection which has not answered yet. The
	 * client gets the server's answer with the given credentials and the
	 * endpoint's addresses, and starts its checks.
	 */
	private Client offerToEndpoint(List<String> hosts, String ufrag, String password) throws Exception {
		Peer client = new Peer(new RTCConfiguration());
		Peer server = new Peer(serverConfig());

		client.channel = client.connection.createDataChannel("data", new RTCDataChannelInit());
		client.channel.registerObserver(client.channelObserver(client.channel));

		setLocal(client);
		setRemote(server, client.connection.getLocalDescription());

		TestCreateDescObserver created = new TestCreateDescObserver();
		server.connection.createAnswer(new RTCAnswerOptions(), created);

		// libwebrtc accepts a local description with other ICE credentials unless a
		// field trial forbids it.
		String sdp = created.get(5, TimeUnit.SECONDS).sdp
				.replaceAll("(?m)^a=ice-ufrag:.*$", "a=ice-ufrag:" + ufrag)
				.replaceAll("(?m)^a=ice-pwd:.*$", "a=ice-pwd:" + password);
		RTCSessionDescription answer = new RTCSessionDescription(RTCSdpType.ANSWER, sdp);

		setRemote(client, answer);

		for (String host : hosts) {
			String candidate = "candidate:1 1 udp 2122260223 " + host + " " + endpoint.getPort() + " typ host";
			client.connection.addIceCandidate(new RTCIceCandidate("0", 0, candidate), new TestAddIceObserver());
		}

		String offer = client.connection.getLocalDescription().sdp;
		String clientUfrag = offer.lines()
				.filter(line -> line.startsWith("a=ice-ufrag:"))
				.findFirst()
				.orElseThrow()
				.substring("a=ice-ufrag:".length());

		return new Client(client, server, answer, clientUfrag);
	}

	private List<InetAddress> endpointHostAddresses() throws Exception {
		return toAddresses(endpointHosts());
	}

	/*
	 * The addresses connections gather on, learned on an endpoint of their own,
	 * so that the test's endpoint has bound none of them yet.
	 */
	private List<InetAddress> gatheredAddresses() throws Exception {
		IceUdpEndpoint probe = new IceUdpEndpoint(freePort());

		try {
			List<String> hosts = hostsGatheredOn(probe);

			assertFalse(hosts.isEmpty(), "No host candidates on the probe endpoint");

			return toAddresses(hosts);
		}
		finally {
			probe.dispose();
		}
	}

	private static List<InetAddress> toAddresses(List<String> hosts) throws IOException {
		List<InetAddress> addresses = new ArrayList<>();

		for (String host : hosts) {
			addresses.add(InetAddress.getByName(host));
		}

		return addresses;
	}

	/*
	 * Waits for the failure of one address. Any other one passes only as the
	 * runner's, see assertUnavailable.
	 */
	private static IceUdpEndpoint.BindFailure awaitFailure(BlockingQueue<IceUdpEndpoint.BindFailure> failures,
			InetSocketAddress local) throws InterruptedException {
		IceUdpEndpoint.BindFailure failure;

		while ((failure = failures.poll(10, TimeUnit.SECONDS)) != null) {
			if (failure.address().equals(local)) {
				return failure;
			}

			assertUnavailable(failure);
		}

		throw new AssertionError("No failure reported for " + local);
	}

	/*
	 * Passes for an address the endpoint could not bind only if a socket of the
	 * test's own cannot bind it either: the cause is then the runner's, such as
	 * another process on the port, and not the endpoint's.
	 */
	private static void assertUnavailable(IceUdpEndpoint.BindFailure failure) {
		assertThrows(SocketException.class, () -> new DatagramSocket(failure.address()).close(),
				failure.address() + " can be bound, yet the endpoint failed: " + failure.failure().getMessage());
	}

	/*
	 * A port that is free on every address, as far as a wildcard bind can tell.
	 */
	private static int freePort() throws IOException {
		try (DatagramSocket probe = new DatagramSocket(0)) {
			return probe.getLocalPort();
		}
	}

	/*
	 * A link-local IPv6 address with its scope, on an interface that is up, that
	 * a socket of the test's own can bind; null if there is none.
	 */
	private static Inet6Address bindableLinkLocalAddress() throws SocketException {
		for (NetworkInterface face : Collections.list(NetworkInterface.getNetworkInterfaces())) {
			if (!face.isUp()) {
				continue;
			}

			for (InetAddress address : Collections.list(face.getInetAddresses())) {
				if (address instanceof Inet6Address linkLocal && linkLocal.isLinkLocalAddress()
						&& linkLocal.getScopeId() != 0) {
					try (DatagramSocket probe = new DatagramSocket(new InetSocketAddress(linkLocal, 0))) {
						return linkLocal;
					}
					catch (SocketException e) {
						// Not usable here, such as an address still in duplicate detection.
					}
				}
			}
		}

		return null;
	}

	private static List<Integer> scopes(Collection<InetSocketAddress> addresses) {
		return addresses.stream()
				.map(local -> ((Inet6Address) local.getAddress()).getScopeId())
				.toList();
	}

	private static String derivedPassword(byte[] key, byte[] context, String ufrag) throws Exception {
		Mac mac = Mac.getInstance("HmacSHA256");
		mac.init(new SecretKeySpec(key, "HmacSHA256"));
		mac.update(context);

		byte[] digest = mac.doFinal(ufrag.getBytes(StandardCharsets.US_ASCII));

		return Base64.getEncoder().withoutPadding().encodeToString(Arrays.copyOf(digest, 24));
	}

	private static String randomIceChars(int length) {
		String alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
		StringBuilder builder = new StringBuilder(length);

		for (int i = 0; i < length; i++) {
			builder.append(alphabet.charAt(RANDOM.nextInt(alphabet.length())));
		}

		return builder.toString();
	}

	private RTCConfiguration serverConfig() {
		return serverConfig(endpoint);
	}

	private static RTCConfiguration serverConfig(IceUdpEndpoint on) {
		RTCConfiguration config = new RTCConfiguration();
		config.iceEndpoint = on;
		config.portAllocatorConfig.setDisableTcp(true);

		return config;
	}

	private Pair connect(RTCConfiguration serverConfig) throws Exception {
		Peer client = new Peer(new RTCConfiguration());
		Peer server = new Peer(serverConfig);

		client.channel = client.connection.createDataChannel("data", new RTCDataChannelInit());
		client.channel.registerObserver(client.channelObserver(client.channel));

		negotiate(client, server);

		client.sendCandidatesTo(server);
		server.sendCandidatesTo(client);

		assertTrue(client.channelOpen.await(20, TimeUnit.SECONDS), "Data channel did not open");
		assertTrue(server.channelOpen.await(20, TimeUnit.SECONDS), "Remote data channel did not open");

		return new Pair(client, server);
	}

	private static void negotiate(Peer client, Peer server) throws Exception {
		setLocal(client);
		setRemote(server, client.connection.getLocalDescription());
		setLocal(server);
		setRemote(client, server.connection.getLocalDescription());
	}

	private static void setLocal(Peer peer) throws Exception {
		TestSetDescObserver observer = new TestSetDescObserver();
		peer.connection.setLocalDescription(observer);
		observer.get(5, TimeUnit.SECONDS);
	}

	private static void setRemote(Peer peer, RTCSessionDescription description) throws Exception {
		TestSetDescObserver observer = new TestSetDescObserver();
		peer.connection.setRemoteDescription(description, observer);
		observer.get(5, TimeUnit.SECONDS);
	}

	private static int port(RTCIceCandidate candidate) {
		return Integer.parseInt(candidate.sdp.split(" ")[5]);
	}



	private record Pair(Peer client, Peer server) {

	}



	/*
	 * A PROXY protocol v2 relay for IPv4 clients. Clients send to its front
	 * socket; it passes each client's datagrams on from a source port of its own,
	 * every one with a header, and sends the replies back.
	 */
	private static final class TestRelay implements AutoCloseable {

		private static final byte[] SIGNATURE = { 0x0D, 0x0A, 0x0D, 0x0A, 0x00, 0x0D, 0x0A, 0x51, 0x55, 0x49, 0x54, 0x0A };

		private final InetAddress bind;

		private final InetSocketAddress target;

		private final DatagramSocket front;

		private final Map<InetSocketAddress, DatagramSocket> backs = new ConcurrentHashMap<>();


		TestRelay(InetAddress bind, InetSocketAddress target) throws IOException {
			this.bind = bind;
			this.target = target;
			this.front = new DatagramSocket(new InetSocketAddress(bind, 0));

			Thread.ofPlatform().daemon().start(this::pumpFront);
		}

		int port() {
			return front.getLocalPort();
		}

		Set<InetSocketAddress> clients() {
			return backs.keySet();
		}

		InetSocketAddress backAddress(InetSocketAddress client) {
			return (InetSocketAddress) backs.get(client).getLocalSocketAddress();
		}

		@Override
		public void close() {
			front.close();
			backs.values().forEach(DatagramSocket::close);
		}

		private void pumpFront() {
			byte[] buffer = new byte[65536];

			while (!front.isClosed()) {
				DatagramPacket packet = new DatagramPacket(buffer, buffer.length);

				try {
					front.receive(packet);

					InetSocketAddress client = (InetSocketAddress) packet.getSocketAddress();
					DatagramSocket back = backs.computeIfAbsent(client, this::openBack);
					byte[] relayed = withHeader(client, packet.getData(), packet.getLength());

					back.send(new DatagramPacket(relayed, relayed.length, target));
				}
				catch (IOException e) {
					return;
				}
			}
		}

		private DatagramSocket openBack(InetSocketAddress client) {
			try {
				DatagramSocket back = new DatagramSocket(new InetSocketAddress(bind, 0));

				Thread.ofPlatform().daemon().start(() -> pumpBack(back, client));

				return back;
			}
			catch (IOException e) {
				throw new UncheckedIOException(e);
			}
		}

		private void pumpBack(DatagramSocket back, InetSocketAddress client) {
			byte[] buffer = new byte[65536];

			while (!back.isClosed()) {
				DatagramPacket packet = new DatagramPacket(buffer, buffer.length);

				try {
					back.receive(packet);
					front.send(new DatagramPacket(packet.getData(), packet.getLength(), client));
				}
				catch (IOException e) {
					return;
				}
			}
		}

		private byte[] withHeader(InetSocketAddress client, byte[] data, int length) {
			if (!(client.getAddress() instanceof Inet4Address)) {
				throw new IllegalStateException("IPv4 clients only: " + client);
			}

			ByteBuffer buffer = ByteBuffer.allocate(16 + 12 + length);
			buffer.put(SIGNATURE);
			// Version 2, PROXY command; IPv4 over UDP; 12 bytes of addresses.
			buffer.put((byte) 0x21).put((byte) 0x12).putShort((short) 12);
			buffer.put(client.getAddress().getAddress()).put(target.getAddress().getAddress());
			buffer.putShort((short) client.getPort()).putShort((short) target.getPort());
			buffer.put(data, 0, length);

			return buffer.array();
		}
	}



	/*
	 * Answers STUN binding requests over IPv4 with the requester's address.
	 */
	private static final class TestStunServer implements AutoCloseable {

		private final DatagramSocket socket;


		TestStunServer(InetAddress bind) throws IOException {
			socket = new DatagramSocket(new InetSocketAddress(bind, 0));

			Thread.ofPlatform().daemon().start(this::serve);
		}

		InetSocketAddress address() {
			return (InetSocketAddress) socket.getLocalSocketAddress();
		}

		@Override
		public void close() {
			socket.close();
		}

		private void serve() {
			byte[] buffer = new byte[2048];

			while (!socket.isClosed()) {
				DatagramPacket packet = new DatagramPacket(buffer, buffer.length);

				try {
					socket.receive(packet);

					ByteBuffer request = ByteBuffer.wrap(packet.getData(), 0, packet.getLength());

					if (packet.getLength() < 20 || request.getShort(0) != 0x0001 || request.getInt(4) != 0x2112A442) {
						continue;
					}

					InetSocketAddress source = (InetSocketAddress) packet.getSocketAddress();
					byte[] address = source.getAddress().getAddress();
					ByteBuffer response = ByteBuffer.allocate(32);

					// Binding success response with one XOR-MAPPED-ADDRESS attribute.
					response.putShort((short) 0x0101).putShort((short) 12).putInt(0x2112A442);
					response.put(packet.getData(), 8, 12);
					response.putShort((short) 0x0020).putShort((short) 8);
					response.put((byte) 0).put((byte) 0x01);
					response.putShort((short) (source.getPort() ^ 0x2112));
					response.putInt(ByteBuffer.wrap(address).getInt() ^ 0x2112A442);

					socket.send(new DatagramPacket(response.array(), response.capacity(), source));
				}
				catch (IOException e) {
					return;
				}
			}
		}
	}



	private record Client(Peer peer, Peer server, RTCSessionDescription answer, String ufrag) {

		void answerOnServer() throws Exception {
			TestSetDescObserver observer = new TestSetDescObserver();
			server.connection.setLocalDescription(answer, observer);
			observer.get(5, TimeUnit.SECONDS);
		}
	}



	private class Peer implements PeerConnectionObserver {

		final RTCPeerConnection connection;

		final List<RTCIceCandidate> candidates = new CopyOnWriteArrayList<>();

		final CountDownLatch gathered = new CountDownLatch(1);

		final CountDownLatch channelOpen = new CountDownLatch(1);

		final Semaphore pairChanges = new Semaphore(0);

		volatile RTCCandidatePairChangeEvent lastPair;

		final BlockingQueue<String> messages = new LinkedBlockingQueue<>();

		volatile RTCDataChannel channel;

		/* Set once the descriptions are in place, guarded by this peer. */
		private Consumer<RTCIceCandidate> forward;

		private final List<RTCIceCandidate> pending = new ArrayList<>();


		Peer(RTCConfiguration config) {
			connection = factory.createPeerConnection(config, this);
			peers.add(this);
		}

		@Override
		public void onIceCandidate(RTCIceCandidate candidate) {
			candidates.add(candidate);

			Consumer<RTCIceCandidate> target;

			synchronized (this) {
				if (forward == null) {
					pending.add(candidate);
					return;
				}
				target = forward;
			}

			target.accept(candidate);
		}

		@Override
		public void onIceGatheringChange(RTCIceGatheringState state) {
			if (state == RTCIceGatheringState.COMPLETE) {
				gathered.countDown();
			}
		}

		@Override
		public void onSelectedCandidatePairChanged(RTCCandidatePairChangeEvent event) {
			lastPair = event;
			pairChanges.release();
		}

		@Override
		public void onDataChannel(RTCDataChannel dataChannel) {
			channel = dataChannel;
			dataChannel.registerObserver(channelObserver(dataChannel));

			if (dataChannel.getState() == RTCDataChannelState.OPEN) {
				channelOpen.countDown();
			}
		}

		List<RTCIceCandidate> awaitCandidates() throws InterruptedException {
			assertTrue(gathered.await(20, TimeUnit.SECONDS), "Gathering did not complete");

			return new ArrayList<>(candidates);
		}

		void sendCandidatesTo(Peer remote) {
			List<RTCIceCandidate> gathered;

			synchronized (this) {
				forward = candidate -> remote.connection.addIceCandidate(candidate, new TestAddIceObserver());
				gathered = new ArrayList<>(pending);
				pending.clear();
			}

			gathered.forEach(forward);
		}

		void send(String message) throws Exception {
			channel.send(new RTCDataChannelBuffer(ByteBuffer.wrap(message.getBytes(StandardCharsets.UTF_8)), false));
		}

		String nextMessage() throws InterruptedException {
			return messages.poll(10, TimeUnit.SECONDS);
		}

		RTCDataChannelObserver channelObserver(RTCDataChannel dataChannel) {
			return new RTCDataChannelObserver() {
				@Override
				public void onBufferedAmountChange(long previousAmount) { }

				@Override
				public void onStateChange() {
					if (dataChannel.getState() == RTCDataChannelState.OPEN) {
						channelOpen.countDown();
					}
				}

				@Override
				public void onMessage(RTCDataChannelBuffer buffer) {
					byte[] bytes = new byte[buffer.data.remaining()];
					buffer.data.duplicate().get(bytes);
					messages.add(new String(bytes, StandardCharsets.UTF_8));
				}
			};
		}

		void close() {
			RTCDataChannel dataChannel = channel;

			if (dataChannel != null) {
				dataChannel.unregisterObserver();
			}

			connection.close();

			if (dataChannel != null) {
				dataChannel.dispose();
			}
		}
	}
}
