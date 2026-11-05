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

/**
 * Which part of the connection an {@link RTCError} comes from, in the order of
 * WebRTC's {@code RTCErrorDetailType}.
 *
 * @author Alex Andres
 */
public enum RTCErrorDetailType {

	/** No detail. */
	NONE,

	/** The data channel failed. */
	DATA_CHANNEL_FAILURE,

	/** The DTLS transport failed. */
	DTLS_FAILURE,

	/** The remote certificate did not match its fingerprint. */
	FINGERPRINT_FAILURE,

	/**
	 * The SCTP association failed or the peer aborted it; see
	 * {@link RTCError#getSctpCauseCode()}.
	 */
	SCTP_FAILURE,

	/** A session description could not be parsed. */
	SDP_SYNTAX_ERROR,

	/** No hardware encoder is available. */
	HARDWARE_ENCODER_NOT_AVAILABLE,

	/** The hardware encoder failed. */
	HARDWARE_ENCODER_ERROR;

}
