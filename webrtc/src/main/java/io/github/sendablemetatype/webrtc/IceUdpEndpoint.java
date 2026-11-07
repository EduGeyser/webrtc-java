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

import io.github.sendablemetatype.webrtc.internal.DisposableNativeObject;

import java.io.UncheckedIOException;
import java.net.BindException;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.SocketException;
import java.nio.ByteBuffer;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;
import java.util.function.Consumer;

/**
 * One UDP port shared by many peer connections, a server's ICE port. Each
 * connection created with {@link RTCConfiguration#iceEndpoint} gathers its UDP
 * host candidates on the endpoint's sockets instead of opening ports of its
 * own, and the endpoint routes every datagram to the connection it belongs to.
 * A connectivity check from a new address is accepted only if it
 * authenticates with the local ICE credentials of a connection. Datagrams stay
 * in native code.
 * <p>
 * The endpoint binds one socket per local address, on the same port, when the
 * first connection gathers on that address or a {@linkplain #monitorStun STUN
 * monitor} needs it, and keeps it until it is disposed. {@link #getSockets()}
 * tells which addresses it holds and why it could not bind others, for example
 * because another process has the port there, and a
 * {@linkplain #setBindFailureHandler handler} learns of each such failure as
 * it happens. One endpoint can serve the connections of several
 * {@link PeerConnectionFactory} instances. Close the connections before
 * disposing the endpoint, or they stop sending and receiving.
 * <p>
 * A connectivity check that no connection can take yet is held, packet and
 * all, and answered once one can. With a {@linkplain #setPendingRequestHandler
 * handler}, that includes checks for which no connection exists: the handler
 * learns about each one and may create a connection whose local ICE ufrag is
 * the request's and whose password the request authenticates with. As soon as
 * that connection has gathered on the address, the endpoint verifies the held
 * check against it and hands it over, so the remote side gets its answer
 * without retransmitting. A server can so admit a client before it has created
 * any state for it.
 * <p>
 * Behind {@linkplain #setProxyRelays PROXY protocol relays}, connections see the
 * clients the relays declare, and replies reach each client through its relay.
 * A {@linkplain #monitorStun STUN monitor} keeps the port's NAT mapping open and
 * known while no connection runs.
 *
 * @author Alex Andres
 */
public class IceUdpEndpoint extends DisposableNativeObject {

	private volatile Dispatch<PendingRequest> requestDispatch;

	private volatile Dispatch<BindFailure> bindFailureDispatch;

	/**
	 * Creates an endpoint on the given port.
	 *
	 * @param port      The UDP port every socket of the endpoint binds, 1 to
	 *                  65535.
	 * @param addresses The local addresses to bind, or none to bind every
	 *                  address a connection gathers on. Connections on the
	 *                  endpoint gather on these addresses only, as with
	 *                  {@link PortAllocatorConfig#allowedAddresses}. An IPv6
	 *                  address keeps its scope, which binding a link-local
	 *                  address needs.
	 */
	public IceUdpEndpoint(int port, InetAddress... addresses) {
		if (port < 1 || port > 65535) {
			throw new IllegalArgumentException("Port out of range: " + port);
		}

		for (InetAddress address : addresses) {
			Objects.requireNonNull(address, "address");
		}

		initialize(port, addresses.clone());
	}

	/**
	 * Returns the port every socket of this endpoint binds.
	 *
	 * @return The UDP port.
	 */
	public native int getPort();

	/**
	 * Returns the local addresses the endpoint holds a socket on so far, and
	 * those it tried to bind and could not, as they are when this is called.
	 * The result is a snapshot that later changes leave alone; each call takes
	 * a new one.
	 *
	 * @return A snapshot of the endpoint's sockets.
	 *
	 * @throws IllegalStateException if the endpoint is disposed.
	 */
	public native Sockets getSockets();

