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

import java.io.PrintWriter;
import java.io.StringWriter;

import io.github.sendablemetatype.webrtc.internal.NativeLoader;

/**
 * Access to the native WebRTC logging.
 * <p>
 * WebRTC takes its logging configuration once, when the native library is
 * loaded, so it comes from system properties, which must be set before the
 * library is first used, for example with {@code -D} on the command line:
 * <ul>
 * <li>{@code io.github.sendablemetatype.webrtc.logging.severity}: the lowest
 * severity of the messages that {@linkplain #addLogSink log sinks} and
 * {@link #logToDebug} can get, one of {@code VERBOSE}, {@code INFO},
 * {@code WARNING} and {@code ERROR}. Without it they get no messages, and
 * WebRTC formats none for them. Each message at or above it is formatted, so
 * a low severity costs time also while no sink wants that message.</li>
 * <li>{@code io.github.sendablemetatype.webrtc.logging.threads} and
 * {@code io.github.sendablemetatype.webrtc.logging.timestamps}: {@code true}
 * to add the ID of the logging thread or the time since the first log line
 * to each line.</li>
 * </ul>
 */
public class Logging {

	static {
		try {
			NativeLoader.loadLibrary("webrtc-java");
		}
		catch (Exception e) {
			throw new RuntimeException("Load library 'webrtc-java' failed", e);
		}
	}


	public enum Severity {

		/**
		 * For data which should not appear in the normal debug log, but should
		 * appear in diagnostic logs.
		 */
		VERBOSE,

		/**
		 * Used in debugging.
		 */
		INFO,

		/**
		 * Something that may warrant investigation.
		 */
		WARNING,

		/**
		 * A critical error has occurred.
		 */
		ERROR,

		/**
		 * Do not log.
		 */
		NONE;

	}

	/**
	 * Registers a sink for messages at or above the given severity, and at or
	 * above the severity of the system property
	 * {@code io.github.sendablemetatype.webrtc.logging.severity}, see above.
	 * Each call creates a separate registration, even for the same sink
	 * object.
	 * <p>
	 * Callbacks run on the thread that emits the message, also on several
	 * threads at once. Callback exceptions are printed to standard error and
	 * do not propagate to the sender. A message logged from a callback is not
	 * delivered to any sink. Adding or removing sinks from a callback throws,
	 * since removal waits for the callbacks that run.
	 * <p>
	 * Registration and removal are serialized across threads. A callback must
	 * not wait for another thread that is adding or removing a sink.
	 *
	 * @param severity The minimum severity to receive.
	 * @param sink The sink to register. Remove it when it is no longer needed.
	 * @throws NullPointerException If severity or sink is null.
	 * @throws IllegalStateException If called from a log sink callback.
	 */
	public static void addLogSink(Severity severity, LogSink sink) {
		LogSinkRegistry.add(severity, sink);
	}

	/**
	 * Removes every registration for the exact sink object, without using
	 * {@code equals}. Removing a sink that is not registered has no effect.
	 * <p>
	 * This call waits for active callbacks to return. No callback from those
	 * registrations runs after it returns. A concurrent add is serialized with
	 * removal; an add ordered after removal creates a new registration.
	 *
	 * @param sink The sink whose registrations should be removed.
	 * @throws NullPointerException If sink is null.
	 * @throws IllegalStateException If called from a log sink callback.
	 */
	public static void removeLogSink(LogSink sink) {
		LogSinkRegistry.remove(sink);
	}

	/**
	 * Sets the lowest severity that a registered sink asks for, so that the
	 * native library passes on only the messages some sink wants.
	 */
	static native void setSinkSeverity(Severity severity);

	public static native void log(Severity severity, String message);

	/**
	 * Writes the messages at or above the given severity to standard error,
	 * and to the debugger output on Windows, or none with {@link
	 * Severity#NONE}. Only messages at or above the severity of the system
	 * property {@code io.github.sendablemetatype.webrtc.logging.severity} are
	 * there to write, see above.
	 *
	 * @param severity The lowest severity to write.
	 */
	public static native void logToDebug(Severity severity);

	public static void verbose(String message) {
		log(Severity.VERBOSE, message);
	}

	public static void info(String message) {
		log(Severity.INFO, message);
	}

	public static void warn(String message) {
		log(Severity.WARNING, message);
	}

	public static void error(String message) {
		log(Severity.ERROR, message);
	}

	public static void error(String message, Throwable e) {
		log(Severity.ERROR, message);
		log(Severity.ERROR, e.toString());
		log(Severity.ERROR, getStackTraceString(e));
	}

	private static String getStackTraceString(Throwable e) {
		if (e == null) {
			return "";
		}

		StringWriter sw = new StringWriter();
		PrintWriter pw = new PrintWriter(sw);

		e.printStackTrace(pw);

		return sw.toString();
	}
}
