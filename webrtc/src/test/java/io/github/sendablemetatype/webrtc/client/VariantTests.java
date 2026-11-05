package io.github.sendablemetatype.webrtc.client;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import io.github.sendablemetatype.webrtc.internal.NativeVariant;

import org.junit.jupiter.api.Test;

/**
 * The runtime with the native library of either variant. Peers and data
 * channels work with both, see PeerTests; audio and video need the full one.
 */
class VariantTests extends ClientTestBase {

	private static final boolean DATA_CHANNELS_ONLY = "data-channels".equals(System.getProperty("webrtc.variant"));


	@Test
	void reportsTheVariantOfTheLoadedLibrary() {
		assertEquals(!DATA_CHANNELS_ONLY, NativeVariant.hasMedia());
	}

	@Test
	void refusesAudioAndVideoWithTheDataChannelsLibrary() {
		assumeTrue(DATA_CHANNELS_ONLY, "the full library has audio and video");

		assertThrows(ConfigurationException.class, () -> rtc.media().devices());
		assertThrows(ConfigurationException.class, () -> rtc.media().customVideo("video"));
		assertThrows(ConfigurationException.class, () -> rtc.media().customAudio("audio"));
		assertThrows(ConfigurationException.class,
				() -> rtc.createPeer(PeerOptions.builder().negotiation(Negotiation.MANUAL).receive(MediaKind.VIDEO).build()));
		assertThrows(ConfigurationException.class,
				() -> WebRtc.builder().hardwareVideoCodecs(true).build());
	}

}