	/**
	 * Feeds a datagram into the endpoint's routing as if the socket for the
	 * local address had received it. This lets an application that reads the
	 * port itself, for example to share it with another protocol, pass the ICE,
	 * DTLS and SCTP datagrams on.
	 *
	 * @param local  The local address the datagram arrived at.
	 * @param remote The address it came from.
	 * @param data   The datagram, from its position to its limit. The bytes are
	 *               copied.
	 */
	public void inject(InetSocketAddress local, InetSocketAddress remote, ByteBuffer data) {
		InetAddress localAddress = Objects.requireNonNull(local.getAddress(), "local address unresolved");
		InetAddress remoteAddress = Objects.requireNonNull(remote.getAddress(), "remote address unresolved");

		byte[] bytes = new byte[data.remaining()];
		data.duplicate().get(bytes);

		inject(localAddress, local.getPort(), remoteAddress, remote.getPort(), bytes);
	}

	/**
	 * Limits the checks held at once and how long each is held. The defaults are
	 * 1024 checks and 15 seconds. Applies to checks held from now on.
	 *
	 * @param maxPending The most checks held at once, at least 1.
	 * @param timeout    How long a check is held at most, at least 1 ms.
	 */
	public void setPendingLimits(int maxPending, Duration timeout) {
		if (maxPending < 1) {
			throw new IllegalArgumentException("maxPending must be at least 1: " + maxPending);
		}
		if (timeout.toMillis() < 1) {
			throw new IllegalArgumentException("timeout must be at least 1 ms: " + timeout);
		}

		setPendingLimits(maxPending, timeout.toMillis());
	}

	/**
	 * Holds checks for which no connection exists and passes each to the handler
	 * on the executor. The handler either creates a connection for the request
	 * or {@linkplain PendingRequest#reject() rejects} it; a request it leaves
	 * alone expires. A request the executor rejects is rejected. Retransmissions
	 * of a held check are not passed on again.
	 * <p>
	 * Without {@linkplain #setDerivedPasswords derived passwords}, every such
	 * check is passed on, authenticated or not.
	 *
	 * @param handler  Receives the requests.
	 * @param executor Runs the handler, off the endpoint's thread.
	 */
	public void setPendingRequestHandler(Consumer<PendingRequest> handler, Executor executor) {
		requestDispatch = new Dispatch<>(Objects.requireNonNull(handler, "handler"),
				Objects.requireNonNull(executor, "executor"));

		enablePendingRequests(true);
	}

	/**
	 * Stops passing on checks for which no connection exists; they are dropped
	 * from now on. Once this returns, the handler is not passed further requests.
	 */
	public void clearPendingRequestHandler() {
		enablePendingRequests(false);

		requestDispatch = null;
	}

	/**
	 * Passes each failed bind of the endpoint's port to the handler on the
	 * executor: when a connection gathers on a local address, or a STUN monitor
	 * needs one, that the endpoint cannot bind, for example because another
	 * process has the port there. A connection then gathers no host candidate
	 * on that address. Every attempt is passed on, so an address that stays
	 * unavailable is reported once for each connection that gathers on it.
	 * {@link #getSockets()} keeps the latest failure of each address. A failure
	 * the executor rejects is not passed on.
	 *
	 * @param handler  Receives the failures.
	 * @param executor Runs the handler, off the endpoint's thread.
	 */
	public void setBindFailureHandler(Consumer<BindFailure> handler, Executor executor) {
		bindFailureDispatch = new Dispatch<>(Objects.requireNonNull(handler, "handler"),
				Objects.requireNonNull(executor, "executor"));

		enableBindFailures(true);
	}

	/**
	 * Stops passing on failed binds. Once this returns, the handler is not
	 * passed further failures.
	 */
	public void clearBindFailureHandler() {
		enableBindFailures(false);

		bindFailureDispatch = null;
	}

