/**
 * Media extension for webrtc-java: reads media files and network streams with
 * FFmpeg and feeds them into a peer connection, and records what a peer
 * connection sends or receives into media files.
 */
module io.github.sendablemetatype.webrtc.media {

	requires io.github.sendablemetatype.webrtc;

	exports io.github.sendablemetatype.webrtc.media.player;
	exports io.github.sendablemetatype.webrtc.media.recorder;

}
