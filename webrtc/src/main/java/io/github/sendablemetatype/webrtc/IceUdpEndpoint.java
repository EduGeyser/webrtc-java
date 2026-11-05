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

import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.UnknownHostException;
import java.nio.ByteBuffer;
import java.time.Duration;
import java.util.Map;
import java.util.Objects;
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
 * first connection gathers on that address. One endpoint can serve the
 * connections of several {@link PeerConnectionFactory} instances. Close the
 * connections before disposing the endpoint, or they stop sending and receiving.
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
 *
 * @author Alex Andres
 */
public class IceUdpEndpoint extends DisposableNativeObject {

	private volatile RequestDispatch requestDispatch;

	/**
	 * Creates an endpoint on the given port.
	 *
	 * @param port      The UDP port every socket of the endpoint binds, 1 to
	 *                  65535.
	 * @param addresses The local addresses to bind, or none to bind every
	 *                  address a connection gathers on.
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
		requestDispatch = new RequestDispatch(Objects.requireNonNull(handler, "handler"),
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
	private void onPendingRequest(long id, byte[] localAddress, int localPort, byte[] remoteAddress,
			int remotePort, String localUfrag, String remoteUfrag) {
		RequestDispatch dispatch = requestDispatch;
		PendingRequest request;

		try {
			request = new PendingRequest(this, id,
					new InetSocketAddress(InetAddress.getByAddress(localAddress), localPort),
					new InetSocketAddress(InetAddress.getByAddress(remoteAddress), remotePort),
					localUfrag, remoteUfrag);
		}
		catch (UnknownHostException e) {
			// The native side passes 4 or 16 bytes.
			throw new IllegalStateException(e);
		}

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

	private native void initialize(int port, InetAddress[] addresses);

	private native void setPendingLimits(int maxPending, long timeoutMs);

	private native void enablePendingRequests(boolean enabled);

	private native void setDerivedPasswords(byte[] context, String[] prefixes, byte[][] keys);

	private native void rejectRequest(long id);

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



	private record RequestDispatch(Consumer<PendingRequest> handler, Executor executor) {

	}

}