	/**
	 * Makes the endpoint pass on a check for which no connection exists only if
	 * its MESSAGE-INTEGRITY verifies against a password derived from its local
	 * ufrag. The key is chosen by ufrag prefix, and the password is the unpadded
	 * standard base64 encoding of the first 24 bytes of
	 * {@code HMAC-SHA256(key, context || ufrag)}. Checks that match no prefix or
	 * fail verification are dropped without reaching Java. The endpoint keeps
	 * copies; the caller may wipe its arrays afterwards.
	 *
	 * @param context The bytes the ufrag is appended to.
	 * @param keys    The HMAC key for each ufrag prefix.
	 */
	public void setDerivedPasswords(byte[] context, Map<String, byte[]> keys) {
		Objects.requireNonNull(context, "context");

		String[] prefixes = new String[keys.size()];
		byte[][] secrets = new byte[keys.size()][];
		int i = 0;

		for (Map.Entry<String, byte[]> entry : keys.entrySet()) {
			if (entry.getKey().isEmpty()) {
				throw new IllegalArgumentException("Empty ufrag prefix");
			}

			prefixes[i] = entry.getKey();
			secrets[i] = Objects.requireNonNull(entry.getValue(), "key");
			i++;
		}

		setDerivedPasswords(context, prefixes, secrets);
	}

	/**
	 * Removes the derived passwords and wipes the endpoint's copies. Checks for
	 * which no connection exists are passed on unverified again.
	 */
	public void clearDerivedPasswords() {
		setDerivedPasswords(new byte[0], new String[0], new byte[0][]);
	}

	/**
	 * Takes PROXY protocol version 2 headers from relays at these addresses. A
	 * relay prefixes a client's datagrams with a header that declares the
	 * client, and sends them from one source port per client. Connections then
	 * see the declared client as the remote address.
	 * <p>
	 * A header alone only attributes the relay's datagrams to the client. Replies
	 * go back through the relay once a connectivity check it brought
	 * authenticates for a connection, so a relay cannot claim arbitrary clients.
	 * A relay port that routes one client passes to another only once the
	 * first one's connection stops receiving.
	 * <p>
	 * While relays are set, nothing leaves the endpoint straight except STUN
	 * server lookups: everything else to a destination no relay routes is
	 * dropped, connectivity checks to signaled candidates included. A listed
	 * relay may leave out the header after the first datagram of a flow; a
	 * header from anywhere else is dropped, as is any other datagram outside a
	 * flow, except STUN responses. TURN is not affected, as it uses sockets of
	 * its own.
	 *
	 * @param relays Numeric addresses or CIDR ranges, such as
	 *               {@code "203.0.113.7"} or {@code "2001:db8::/32"}. A new list
	 *               keeps the clients of relays it still lists; none clears the
	 *               list.
	 *
	 * @throws IllegalArgumentException if an entry is neither.
	 */
	public void setProxyRelays(String... relays) {
		for (String relay : relays) {
			Objects.requireNonNull(relay, "relay");
		}

		if (!applyProxyRelays(relays.clone())) {
			throw new IllegalArgumentException("Not a numeric address or CIDR range in " + String.join(", ", relays));
		}
	}

	/**
	 * Returns the relay a client comes through, including one whose check is
	 * still held.
	 *
	 * @param client The client as a relay's header declared it.
	 *
	 * @return The relay's address and source port, or {@code null} if no relay
	 *         speaks for the client.
	 */
	public InetSocketAddress getProxyRelay(InetSocketAddress client) {
		InetAddress address = Objects.requireNonNull(client.getAddress(), "client address unresolved");

		return getProxyRelay(address, client.getPort());
	}

	/**
	 * Keeps a binding with a STUN server alive from the endpoint's port, on the
	 * local address the system would send to the server from.
	 *
	 * @param server The STUN server, resolved.
	 *
	 * @return The monitor, to read and to close.
	 *
	 * @see #monitorStun(InetAddress, InetSocketAddress)
	 */
	public StunMonitor monitorStun(InetSocketAddress server) {
		InetAddress serverAddress = Objects.requireNonNull(server.getAddress(), "server unresolved");

		try (DatagramSocket probe = new DatagramSocket()) {
			// Connecting a UDP socket sends nothing; it only picks the route.
			probe.connect(serverAddress, server.getPort());

			return monitorStun(probe.getLocalAddress(), server);
		}
		catch (SocketException e) {
			throw new UncheckedIOException(e);
		}
	}

