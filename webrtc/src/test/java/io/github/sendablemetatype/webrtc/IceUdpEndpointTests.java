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

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.net.DatagramSocket;
import java.net.InetAddress;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Base64;
import java.util.List;
import java.util.Map;
import java.util.concurrent.BlockingQueue;
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

	private final ExecutorService requestExecutor = Executors.newSingleThreadExecutor();


	@BeforeEach
	void createEndpoint() throws Exception {
		try (DatagramSocket socket = new DatagramSocket(0)) {
			endpoint = new IceUdpEndpoint(socket.getLocalPort());
		}
	}

	@AfterEach
	void dispose() {
		peers.forEach(Peer::close);
		endpoint.dispose();
		requestExecutor.shutdownNow();
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
	}

	@Test
	void iceRestartMovesToTheNewSession() throws Exception {
		Pair pair = connect(serverConfig());

		pair.client.connection.restartIce();
		negotiate(pair.client, pair.server);

		// Checks of the new session come from the same addresses as the old ones,
		// so the pair only changes if the endpoint hands them to the new session.
		assertTrue(pair.server.pairChanges.tryAcquire(2, 20, TimeUnit.SECONDS), "No pair of the new session");

		pair.client.send("after restart");

		assertEquals("after restart", pair.server.nextMessage());
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

		String ufrag = "TST1" + randomIceChars(12);
		Client client = offerToEndpoint(hosts, ufrag, derivedPassword(key, context, ufrag));

		IceUdpEndpoint.PendingRequest request = requests.poll(10, TimeUnit.SECONDS);

		assertNotNull(request, "An authentic check was not passed on");
		assertEquals(ufrag, request.localUfrag);

		client.answerOnServer();

		assertTrue(client.peer.channelOpen.await(20, TimeUnit.SECONDS), "Data channel did not open");
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
		Peer probe = new Peer(serverConfig());
		probe.connection.createDataChannel("data", new RTCDataChannelInit());
		setLocal(probe);

		List<String> hosts = probe.awaitCandidates().stream()
				.map(candidate -> candidate.sdp.split(" ")[4])
				.distinct()
				.toList();

		assertFalse(hosts.isEmpty(), "No host candidates on the endpoint");

		return hosts;
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
		RTCConfiguration config = new RTCConfiguration();
		config.iceEndpoint = endpoint;
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
