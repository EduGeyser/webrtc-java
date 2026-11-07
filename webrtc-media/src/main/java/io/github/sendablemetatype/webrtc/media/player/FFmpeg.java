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

package io.github.sendablemetatype.webrtc.media.player;

import io.github.sendablemetatype.webrtc.internal.NativeLoader;

import java.io.IOException;
import java.io.InputStream;
import java.util.Properties;

/**
 * Loads this module's native library and reports what it was built against.
 * <p>
 * The FFmpeg libraries are loaded first, in the order they depend on each
 * other, and under the names they were built with. A dynamic linker matches an
 * already loaded module by file name, so by the time the media library itself
 * is loaded every symbol it imports is resolved without changing any search
 * path.
 *
 * @author Alex Andres
 */
public final class FFmpeg {

	/** The name of this module's native library, without platform decoration. */
	private static final String LIBRARY = "webrtc-java-media";

	/** The FFmpeg libraries to load first, per operating system. */
	private static final String LIBRARIES = "ffmpeg-libraries.properties";


	static {
		try {
			NativeLoader.loadLibrary(LIBRARY, dependencies());
		}
		catch (Exception e) {
			throw new RuntimeException("Load library '" + LIBRARY + "' failed", e);
		}
	}


	private FFmpeg() {
		// Static access only.
	}

	/**
	 * Returns the FFmpeg version the loaded native library was built against,
	 * for example {@code 8.1}. Useful to confirm which FFmpeg an application
	 * actually ended up with, since the libraries may be replaced.
	 *
	 * @return The FFmpeg version string.
	 */
	public static native String version();

	/**
	 * Returns the license of the loaded FFmpeg libraries, which must be an
	 * LGPL variant. A build that reports a GPL license is one this project did
	 * not produce, and redistributing it carries obligations this project does
	 * not meet.
	 *
	 * @return The FFmpeg license string, for example {@code LGPL version 2.1 or later}.
	 */
	public static native String license();

	/**
	 * Makes sure the native library is loaded. Loading happens when this class
	 * is first used, so this is only needed to bring the failure forward to a
	 * point where it can be reported.
	 */
	public static void load() {
		// Touching the class runs the static initializer.
	}

	/**
	 * Returns the shared libraries to load before this module's own, in the
	 * order they depend on each other, named as they are in the platform jar.
	 * They are listed in {@value #LIBRARIES}, which the native build checks
	 * against the FFmpeg libraries it packages.
	 *
	 * @return The file names of the FFmpeg libraries for this platform.
	 *
	 * @throws IOException if the list cannot be read or has no entry for this
	 *                     platform.
	 */
	private static String[] dependencies() throws IOException {
		String osName = System.getProperty("os.name").toLowerCase();
		String os = osName.startsWith("windows") ? "windows" : osName.startsWith("mac os") ? "macos" : "linux";

		Properties libraries = new Properties();

		try (InputStream input = FFmpeg.class.getResourceAsStream(LIBRARIES)) {
			if (input == null) {
				throw new IOException("Missing resource " + LIBRARIES);
			}

			libraries.load(input);
		}

		String names = libraries.getProperty(os, "").trim();

		if (names.isEmpty()) {
			throw new IOException("No FFmpeg libraries for " + os + " in " + LIBRARIES);
		}

		return names.split("[ \t]+");
	}

}
