/*
 * Copyright 2019 Alex Andres
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

import static java.util.Objects.nonNull;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.TimeUnit;

/**
 * Convenience test class implementing the basic {@link RTCPeerConnection}
 * functionality to establish a connection to a remote peer.
 *
 * @author Alex Andres
 */
class TestPeerConnection implements PeerConnectionObserver {

	private final CountDownLatch connectedLatch;

	/*
	 * Candidates reach the remote peer off this peer's signaling thread. When
	 * the two peers come from different factories, a synchronous
	 * addIceCandidate from each signaling thread into the other's deadlocks as
	 * soon as both gather at once. One thread keeps the candidates in order.
	 */
	private final ExecutorService candidateForwarder =
			Executors.newSingleThreadExecutor(Thread.ofPlatform().daemon().factory());

	// Cleared once this peer or its remote peer closes; later candidates are dropped.
	private volatile boolean forwarding = true;

	private RTCPeerConnection localPeerConnection;

	private RTCPeerConnection remotePeerConnection;

	private TestPeerConnection remotePeer;


	TestPeerConnection(PeerConnectionFactory factory) {
		RTCConfiguration config = new RTCConfiguration();

		localPeerConnection = factory.createPeerConnection(config, this);
		localPeerConnection.createDataChannel("dummy", new RTCDataChannelInit());

		connectedLatch = new CountDownLatch(1);
	}

	@Override
	public void onIceCandidate(RTCIceCandidate candidate) {
		RTCPeerConnection remote = remotePeerConnection;

		if (!forwarding || remote == null) {
			return;
		}

		try {
			candidateForwarder.execute(() -> {
				if (forwarding) {
					remote.addIceCandidate(candidate);
				}
			});
		}
		catch (RejectedExecutionException e) {
			// Closing: the remote peer no longer needs the candidate.
		}
	}

	@Override
	public void onConnectionChange(RTCPeerConnectionState state) {
		if (state == RTCPeerConnectionState.CONNECTED) {
			connectedLatch.countDown();
		}
	}

	void waitUntilConnected() throws InterruptedException {
		connectedLatch.await();
	}

	RTCSessionDescription createOffer() throws Exception {
		TestCreateDescObserver createObserver = new TestCreateDescObserver();
		TestSetDescObserver setObserver = new TestSetDescObserver();

		localPeerConnection.createOffer(new RTCOfferOptions(), createObserver);

		RTCSessionDescription offerDesc = createObserver.get();

		localPeerConnection.setLocalDescription(offerDesc, setObserver);
		setObserver.get();

		return offerDesc;
	}

	RTCSessionDescription createAnswer() throws Exception {
		TestCreateDescObserver createObserver = new TestCreateDescObserver();
		TestSetDescObserver setObserver = new TestSetDescObserver();

		localPeerConnection.createAnswer(new RTCAnswerOptions(), createObserver);

		RTCSessionDescription answerDesc = createObserver.get();

		localPeerConnection.setLocalDescription(answerDesc, setObserver);
		setObserver.get();

		return answerDesc;
	}

	void setRemotePeerConnection(TestPeerConnection connection) {
		this.remotePeer = connection;
		this.remotePeerConnection = connection.localPeerConnection;
	}

	void setRemoteDescription(RTCSessionDescription description) throws Exception {
		TestSetDescObserver setObserver = new TestSetDescObserver();

		localPeerConnection.setRemoteDescription(description, setObserver);
		setObserver.get();
	}

	RTCPeerConnection getPeerConnection() {
		return localPeerConnection;
	}

	/**
	 * Stops handing candidates to the remote peer and waits for a hand-off
	 * already running, so that once one peer of a pair closes, neither calls
	 * into the other's connection.
	 */
	private void stopForwarding() {
		forwarding = false;
		candidateForwarder.shutdown();

		try {
			candidateForwarder.awaitTermination(10, TimeUnit.SECONDS);
		}
		catch (InterruptedException e) {
			Thread.currentThread().interrupt();
		}
	}

	void close() {
		stopForwarding();

		if (nonNull(remotePeer)) {
			remotePeer.stopForwarding();
		}

		if (nonNull(localPeerConnection)) {
			localPeerConnection.close();
			localPeerConnection = null;
		}
	}
}
