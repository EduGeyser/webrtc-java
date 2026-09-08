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

import io.github.sendablemetatype.webrtc.media.audio.AudioDeviceModule;
import io.github.sendablemetatype.webrtc.media.audio.AudioLayer;

import org.junit.jupiter.api.AfterAll;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.TestInstance;
import org.junit.jupiter.api.parallel.Execution;
import org.junit.jupiter.api.parallel.ExecutionMode;

/**
 * Test base class maintaining the {@link PeerConnectionFactory}.
 *
 * @author Alex Andres
 */
@TestInstance(TestInstance.Lifecycle.PER_CLASS)
@Execution(ExecutionMode.SAME_THREAD)
public abstract class TestBase {

	/**
	 * Whether the tests run against the data channels only variant of the
	 * native library, which has no audio device module.
	 */
	protected static final boolean DATA_CHANNELS_ONLY = "data-channels".equals(System.getProperty("webrtc.variant"));

	protected PeerConnectionFactory factory;

	protected AudioDeviceModule audioDevModule;


	@BeforeAll
	protected void initFactory() {
		if (DATA_CHANNELS_ONLY) {
			factory = new PeerConnectionFactory();
		}
		else {
			audioDevModule = new AudioDeviceModule(AudioLayer.kDummyAudio);
			factory = new PeerConnectionFactory(audioDevModule);
		}
	}

	@AfterAll
	protected void disposeFactory() {
		if (audioDevModule != null) {
			audioDevModule.dispose();
		}
		factory.dispose();
	}

}
