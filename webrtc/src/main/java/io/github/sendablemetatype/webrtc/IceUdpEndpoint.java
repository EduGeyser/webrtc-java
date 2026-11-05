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
import java.nio.ByteBuffer;
import java.util.Objects;

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
 *
 * @author Alex Andres
 */
public class IceUdpEndpoint extends DisposableNativeObject {

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
	 * Closes the endpoint's sockets and releases it. Connections that still use
	 * the endpoint stop sending and receiving.
	 */
	@Override
	public native void dispose();

	private native void initialize(int port, InetAddress[] addresses);

	private native void inject(InetAddress localAddress, int localPort, InetAddress remoteAddress,
			int remotePort, byte[] data);

}
