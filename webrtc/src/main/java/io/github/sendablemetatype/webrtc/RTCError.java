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
 * An error WebRTC reported, with its kind, the part of the connection it comes
 * from and, for an SCTP failure, the SCTP cause code.
 *
 * @author Alex Andres
 *
 * @see RTCDataChannel#getError()
 */
public class RTCError {

	private final RTCErrorType type;

	private final RTCErrorDetailType detail;

	private final Integer sctpCauseCode;

	private final String message;


	RTCError(RTCErrorType type, RTCErrorDetailType detail,
			Integer sctpCauseCode, String message) {
		this.type = type;
		this.detail = detail;
		this.sctpCauseCode = sctpCauseCode;
		this.message = message;
	}

	/**
	 * Returns the kind of the error.
	 *
	 * @return The error type.
	 */
	public RTCErrorType getType() {
		return type;
	}

	/**
	 * Returns which part of the connection the error comes from.
	 *
	 * @return The error detail type, {@link RTCErrorDetailType#NONE} if WebRTC
	 * gave none.
	 */
	public RTCErrorDetailType getDetail() {
		return detail;
	}

	/**
	 * Returns the SCTP cause code of an SCTP failure, for example 12 (User
	 * Initiated Abort) when the peer aborted the association. See RFC 9260,
	 * section 3.3.10.
	 *
	 * @return The SCTP cause code, or {@code null} if the error carries none.
	 */
	public Integer getSctpCauseCode() {
		return sctpCauseCode;
	}

	/**
	 * Returns WebRTC's description of the error.
	 *
	 * @return The error message, or {@code null} if there is none.
	 */
	public String getMessage() {
		return message;
	}

	@Override
	public String toString() {
		return String.format("%s@%d [type=%s, detail=%s, sctpCauseCode=%s, message=%s]",
				RTCError.class.getSimpleName(), hashCode(), type, detail,
				sctpCauseCode, message);
	}
}
