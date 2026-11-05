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

import java.time.Duration;

/**
 * Metrics of the transport that carries a connection's data channels, read
 * directly instead of through a stats report.
 *
 * @param iceRoundTripTime The latest STUN round trip time of the selected
 *                         candidate pair, or {@code null} before one is
 *                         measured.
 * @param sctp             The SCTP association's metrics, or {@code null}
 *                         before it starts.
 *
 * @see RTCPeerConnection#getTransportMetrics()
 */
public record RTCTransportMetrics(Duration iceRoundTripTime, RTCSctpMetrics sctp) {

	/*
	 * Called from native code. -1 marks a round trip time not measured yet.
	 */
	@SuppressWarnings("unused")
	private static RTCTransportMetrics create(long iceRoundTripTimeMs, boolean hasSctp,
			long smoothedRoundTripTimeMs, long congestionWindowBytes, long unackedDataCount,
			long peerReceiveWindowBytes, long packetsSent, long messagesSent, long packetsRetransmitted,
			long bytesRetransmitted, long packetsReceived, long messagesReceived) {
		Duration iceRoundTripTime = iceRoundTripTimeMs < 0 ? null : Duration.ofMillis(iceRoundTripTimeMs);
		RTCSctpMetrics sctp = hasSctp
				? new RTCSctpMetrics(Duration.ofMillis(smoothedRoundTripTimeMs), congestionWindowBytes,
						unackedDataCount, peerReceiveWindowBytes, packetsSent, messagesSent, packetsRetransmitted,
						bytesRetransmitted, packetsReceived, messagesReceived)
				: null;

		return new RTCTransportMetrics(iceRoundTripTime, sctp);
	}

}
