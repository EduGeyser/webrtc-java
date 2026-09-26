package io.github.sendablemetatype.webrtc.logging;

import static org.junit.jupiter.api.Assertions.*;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

import io.github.sendablemetatype.webrtc.logging.Logging.Severity;

import org.junit.jupiter.api.Test;

class LogSinkLifecycleTests {

	@Test
	void removalStopsDeliveryAndCanBeRepeated() {
		MatchingSink sink = new MatchingSink("removable log sink");
		Logging.removeLogSink(sink);
		Logging.addLogSink(Severity.INFO, sink);
		try {
			Logging.info(sink.marker);
			assertEquals(1, sink.messages.get());
			Logging.removeLogSink(sink);
			Logging.info(sink.marker);
			assertEquals(1, sink.messages.get());
		}
		finally {
			Logging.removeLogSink(sink);
		}
	}

	@Test
	void removalClearsEveryRegistrationForTheSink() {
		MatchingSink sink = new MatchingSink("duplicate log sink");
		Logging.addLogSink(Severity.INFO, sink);
		try {
			Logging.addLogSink(Severity.WARNING, sink);
			Logging.info(sink.marker);
			assertEquals(1, sink.messages.get());
			Logging.warn(sink.marker);
			assertEquals(3, sink.messages.get());
			Logging.removeLogSink(sink);
			Logging.warn(sink.marker);
			assertEquals(3, sink.messages.get());
		}
		finally {
			Logging.removeLogSink(sink);
		}
	}

	@Test
	void removalUsesIdentityRatherThanEquals() {
		MatchingSink first = new MatchingSink("equal log sinks");
		MatchingSink second = new MatchingSink(first.marker);
		assertEquals(first, second);
		Logging.addLogSink(Severity.INFO, first);
		try {
			Logging.addLogSink(Severity.INFO, second);
			Logging.removeLogSink(first);
			Logging.info(first.marker);
			assertEquals(0, first.messages.get());
			assertEquals(1, second.messages.get());
		}
		finally {
			Logging.removeLogSink(first);
			Logging.removeLogSink(second);
		}
	}

	@Test
	void concurrentRegistrationsAndRemovalsLeaveNoRegistrations() throws Exception {
		MatchingSink sink = new MatchingSink("concurrent log sinks");
		ExecutorService executor = Executors.newFixedThreadPool(4, Thread.ofPlatform().daemon().factory());
		CountDownLatch start = new CountDownLatch(1);
		List<Future<?>> tasks = new ArrayList<>();
		try {
			for (int worker = 0; worker < 4; worker++) {
				tasks.add(executor.submit(() -> {
					assertTrue(start.await(5, TimeUnit.SECONDS));
					for (int attempt = 0; attempt < 20; attempt++) {
						Logging.addLogSink(Severity.INFO, sink);
						Logging.removeLogSink(sink);
					}
					return null;
				}));
			}
			start.countDown();
			for (Future<?> task : tasks) {
				task.get(10, TimeUnit.SECONDS);
			}
			Logging.info(sink.marker);
			assertEquals(0, sink.messages.get());
		}
		finally {
			start.countDown();
			executor.shutdownNow();
			executor.awaitTermination(10, TimeUnit.SECONDS);
			Logging.removeLogSink(sink);
		}
	}

	@Test
	void removalWaitsForAnActiveCallback() throws Exception {
		String marker = "blocked log sink callback";
		CountDownLatch entered = new CountDownLatch(1);
		CountDownLatch release = new CountDownLatch(1);
		CountDownLatch removing = new CountDownLatch(1);
		AtomicReference<Throwable> callbackFailure = new AtomicReference<>();
		AtomicInteger messages = new AtomicInteger();
		LogSink sink = (severity, message) -> {
			if (message.contains(marker)) {
				messages.incrementAndGet();
				entered.countDown();
				try {
					if (!release.await(10, TimeUnit.SECONDS)) {
						callbackFailure.set(new AssertionError("Callback release timed out"));
					}
				}
				catch (InterruptedException e) {
					Thread.currentThread().interrupt();
					callbackFailure.set(e);
				}
			}
		};
		ExecutorService executor = Executors.newFixedThreadPool(2, Thread.ofPlatform().daemon().factory());
		Logging.addLogSink(Severity.INFO, sink);
		try {
			Future<?> sender = executor.submit(() -> Logging.info(marker));
			assertTrue(entered.await(5, TimeUnit.SECONDS), "Callback did not start");
			Future<?> remover = executor.submit(() -> {
				removing.countDown();
				Logging.removeLogSink(sink);
			});
			assertTrue(removing.await(5, TimeUnit.SECONDS), "Removal did not start");
			assertThrows(TimeoutException.class, () -> remover.get(100, TimeUnit.MILLISECONDS));
			release.countDown();
			sender.get(5, TimeUnit.SECONDS);
			remover.get(5, TimeUnit.SECONDS);
			assertNull(callbackFailure.get());
			Logging.info(marker);
			assertEquals(1, messages.get());
		}
		finally {
			release.countDown();
			executor.shutdownNow();
			executor.awaitTermination(10, TimeUnit.SECONDS);
			Logging.removeLogSink(sink);
		}
	}

	@Test
	void callbackCannotAddOrRemoveSinks() {
		String marker = "reentrant log sink operation";
		AtomicInteger rejected = new AtomicInteger();
		LogSink[] holder = new LogSink[1];
		holder[0] = (severity, message) -> {
			if (message.contains(marker)) {
				try {
					Logging.addLogSink(Severity.INFO, holder[0]);
				}
				catch (IllegalStateException e) {
					rejected.incrementAndGet();
				}
				try {
					Logging.removeLogSink(holder[0]);
				}
				catch (IllegalStateException e) {
					rejected.incrementAndGet();
				}
			}
		};
		Logging.addLogSink(Severity.INFO, holder[0]);
		try {
			Logging.info(marker);
			assertEquals(2, rejected.get());
		}
		finally {
			Logging.removeLogSink(holder[0]);
		}
	}

	@Test
	void callbackExceptionsDoNotEscapeToTheSender() {
		MatchingSink sink = new MatchingSink("throwing log sink");
		LogSink throwing = (severity, message) -> {
			if (message.contains(sink.marker)) {
				throw new IllegalStateException("Expected log sink test exception");
			}
		};
		Logging.addLogSink(Severity.INFO, sink);
		try {
			Logging.addLogSink(Severity.INFO, throwing);
			assertDoesNotThrow(() -> Logging.info(sink.marker));
			assertEquals(1, sink.messages.get());
		}
		finally {
			Logging.removeLogSink(throwing);
			Logging.removeLogSink(sink);
		}
	}

	@Test
	void nullArgumentsAreRejected() {
		LogSink sink = (severity, message) -> {};
		assertThrows(NullPointerException.class, () -> Logging.addLogSink(null, sink));
		assertThrows(NullPointerException.class, () -> Logging.addLogSink(Severity.INFO, null));
		assertThrows(NullPointerException.class, () -> Logging.removeLogSink(null));
	}

	private static final class MatchingSink implements LogSink {
		private final String marker;
		private final AtomicInteger messages = new AtomicInteger();

		private MatchingSink(String marker) {
			this.marker = marker;
		}

		@Override
		public void onLogMessage(Severity severity, String message) {
			if (message.contains(marker)) {
				messages.incrementAndGet();
			}
		}

		@Override
		public boolean equals(Object other) {
			return other instanceof MatchingSink;
		}

		@Override
		public int hashCode() {
			return 1;
		}
	}
}
