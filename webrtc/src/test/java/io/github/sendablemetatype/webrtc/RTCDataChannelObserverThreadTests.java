package io.github.sendablemetatype.webrtc;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import io.github.sendablemetatype.webrtc.media.audio.AudioDeviceModule;
import io.github.sendablemetatype.webrtc.media.audio.AudioLayer;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Supplier;

import org.junit.jupiter.api.Test;

/**
 * Which thread calls a data channel observer: the signaling thread by
 * default, the network thread for an observer that asks for it.
 * <p>
 * The network thread cannot be named from Java, so the tests hold the
 * signaling thread in a callback and check that a message still arrives,
 * which it cannot if WebRTC posts it to the signaling thread.
 */
class RTCDataChannelObserverThreadTests extends TestBase {

	@Test
	void anObserverThatKeepsTheDefaultRunsOnTheSignalingThread() throws Exception {
		try (Pair pair = new Pair(factory, false)) {
			pair.connect();
			pair.send("default");

			Thread signaling = pair.signalingThread.get(5, TimeUnit.SECONDS);

			assertFalse(pair.receiverObserver.isOkToCallOnTheNetworkThread());
			assertSame(signaling, pair.messageThread.get(5, TimeUnit.SECONDS));
			assertEquals(Set.of(signaling), pair.stateThreads);
		}
	}

	@Test
	void networkThreadObserverRunsWhileTheSignalingThreadIsHeld() throws Exception {
		try (Pair pair = new Pair(factory, true)) {
			pair.connect();

			CountDownLatch received = new CountDownLatch(1);
			pair.onMessage = () -> received.countDown();

			Hold hold = Hold.signalingThread(pair.caller, received);
			pair.send("network");

			assertTrue(hold.released.get(10, TimeUnit.SECONDS), "The message waited for the signaling thread");

			Thread signaling = pair.signalingThread.get(5, TimeUnit.SECONDS);
			Thread network = pair.messageThread.get(5, TimeUnit.SECONDS);

			assertSame(signaling, hold.thread);
			assertNotSame(signaling, network);
			// Both channels' state changes, on the one network thread.
			assertEquals(Set.of(network), pair.stateThreads);
			// Read when registering, not for each event.
			assertEquals(1, pair.senderAsked.get());
			assertEquals(1, pair.receiverAsked.get());
		}
	}

	@Test
	void networkThreadCallbackCannotUnregisterButCanDispose() throws Exception {
		try (Pair pair = new Pair(factory, true)) {
			pair.connect();

			CompletableFuture<Throwable> unregistered = new CompletableFuture<>();
			CountDownLatch disposed = new CountDownLatch(1);
			pair.onMessage = () -> {
				try {
					pair.receiver.unregisterObserver();
					unregistered.complete(null);
				}
				catch (Throwable e) {
					unregistered.complete(e);
				}
				// Waits for the signaling thread, unless the release is handed on.
				pair.receiver.dispose();
				disposed.countDown();
			};

			Hold hold = Hold.signalingThread(pair.caller, disposed);
			pair.send("dispose");

			assertTrue(hold.released.get(10, TimeUnit.SECONDS), "The dispose waited for the signaling thread");
			assertInstanceOf(IllegalStateException.class, unregistered.get(5, TimeUnit.SECONDS));
			assertThrows(IllegalStateException.class, pair.receiver::getState);
		}
	}

	/**
	 * A release handed to the callback thread still needs the factory's
	 * threads, so a dispose of the factory before it has run must leave them
	 * until it is done. Without that, the release runs on freed threads and
	 * crashes or hangs, and the marker queued behind it never runs.
	 */
	@Test
	void aReleaseHandedOnFromACallbackFinishesBeforeTheFactoryStops() throws Exception {
		AudioDeviceModule module = DATA_CHANNELS_ONLY ? null : new AudioDeviceModule(AudioLayer.kDummyAudio);
		PeerConnectionFactory own = module == null ? new PeerConnectionFactory() : new PeerConnectionFactory(module);
		Pair pair = new Pair(own, true);
		TestDataChannelPair marker = new TestDataChannelPair(factory);
		CountDownLatch blocked = new CountDownLatch(1);
		CountDownLatch unblock = new CountDownLatch(1);

		try {
			pair.connect();
			marker.connect();

			CountDownLatch disposed = new CountDownLatch(1);
			pair.onMessage = () -> {
				pair.receiver.dispose();
				disposed.countDown();
			};

			// The send's result is posted to the callback thread before the
			// message reaches the receiver, whose callback then posts the
			// release, so the release waits behind the held result.
			pair.sender.sendAsync(binary("hold"), new RTCDataChannelSendObserver() {
				@Override
				public void onSuccess() {
					blocked.countDown();
					try {
						unblock.await(10, TimeUnit.SECONDS);
					}
					catch (InterruptedException e) {
						Thread.currentThread().interrupt();
					}
				}

				@Override
				public void onFailure(String error) {
					blocked.countDown();
				}
			});

			assertTrue(blocked.await(5, TimeUnit.SECONDS), "The callback thread was not held");
			assertTrue(disposed.await(5, TimeUnit.SECONDS), "The receiver was not disposed in its callback");

			pair.close();
			if (module != null) {
				module.dispose();
			}
			own.dispose();

			CountDownLatch released = new CountDownLatch(1);
			marker.sender.sendAsync(binary("marker"), new RTCDataChannelSendObserver() {
				@Override
				public void onSuccess() {
					released.countDown();
				}

				@Override
				public void onFailure(String error) {
					released.countDown();
				}
			});
			unblock.countDown();

			assertTrue(released.await(10, TimeUnit.SECONDS), "The release queued before the marker did not finish");
		}
		finally {
			unblock.countDown();
			marker.close();
		}
	}

