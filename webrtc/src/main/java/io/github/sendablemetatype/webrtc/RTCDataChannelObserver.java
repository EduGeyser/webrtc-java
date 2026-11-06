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

/**
 * Used to receive events from the {@link RTCDataChannel}.
 *
 * <h2>Threads</h2>
 * WebRTC raises a data channel's events on the network thread of the channel's
 * {@link PeerConnectionFactory}. Where it calls {@link #onStateChange()},
 * {@link #onMessage} and {@link #onBufferedAmountChange} depends on
 * {@link #isOkToCallOnTheNetworkThread()}, which is read once, when the
 * observer is registered:
 * <ul>
 * <li>{@code false}, the default: each event is posted to the factory's
 * signaling thread, and the observer is called there, like a
 * {@link PeerConnectionObserver}. One signaling thread serves every peer
 * connection of the factory, so a slow callback holds up their events too.
 * Inside a callback, {@link RTCDataChannel#getState()},
 * {@link RTCDataChannel#getError()} and {@link RTCDataChannel#getId()}
 * return the values the channel had when the event was raised.</li>
 * <li>{@code true}: the observer is called directly on the network thread,
 * which saves a thread switch per event. That thread carries the network
 * traffic of every peer connection of the factory and is also WebRTC's worker
 * thread, so a callback must return promptly. It may call the methods of the
 * factory's data channels, except {@link RTCDataChannel#registerObserver} and
 * {@link RTCDataChannel#unregisterObserver()}, which throw an
 * {@link IllegalStateException} there. It must not call any other WebRTC
 * method, such as one of {@link RTCPeerConnection}, of the factory or of the
 * objects they return: those wait for the signaling thread, which may be
 * waiting for the network thread, and then every peer connection of the
 * factory stops. Calls into a channel, apart from
 * {@link RTCDataChannel#sendAsync(RTCDataChannelBuffer) sendAsync}, run at
 * once and may call the observer again before they return:
 * {@link RTCDataChannel#close()} reports the closing state from inside the
 * call, and a send that fails can close the channel. The channel's events are
 * not ordered with the peer connection's,
 * which stay on the signaling thread; a channel can report that it is open
 * before the {@link PeerConnectionObserver} hears that the connection is
 * connected.</li>
 * </ul>
 * <p>
 * In both cases, the events of one channel arrive in order and, apart from
 * such nested calls, one at a time. A callback must not wait for a lock or a
 * thread that may itself be waiting for a WebRTC call to return, since that
 * call may need the thread the callback runs on. Messages that arrived
 * before the observer was registered are delivered when
 * it is registered, and any callback may run before
 * {@link RTCDataChannel#registerObserver} returns. An exception thrown by a
 * callback is reported to the calling thread's uncaught exception handler; it
 * does not reach WebRTC.
 *
 * @author Alex Andres
 */
public interface RTCDataChannelObserver {

	/**
	 * The RTCDataChannel's buffered amount has decreased. Notifications may
	 * combine multiple changes. The reported value is a decrease in queued
	 * bytes, not an absolute buffer size or an acknowledgment from the peer.
	 *
	 * @param sentDataSize The number of bytes removed from the buffered amount.
	 */
	void onBufferedAmountChange(long sentDataSize);

	/**
	 * The RTCDataChannel's state has changed.
	 */
	void onStateChange();

	/**
	 * A data buffer was successfully received.
	 * <p>
	 * NOTE: {@link RTCDataChannelBuffer#data} will be freed once this function
	 * returns so observers who want to use the data asynchronously must make
	 * sure to copy it first. This holds on either thread.
	 *
	 * @param buffer The buffer containing the received message.
	 */
	void onMessage(RTCDataChannelBuffer buffer);

	/**
	 * Whether WebRTC may call this observer directly on its network thread,
	 * like libwebrtc's {@code DataChannelObserver::IsOkToCallOnTheNetworkThread()}.
	 * The default, {@code false}, has each event posted to the signaling thread
	 * first. Return {@code true} only if every callback keeps to the rules for
	 * the network thread in the {@linkplain RTCDataChannelObserver interface
	 * description}.
	 * <p>
	 * {@link RTCDataChannel#registerObserver} calls this once, on the
	 * registering thread. If it throws, the exception is thrown from there and
	 * the observer is not registered.
	 *
	 * @return {@code true} to be called on the network thread.
	 */
	default boolean isOkToCallOnTheNetworkThread() {
		return false;
	}

}
