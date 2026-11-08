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

	private RTCPeerConnection localPeerConnection;

	private RTCPeerConnection remotePeerConnection;


	TestPeerConnection(PeerConnectionFactory factory) {
		RTCConfiguration config = new RTCConfiguration();

		localPeerConnection = factory.createPeerConnection(config, this);
		localPeerConnection.createDataChannel("dummy", new RTCDataChannelInit());

		connectedLatch = new CountDownLatch(1);
	}

	@Override
	public void onIceCandidate(RTCIceCandidate candidate) {
		RTCPeerConnection remote = remotePeerConnection;

		candidateForwarder.execute(() -> remote.addIceCandidate(candidate));
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

	void close() {
		candidateForwarder.shutdownNow();

		if (nonNull(localPeerConnection)) {
			localPeerConnection.close();
			localPeerConnection = null;
		}
	}
}
