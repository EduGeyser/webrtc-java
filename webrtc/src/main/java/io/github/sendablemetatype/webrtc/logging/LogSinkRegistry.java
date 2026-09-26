package io.github.sendablemetatype.webrtc.logging;

import java.util.ArrayList;
import java.util.IdentityHashMap;
import java.util.Objects;

final class LogSinkRegistry {

	private static final IdentityHashMap<LogSink, ArrayList<Long>> SINKS = new IdentityHashMap<>();
	private static final ThreadLocal<Boolean> IN_CALLBACK = new ThreadLocal<>();

	private LogSinkRegistry() {
	}

	static void add(Logging.Severity severity, LogSink sink) {
		Objects.requireNonNull(severity, "severity");
		Objects.requireNonNull(sink, "sink");
		checkNotInCallback();

		synchronized (SINKS) {
			ArrayList<Long> handles = SINKS.get(sink);
			if (handles == null) {
				handles = new ArrayList<>(1);
				SINKS.put(sink, handles);
			}
			else {
				handles.ensureCapacity(handles.size() + 1);
			}

			try {
				// Reserve the list slot before native registration can start callbacks.
				handles.add(register(severity, sink));
			}
			catch (RuntimeException | Error e) {
				if (handles.isEmpty()) {
					SINKS.remove(sink);
				}
				throw e;
			}
		}
	}

	static void remove(LogSink sink) {
		Objects.requireNonNull(sink, "sink");
		checkNotInCallback();

		synchronized (SINKS) {
			ArrayList<Long> handles = SINKS.get(sink);
			if (handles == null) {
				return;
			}
			while (!handles.isEmpty()) {
				// Native removal waits for running callbacks, then frees the sink.
				Logging.removeLogSinkNative(handles.getLast());
				handles.removeLast();
			}
			SINKS.remove(sink);
		}
	}

	private static long register(Logging.Severity severity, LogSink sink) {
		long handle = Logging.addLogSinkNative(severity, (level, message) -> deliver(sink, level, message));
		if (handle == 0) {
			throw new OutOfMemoryError("Cannot allocate a native log sink");
		}
		return handle;
	}

	private static void checkNotInCallback() {
		if (Boolean.TRUE.equals(IN_CALLBACK.get())) {
			throw new IllegalStateException("Cannot add or remove log sinks from a log callback");
		}
	}

	private static void deliver(LogSink sink, Logging.Severity severity, String message) {
		try {
			IN_CALLBACK.set(true);
			sink.onLogMessage(severity, message);
		}
		catch (Throwable e) {
			reportCallbackFailure(e);
		}
		finally {
			IN_CALLBACK.remove();
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