	/**
	 * Keeps a binding with a STUN server alive from the endpoint's port on a
	 * local address, so that the port's NAT mapping stays open, and known, with
	 * no connection running. The monitor sends a binding request every 15
	 * seconds, retransmitted while unanswered, with no credentials, so that
	 * relays let it through. A server can so publish its public address before
	 * any client connects.
	 *
	 * @param local  The local address to send from, one the endpoint allows. A
	 *               link-local one needs its scope, unless the endpoint lists
	 *               it with one.
	 * @param server The STUN server, resolved.
	 *
	 * @return The monitor, to read and to close.
	 *
	 * @throws IllegalStateException if the endpoint cannot open its socket on
	 *                               the local address.
	 */
	public StunMonitor monitorStun(InetAddress local, InetSocketAddress server) {
		Objects.requireNonNull(local, "local");
		InetAddress serverAddress = Objects.requireNonNull(server.getAddress(), "server unresolved");

		long id = openStunMonitor(local, serverAddress, server.getPort());

		if (id == 0) {
			throw new IllegalStateException("Cannot monitor STUN from " + local.getHostAddress() + " on port " + getPort());
		}

		return new StunMonitor(this, id);
	}

	/**
	 * Returns the endpoint's counters since it was created, and its current
	 * routes, sessions and held checks.
	 *
	 * @return The statistics.
	 */
	public native Stats getStats();

	/**
	 * Closes the endpoint's sockets and releases it. Connections that still use
	 * the endpoint stop sending and receiving.
	 */
	@Override
	public native void dispose();

	/*
	 * Called from native code on the endpoint's thread, which must not wait for
	 * the handler.
	 */
	@SuppressWarnings("unused")
	private void onPendingRequest(long id, InetSocketAddress localAddress, InetSocketAddress remoteAddress,
			String localUfrag, String remoteUfrag) {
		Dispatch<PendingRequest> dispatch = requestDispatch;
		PendingRequest request = new PendingRequest(this, id, localAddress, remoteAddress, localUfrag, remoteUfrag);

		if (dispatch == null) {
			request.reject();
			return;
		}

		try {
			dispatch.executor.execute(() -> dispatch.handler.accept(request));
		}
		catch (RejectedExecutionException e) {
			request.reject();
		}
	}

	/*
	 * Called from native code on the endpoint's thread, which must not wait for
	 * the handler.
	 */
	@SuppressWarnings("unused")
	private void onBindFailure(InetSocketAddress address, String reason) {
		Dispatch<BindFailure> dispatch = bindFailureDispatch;

		if (dispatch == null) {
			return;
		}

		BindFailure failure = bindFailure(address, reason);

		try {
			dispatch.executor.execute(() -> dispatch.handler.accept(failure));
		}
		catch (RejectedExecutionException e) {
			// getSockets() still has it.
		}
	}

	private native void initialize(int port, InetAddress[] addresses);

	private native void setPendingLimits(int maxPending, long timeoutMs);

	private native void enablePendingRequests(boolean enabled);

	private native void enableBindFailures(boolean enabled);

	private native void setDerivedPasswords(byte[] context, String[] prefixes, byte[][] keys);

	private native void rejectRequest(long id);

	private native boolean applyProxyRelays(String[] relays);

	private native InetSocketAddress getProxyRelay(InetAddress address, int port);

	private native long openStunMonitor(InetAddress local, InetAddress server, int port);

	private native StunReading readStunMonitor(long id);

	private native void closeStunMonitor(long id);

	/*
	 * Called from native code.
	 */
	@SuppressWarnings("unused")
	private static Stats stats(long datagramsReceived, long bytesReceived, long datagramsSent, long bytesSent,
			long unroutable, long unauthenticated, long pendingDropped, long announced, long replayed,
			long expired, long rejected, long relayDropped, long relayFiltered, long routes, long sessions,
			long pending) {
		return new Stats(datagramsReceived, bytesReceived, datagramsSent, bytesSent, unroutable,
				unauthenticated, pendingDropped, announced, replayed, expired, rejected, relayDropped,
				relayFiltered, routes, sessions, pending);
	}

