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

package io.github.sendablemetatype.webrtc.logging;

import static org.junit.jupiter.api.Assertions.*;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

import io.github.sendablemetatype.webrtc.logging.Logging.Severity;

import org.junit.jupiter.api.Test;

class LoggingTests {

	@Test
	void logToDebug() {
		Logging.logToDebug(Severity.ERROR);
		Logging.error("logToDebug at ERROR");
		Logging.logToDebug(Severity.INFO);
		Logging.logToDebug(Severity.NONE);
		Logging.info("logToDebug at NONE must not print this");
	}

	@Test
	void logInfo() throws Exception {
		String expectedMessage = "Explicit log sink test message";
		CountDownLatch latch = new CountDownLatch(1);
		AtomicReference<Severity> receivedSeverity = new AtomicReference<>();

		LogSink sink = (severity, message) -> {
			if (message != null && message.contains(expectedMessage)) {
				receivedSeverity.set(severity);
				latch.countDown();
			}
		};

		Logging.addLogSink(Severity.INFO, sink);
		try {
			Logging.info(expectedMessage);
			assertTrue(latch.await(5, TimeUnit.SECONDS), "Did not receive the explicit log message");
			assertEquals(Severity.INFO, receivedSeverity.get());
		}
		finally {
			Logging.removeLogSink(sink);
		}
	}

}
