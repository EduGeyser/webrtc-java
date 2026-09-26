package io.github.sendablemetatype.webrtc.logging;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.IdentityHashMap;
import java.util.Objects;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

final class LogSinkRegistry {

	private static final IdentityHashMap<LogSink, ArrayList<Registration>> SINKS = new IdentityHashMap<>();
	private static final ThreadLocal<Boolean> IN_CALLBACK = new ThreadLocal<>();
	private static final Logging.Severity[] SEVERITIES = Logging.Severity.values();
	private static final Linker LINKER = findLinker();

	private LogSinkRegistry() {
	}

	static void add(Logging.Severity severity, LogSink sink) {
		Objects.requireNonNull(severity, "severity");
		Objects.requireNonNull(sink, "sink");
		checkNotInCallback();

		synchronized (SINKS) {
			ArrayList<Registration> registrations = SINKS.get(sink);
			if (registrations == null) {
				registrations = new ArrayList<>(1);
				SINKS.put(sink, registrations);
			}
			else {
				registrations.ensureCapacity(registrations.size() + 1);
			}

			try {
				// Reserve the list slot before native registration can start callbacks.
				registrations.add(register(severity, sink));
			}
			catch (RuntimeException | Error e) {
				if (registrations.isEmpty()) {
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
			ArrayList<Registration> registrations = SINKS.get(sink);
			if (registrations == null) {
				return;
			}
			while (!registrations.isEmpty()) {
				registrations.getLast().close();
				registrations.removeLast();
			}
			SINKS.remove(sink);
		}
	}

	private static Linker findLinker() {
		try {
			return Linker.nativeLinker();
		}
		catch (UnsupportedOperationException e) {
			// The ARM32 JDK has neither a native linker nor the libffi fallback.
			return null;
		}
	}

	private static Registration register(Logging.Severity severity, LogSink sink) {
		Registration registration = new Registration(LINKER == null ? null : Arena.ofShared());
		try {
			if (registration.arena == null) {
				registration.handle = Logging.addLogSinkNative(severity,
						(level, message) -> deliver(sink, level, message));
			}
			else {
				MemorySegment callback = LINKER.upcallStub(ForeignCalls.CALLBACK.bindTo(sink),
						ForeignCalls.CALLBACK_TYPE, registration.arena);
				registration.handle = (long) ForeignCalls.ADD.invokeExact(severity.ordinal(), callback);
			}
			if (registration.handle == 0) {
				throw new OutOfMemoryError("Cannot allocate a native log sink");
			}
			return registration;
		}
		catch (Throwable e) {
			registration.close();
			throw nativeFailure(e);
		}
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

	private static void onNativeLog(LogSink sink, int severity, MemorySegment message, long length) {
		try {
			String text = new String(message.reinterpret(length).toArray(JAVA_BYTE), StandardCharsets.UTF_8);
			deliver(sink, SEVERITIES[severity], text);
		}
		catch (Throwable e) {
			// An exception must not escape an FFM upcall, which would abort the JVM.
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

	private static RuntimeException nativeFailure(Throwable error) {
		if (error instanceof Error fatal) {
			throw fatal;
		}
		if (error instanceof RuntimeException runtime) {
			return runtime;
		}
		return new IllegalStateException("Native log sink operation failed", error);
	}

	private static final class Registration {
		private final Arena arena;
		private long handle;

		private Registration(Arena arena) {
			this.arena = arena;
		}

		private void close() {
			if (handle != 0) {
				if (arena == null) {
					Logging.removeLogSinkNative(handle);
				}
				else {
					try {
						ForeignCalls.REMOVE.invokeExact(handle);
					}
					catch (Throwable e) {
						throw nativeFailure(e);
					}
				}
				handle = 0;
			}
			if (arena != null) {
				// Native removal waits for callbacks before the upcall stub is freed.
				arena.close();
			}
		}
	}

	private static final class ForeignCalls {
		private static final FunctionDescriptor CALLBACK_TYPE = FunctionDescriptor.ofVoid(JAVA_INT, ADDRESS, JAVA_LONG);
		private static final MethodHandle CALLBACK;
		private static final MethodHandle ADD;
		private static final MethodHandle REMOVE;

		static {
			try {
				CALLBACK = MethodHandles.lookup().findStatic(LogSinkRegistry.class, "onNativeLog",
						MethodType.methodType(void.class, LogSink.class, int.class, MemorySegment.class, long.class));
				SymbolLookup symbols = SymbolLookup.loaderLookup();
				ADD = LINKER.downcallHandle(symbols.find("webrtc_java_add_log_sink")
						.orElseThrow(() -> new UnsatisfiedLinkError("Missing native symbol: webrtc_java_add_log_sink")),
						FunctionDescriptor.of(JAVA_LONG, JAVA_INT, ADDRESS));
				REMOVE = LINKER.downcallHandle(symbols.find("webrtc_java_remove_log_sink")
						.orElseThrow(() -> new UnsatisfiedLinkError("Missing native symbol: webrtc_java_remove_log_sink")),
						FunctionDescriptor.ofVoid(JAVA_LONG));
			}
			catch (ReflectiveOperationException e) {
				throw new ExceptionInInitializerError(e);
			}
		}
	}
}
