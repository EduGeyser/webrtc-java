// The qualified export below names io.github.sendablemetatype.webrtc.media, which depends on this
// module and so is never on the module path while this module compiles.
@SuppressWarnings("module")
module io.github.sendablemetatype.webrtc {

	requires java.desktop;

	exports io.github.sendablemetatype.webrtc;
	exports io.github.sendablemetatype.webrtc.logging;
	exports io.github.sendablemetatype.webrtc.media;
	exports io.github.sendablemetatype.webrtc.media.audio;
	exports io.github.sendablemetatype.webrtc.media.video;
	exports io.github.sendablemetatype.webrtc.media.video.codec;
	exports io.github.sendablemetatype.webrtc.media.video.desktop;

	// Not API for applications. A native extension module, such as the FFmpeg
	// based media module, needs NativeApi to reach the native side of a custom
	// media source directly instead of carrying every frame through Java.
	exports io.github.sendablemetatype.webrtc.internal to io.github.sendablemetatype.webrtc.media;

}