	@Test
	void registerObserverRefusesAMissingOrFailingObserver() {
		RTCPeerConnection connection = factory.createPeerConnection(new RTCConfiguration(), candidate -> { });
		RTCDataChannel channel = connection.createDataChannel("refused", new RTCDataChannelInit());

		try {
			assertThrows(NullPointerException.class, () -> channel.registerObserver(null));

			RuntimeException failure = new RuntimeException("asked");
			RuntimeException thrown = assertThrows(RuntimeException.class, () ->
					channel.registerObserver(new Recorder() {
						@Override
						public boolean isOkToCallOnTheNetworkThread() {
							throw failure;
						}
					}));

			assertSame(failure, thrown);
		}
		finally {
			channel.dispose();
			connection.close();
		}
	}

	private static RTCDataChannelBuffer binary(String text) {
		return new RTCDataChannelBuffer(ByteBuffer.wrap(text.getBytes(StandardCharsets.UTF_8)), true);
	}

	/**
	 * Blocks the signaling thread in a callback until a latch opens, for at
	 * most 5 seconds.
	 */
	private static final class Hold {

		/** Completes with whether the latch opened while the thread was held. */
		final CompletableFuture<Boolean> released = new CompletableFuture<>();

		volatile Thread thread;

		static Hold signalingThread(RTCPeerConnection peer, CountDownLatch until) throws InterruptedException {
			Hold hold = new Hold();
			CountDownLatch holding = new CountDownLatch(1);

			// A created offer is reported on the signaling thread.
			peer.createOffer(new RTCOfferOptions(), new CreateSessionDescriptionObserver() {
				@Override
				public void onSuccess(RTCSessionDescription description) {
					hold.thread = Thread.currentThread();
					holding.countDown();
					try {
						hold.released.complete(until.await(5, TimeUnit.SECONDS));
					}
					catch (InterruptedException e) {
						hold.released.completeExceptionally(e);
					}
				}

				@Override
				public void onFailure(String error) {
					hold.released.completeExceptionally(new IllegalStateException(error));
					holding.countDown();
				}
			});

			assertTrue(holding.await(5, TimeUnit.SECONDS), "The signaling thread was not held");

			return hold;
		}
	}

	/**
	 * Two connected peers of one factory with a negotiated data channel, which
	 * record the threads their observers run on.
	 */
	private static final class Pair implements AutoCloseable {

		final RTCPeerConnection caller;
		final RTCPeerConnection callee;
		final RTCDataChannel sender;
		final RTCDataChannel receiver;
		final RTCDataChannelObserver receiverObserver;

		final CompletableFuture<Thread> signalingThread = new CompletableFuture<>();
		final CompletableFuture<Thread> messageThread = new CompletableFuture<>();
		final Set<Thread> stateThreads = ConcurrentHashMap.newKeySet();
		final AtomicInteger senderAsked = new AtomicInteger();
		final AtomicInteger receiverAsked = new AtomicInteger();

		/** Runs in the receiver's onMessage, before the message thread is recorded. */
		volatile Runnable onMessage = () -> { };

		private final CountDownLatch open = new CountDownLatch(2);

