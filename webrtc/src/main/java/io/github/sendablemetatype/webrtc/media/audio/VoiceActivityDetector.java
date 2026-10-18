/*
 * Copyright 2021 Alex Andres
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

package io.github.sendablemetatype.webrtc.media.audio;

import io.github.sendablemetatype.webrtc.internal.DisposableNativeObject;
import io.github.sendablemetatype.webrtc.internal.NativeLoader;

/**
 * A voice activity detector that analyzes audio data chunks to determine the probability of voice presence in the
 * audio stream.
 * <p>
 * Each instance belongs to one continuous mono signed 16-bit PCM stream. Calls,
 * including disposal, are serialized; callers must still preserve stream order.
 * No peer connection factory is required.
 *
 * @author Alex Andres
 */
public class VoiceActivityDetector extends DisposableNativeObject {

	static {
		try {
			NativeLoader.loadLibrary("webrtc-java");
		}
		catch (Exception e) {
			throw new RuntimeException("Load library 'webrtc-java' failed", e);
		}
	}

	/**
	 * Constructs a new {@code VoiceActivityDetector} instance.
	 */
	public VoiceActivityDetector() {
		initialize();
	}

	/**
	 * Processes one 10 ms frame of mono signed 16-bit PCM in native byte order.
	 * Supported sample rates are 8000, 16000, 32000 and 48000 Hz.
	 *
	 * @param audio             The audio data to process.
	 * @param samplesPerChannel The number of samples in the audio chunk.
	 * @param sampleRate        The sample rate of the audio data in Hz.
	 */
	public synchronized void process(byte[] audio, int samplesPerChannel, int sampleRate) {
		processInternal(audio, samplesPerChannel, sampleRate);
	}

	/**
	 * Gets the probability of voice presence from the last processed audio data.
	 *
	 * @return A value between 0.0 and 1.0 representing the probability of voice presence.
	 */
	public synchronized float getLastVoiceProbability() {
		return getLastVoiceProbabilityInternal();
	}

	/**
	 * Releases native resources held by this object. Repeated calls are harmless.
	 */
	@Override
	public synchronized void dispose() {
		disposeInternal();
	}

	/**
	 * Initializes the native resources for the voice activity detector. This method is called during construction and
	 * sets up the underlying native implementation.
	 */
	private native void initialize();

	// The methods above take the lock in Java rather than as synchronized
	// native methods: the 32-bit ARM JVM of JDK 27 crashes when a synchronized
	// native method has to wait for its lock.

	private native void processInternal(byte[] audio, int samplesPerChannel, int sampleRate);

	private native float getLastVoiceProbabilityInternal();

	private native void disposeInternal();

}
