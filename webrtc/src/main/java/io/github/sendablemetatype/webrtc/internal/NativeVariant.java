package io.github.sendablemetatype.webrtc.internal;

/**
 * Tells which variant the loaded native library was built as: the full one,
 * or the data channels one, which has no audio and video. Code that reaches
 * the audio and video classes only on request asks here first, so that it can
 * keep away from natives the data channels library does not have.
 */
public final class NativeVariant {

	static {
		try {
			NativeLoader.loadLibrary("webrtc-java");
		}
		catch (Exception e) {
			throw new RuntimeException("Load library 'webrtc-java' failed", e);
		}
	}

	private static final boolean MEDIA = hasMediaNative();


	private NativeVariant() {
	}

	/**
	 * @return {@code true} if the native library has audio and video, {@code
	 * false} if it is the data channels variant.
	 */
	public static boolean hasMedia() {
		return MEDIA;
	}

	private static native boolean hasMediaNative();

}