	/*
	 * Called from native code. reasons[i] is why failed[i] could not be bound.
	 */
	@SuppressWarnings("unused")
	private static Sockets sockets(InetSocketAddress[] bound, InetSocketAddress[] failed, String[] reasons) {
		List<BindFailure> failures = new ArrayList<>(failed.length);

		for (int i = 0; i < failed.length; i++) {
			failures.add(bindFailure(failed[i], reasons[i]));
		}

		return new Sockets(List.of(bound), failures);
	}

	private static BindFailure bindFailure(InetSocketAddress address, String reason) {
		// The host address names the scope, as in fe80:0:0:0:0:0:0:1%7.
		return new BindFailure(address, new BindException("Cannot bind " + address.getAddress().getHostAddress()
				+ " port " + address.getPort() + ": " + reason));
	}

	/*
	 * Called from native code.
	 */
	@SuppressWarnings("unused")
	private static StunReading stunReading(InetSocketAddress server, InetSocketAddress local,
			InetSocketAddress mapped, int state, long successfulResponses, long failedTransactions,
			long mappingRevision, long lastSuccessAgeMs) {
		return new StunReading(server, local, mapped, StunReading.State.values()[state], successfulResponses,
				failedTransactions, mappingRevision,
				lastSuccessAgeMs < 0 ? Optional.empty() : Optional.of(Duration.ofMillis(lastSuccessAgeMs)));
	}

	private native void inject(InetAddress localAddress, int localPort, InetAddress remoteAddress,
			int remotePort, byte[] data);



	/**
	 * A connectivity check that no connection on the endpoint has taken. The
	 * endpoint holds it until a connection whose local ICE ufrag is
	 * {@link #localUfrag} and whose password it authenticates with has gathered
	 * on {@link #localAddress}, until it is rejected, or until it expires.
	 */
	public static final class PendingRequest {

		/** The endpoint address the check arrived at. */
		public final InetSocketAddress localAddress;

		/** The address the check came from. */
		public final InetSocketAddress remoteAddress;

		/** The local ICE ufrag the check names, the first part of its USERNAME. */
		public final String localUfrag;

		/** The remote side's ICE ufrag, the second part of its USERNAME. */
		public final String remoteUfrag;

		private final IceUdpEndpoint endpoint;

		private final long id;


		private PendingRequest(IceUdpEndpoint endpoint, long id, InetSocketAddress localAddress,
				InetSocketAddress remoteAddress, String localUfrag, String remoteUfrag) {
			this.endpoint = endpoint;
			this.id = id;
			this.localAddress = localAddress;
			this.remoteAddress = remoteAddress;
			this.localUfrag = localUfrag;
			this.remoteUfrag = remoteUfrag;
		}

		/**
		 * Drops the held check. Has no effect once a connection took it, or after
		 * it expired.
		 */
		public void reject() {
			endpoint.rejectRequest(id);
		}

		@Override
		public String toString() {
			return "PendingRequest[local=" + localAddress + ", remote=" + remoteAddress
					+ ", localUfrag=" + localUfrag + ", remoteUfrag=" + remoteUfrag + "]";
		}
	}



	/**
	 * A STUN binding the endpoint keeps alive. Close it when it is no longer
	 * needed; disposing the endpoint closes it too.
	 */
	public static final class StunMonitor implements AutoCloseable {

		private final IceUdpEndpoint endpoint;

		private final long id;


		private StunMonitor(IceUdpEndpoint endpoint, long id) {
			this.endpoint = endpoint;
			this.id = id;
		}

		/**
		 * Returns the binding's state.
		 *
		 * @return The latest reading.
		 *
		 * @throws IllegalStateException if the monitor or its endpoint is closed.
		 */
		public StunReading read() {
			StunReading reading = endpoint.readStunMonitor(id);

			if (reading == null) {
				throw new IllegalStateException("STUN monitor closed");
			}

			return reading;
		}

