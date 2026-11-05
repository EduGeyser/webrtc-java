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
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.net.DatagramSocket;
import java.net.InetAddress;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;

import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

class IceUdpEndpointTests extends TestBase {

	private IceUdpEndpoint endpoint;

	private final List<Peer> peers = new ArrayList<>();


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
