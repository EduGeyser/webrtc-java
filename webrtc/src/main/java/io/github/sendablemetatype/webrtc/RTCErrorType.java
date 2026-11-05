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
 * The kind of an {@link RTCError}, in the order of WebRTC's
 * {@code RTCErrorType}.
 *
 * @author Alex Andres
 */
public enum RTCErrorType {

	/** No error. */
	NONE,

	/** An operation is valid but not supported. */
	UNSUPPORTED_OPERATION,

	/** A supplied parameter is valid but not supported. */
	UNSUPPORTED_PARAMETER,

	/** A supplied parameter is invalid. */
	INVALID_PARAMETER,

	/** A supplied parameter is out of range. */
	INVALID_RANGE,

	/** A supplied text could not be parsed. */
	SYNTAX_ERROR,

	/** The object is in a state that does not allow the operation. */
	INVALID_STATE,

	/** The operation would modify something that cannot be modified. */
	INVALID_MODIFICATION,

	/** A failure in the network or transport layer. */
	NETWORK_ERROR,

	/** A resource, such as memory or a send buffer, ran out. */
	RESOURCE_EXHAUSTED,

	/** An internal error of the implementation. */
	INTERNAL_ERROR,

	/** An operation error; the detail type says which. */
	OPERATION_ERROR_WITH_DATA;

}
