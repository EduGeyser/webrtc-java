package io.github.sendablemetatype.webrtc.logging;

import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.locks.ReentrantReadWriteLock;

/**
 * The Java log sinks. WebRTC takes its sinks once, when the library is
 * loaded, so the native library has one sink that hands this class every
 * message some Java sink wants, and the registrations live here.
 */
final class LogSinkRegistry {

	private record Registration(LogSink sink, Logging.Severity severity) {
	}

	// Replaced as a whole, so that deliveries read it without a lock.
	private static volatile List<Registration> registrations = List.of();

	// Deliveries hold the read lock, removals the write lock, so that a removal
	// returns only once no callback of the removed sink runs any more.
	private static final ReentrantReadWriteLock LOCK = new ReentrantReadWriteLock();

	private static final ThreadLocal<Boolean> IN_CALLBACK = new ThreadLocal<>();

	private LogSinkRegistry() {
	}

	static void add(Logging.Severity severity, LogSink sink) {
		Objects.requireNonNull(severity, "severity");
		Objects.requireNonNull(sink, "sink");
		checkNotInCallback();

		LOCK.writeLock().lock();
		try {
			List<Registration> next = new ArrayList<>(registrations);
			next.add(new Registration(sink, severity));
			update(next);
		}
		finally {
			LOCK.writeLock().unlock();
		}
	}

	static void remove(LogSink sink) {
		Objects.requireNonNull(sink, "sink");
		checkNotInCallback();

		LOCK.writeLock().lock();
		try {
			List<Registration> next = new ArrayList<>(registrations);
			if (next.removeIf(registration -> registration.sink() == sink)) {
				update(next);
			}
		}
		finally {
			LOCK.writeLock().unlock();
		}
	}

	/**
	 * Called by the native log sink on the thread that logged, for a message
	 * at or above the severity last passed to {@link Logging#setSinkSeverity}.
	 */
	static void dispatch(Logging.Severity severity, String message) {
		// A sink that logs would otherwise get its own messages, without end.
		if (IN_CALLBACK.get() != null) {
			return;
		}

		LOCK.readLock().lock();
		IN_CALLBACK.set(Boolean.TRUE);
		try {
			for (Registration registration : registrations) {
				if (severity.compareTo(registration.severity()) >= 0) {
					deliver(registration.sink(), severity, message);
				}
			}
		}
		finally {
			IN_CALLBACK.remove();
			LOCK.readLock().unlock();
		}
	}

	private static void update(List<Registration> next) {
		Logging.Severity lowest = Logging.Severity.NONE;

		for (Registration registration : next) {
			if (registration.severity().compareTo(lowest) < 0) {
				lowest = registration.severity();
			}
		}

		Logging.setSinkSeverity(lowest);
		registrations = List.copyOf(next);
	}

	private static void checkNotInCallback() {
		if (IN_CALLBACK.get() != null) {
			throw new IllegalStateException("Cannot add or remove log sinks from a log callback");
		}
	}

	private static void deliver(LogSink sink, Logging.Severity severity, String message) {
		try {
			sink.onLogMessage(severity, message);
		}
		catch (Throwable e) {
			reportCallbackFailure(e);
		}
	}

	private static void reportCallbackFailure(Throwable error) {
		try {
			error.printStackTrace(System.err);
		}
		catch (Throwable ignored) {
			// A failing error stream must not let an exception escape the callback.
		}
	}
}