		/**
		 * @param networkThread whether the observers ask for the network
		 *                      thread; if not, they keep the interface's default
		 */
		Pair(PeerConnectionFactory factory, boolean networkThread) {
			RTCPeerConnection[] peers = new RTCPeerConnection[2];
			caller = factory.createPeerConnection(new RTCConfiguration(), new Peer(() -> peers[1]));
			callee = factory.createPeerConnection(new RTCConfiguration(), new Peer(() -> peers[0]));
			peers[0] = caller;
			peers[1] = callee;

			RTCDataChannelInit config = new RTCDataChannelInit();
			config.negotiated = true;
			config.id = 0;
			sender = caller.createDataChannel("threads", config);
			receiver = callee.createDataChannel("threads", config);

			Recorder senderObserver = networkThread ? new NetworkThreadRecorder(senderAsked) : new Recorder();
			Recorder receiverRecorder = networkThread ? new NetworkThreadRecorder(receiverAsked) : new Recorder();
			senderObserver.bind(this, sender, false);
			receiverRecorder.bind(this, receiver, true);
			receiverObserver = receiverRecorder;

			sender.registerObserver(senderObserver);
			receiver.registerObserver(receiverRecorder);
		}

		void connect() throws Exception {
			TestCreateDescObserver offer = new TestCreateDescObserver();
			caller.createOffer(new RTCOfferOptions(), offer);
			RTCSessionDescription offerDescription = offer.get(5, TimeUnit.SECONDS);
			setDescription(caller, offerDescription, true);
			setDescription(callee, offerDescription, false);

			TestCreateDescObserver answer = new TestCreateDescObserver();
			callee.createAnswer(new RTCAnswerOptions(), answer);
			RTCSessionDescription answerDescription = answer.get(5, TimeUnit.SECONDS);
			setDescription(callee, answerDescription, true);
			setDescription(caller, answerDescription, false);

			assertTrue(open.await(5, TimeUnit.SECONDS), "Data channels did not open");
		}

		void send(String text) throws Exception {
			sender.send(binary(text));
		}

		void stateChanged(RTCDataChannel channel) {
			stateThreads.add(Thread.currentThread());
			if (channel.getState() == RTCDataChannelState.OPEN) {
				open.countDown();
			}
		}

		void received() {
			onMessage.run();
			messageThread.complete(Thread.currentThread());
		}

		private static void setDescription(RTCPeerConnection peer,
				RTCSessionDescription description, boolean local) throws Exception {
			TestSetDescObserver observer = new TestSetDescObserver();
			if (local) {
				peer.setLocalDescription(description, observer);
			}
			else {
				peer.setRemoteDescription(description, observer);
			}
			observer.get(5, TimeUnit.SECONDS);
		}

		@Override
		public void close() {
			for (RTCDataChannel channel : new RTCDataChannel[] { sender, receiver }) {
				try {
					channel.unregisterObserver();
				}
				catch (IllegalStateException disposed) {
					// A test disposed it.
				}
			}
			caller.close();
			callee.close();
			sender.dispose();
			receiver.dispose();
		}

		/** Passes candidates on and records the signaling thread. */
		private final class Peer implements PeerConnectionObserver {

			private final Supplier<RTCPeerConnection> remote;

			Peer(Supplier<RTCPeerConnection> remote) {
				this.remote = remote;
			}

			@Override
			public void onIceCandidate(RTCIceCandidate candidate) {
				remote.get().addIceCandidate(candidate);
			}

			@Override
			public void onSignalingChange(RTCSignalingState state) {
				signalingThread.complete(Thread.currentThread());
			}
		}
	}

	/**
	 * Hands a pair's channel events to it, on the thread the interface's
	 * default asks for: this class does not override
	 * {@link #isOkToCallOnTheNetworkThread()}.
	 */
	private static class Recorder implements RTCDataChannelObserver {

		private Pair pair;
		private RTCDataChannel channel;
		private boolean receiver;

		void bind(Pair pair, RTCDataChannel channel, boolean receiver) {
			this.pair = pair;
			this.channel = channel;
			this.receiver = receiver;
		}

		@Override
		public void onBufferedAmountChange(long sentDataSize) {
		}

		@Override
		public void onStateChange() {
			if (pair != null) {
				pair.stateChanged(channel);
			}
		}

		@Override
		public void onMessage(RTCDataChannelBuffer buffer) {
			if (pair != null && receiver) {
				pair.received();
			}
		}
	}

	/** A recorder that asks for the network thread and counts how often it is asked. */
	private static final class NetworkThreadRecorder extends Recorder {

		private final AtomicInteger asked;

		NetworkThreadRecorder(AtomicInteger asked) {
			this.asked = asked;
		}

		@Override
		public boolean isOkToCallOnTheNetworkThread() {
			asked.incrementAndGet();
			return true;
		}
	}
}