		/**
		 * Stops keeping the binding alive.
		 */
		@Override
		public void close() {
			endpoint.closeStunMonitor(id);
		}
	}



	/**
	 * The state of a STUN binding.
	 *
	 * @param server              The STUN server.
	 * @param local               The endpoint address the binding is kept from.
	 * @param mapped              The address the server saw, from its latest
	 *                            answer, or {@code null} before the first one.
	 * @param state               The outcome of the latest transaction that
	 *                            completed.
	 * @param successfulResponses The answers so far.
	 * @param failedTransactions  The transactions that got an error or no answer
	 *                            in time.
	 * @param mappingRevision     How often the mapped address changed, the first
	 *                            answer included.
	 * @param lastSuccessAge      How long ago the latest answer came.
	 */
	public record StunReading(InetSocketAddress server, InetSocketAddress local, InetSocketAddress mapped,
			State state, long successfulResponses, long failedTransactions, long mappingRevision,
			Optional<Duration> lastSuccessAge) {

		/** The outcome of a binding's latest transaction. */
		public enum State {
			/** No transaction has completed yet. */
			PENDING,
			/** The server answered with a mapped address. */
			SUCCEEDED,
			/** The server answered with an error, or not in time. */
			FAILED
		}
	}



	/**
	 * A local address the endpoint could not bind.
	 *
	 * @param address The address, with its scope, and the endpoint's port.
	 * @param failure Why the system refused it.
	 */
	public record BindFailure(InetSocketAddress address, BindException failure) {

	}



	/**
	 * The sockets of an endpoint, all on its port, as {@link #getSockets()}
	 * found them. Both lists are unmodifiable copies.
	 *
	 * @param bound  The addresses the endpoint held a socket on.
	 * @param failed The addresses it had tried to bind and held no socket on,
	 *               each with the failure of its latest attempt. Java's
	 *               address equality leaves the scope out, so the same
	 *               link-local address may appear once per scope. The
	 *               endpoint tries such an address again when a connection
	 *               gathers on it.
	 */
	public record Sockets(List<InetSocketAddress> bound, List<BindFailure> failed) {

		/**
		 * Copies the lists, keeping their order.
		 */
		public Sockets {
			bound = List.copyOf(bound);
			failed = List.copyOf(failed);
		}
	}



	/**
	 * The counters of an endpoint, and its current state.
	 *
	 * @param datagramsReceived The datagrams the endpoint's sockets received or
	 *                          that were injected.
	 * @param bytesReceived     Their bytes.
	 * @param datagramsSent     The datagrams the endpoint sent.
	 * @param bytesSent         Their bytes.
	 * @param unroutable        Datagrams no connection took: from an address
	 *                          with no route, or for a route another connection
	 *                          still uses.
	 * @param unauthenticated   Connectivity checks that authenticated for no
	 *                          connection and were not held.
	 * @param pendingDropped    Checks not held, as the most allowed were held or
	 *                          they were too large.
	 * @param announced         Held checks passed to the request handler.
	 * @param replayed          Held checks a connection took.
	 * @param expired           Held checks that timed out.
	 * @param rejected          Held checks rejected.
	 * @param relayDropped      Datagrams the relay rules dropped on the way in.
	 * @param relayFiltered     Datagrams not sent, as no relay routes their
	 *                          destination.
	 * @param routes            The current routes from remote addresses to
	 *                          connections.
	 * @param sessions          The current ICE sessions of connections.
	 * @param pending           The checks held now.
	 */
	public record Stats(long datagramsReceived, long bytesReceived, long datagramsSent, long bytesSent,
			long unroutable, long unauthenticated, long pendingDropped, long announced, long replayed, long expired,
			long rejected, long relayDropped, long relayFiltered, long routes, long sessions, long pending) {

	}



	private record Dispatch<T>(Consumer<T> handler, Executor executor) {

	}

}
