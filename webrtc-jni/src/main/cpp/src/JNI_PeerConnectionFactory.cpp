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

#include "JNI_PeerConnectionFactory.h"
#include "NativeHandle.h"
#include "api/CreateSessionDescriptionObserver.h"
#include "api/EndpointPortAllocator.h"
#include "api/FieldTrialsView.h"
#include "api/PeerConnectionObserver.h"
#include "api/PortAllocatorConfig.h"
#include "api/RTCConfiguration.h"
#include "JavaClasses.h"
#include "JavaEnums.h"
#include "JavaError.h"
#include "JavaFactories.h"
#include "JavaHashMap.h"
#include "JavaNullPointerException.h"
#include "JavaObject.h"
#include "JavaRuntimeException.h"
#include "JavaRef.h"
#include "JavaString.h"
#include "JavaUtils.h"
#include "JavaWrappedException.h"
#include "WebRTCContext.h"

#include "api/create_modular_peer_connection_factory.h"
#include "api/environment/environment_factory.h"

#ifndef WEBRTC_DATA_CHANNELS_ONLY
#include "api/AudioOptions.h"
#include "api/ProxyAudioDeviceModule.h"
#include "api/RTCRtpCapabilities.h"
#include "media/audio/AudioCaptureTap.h"
#include "media/audio/CustomAudioSource.h"
#include "media/audio/VoiceActivityListener.h"
#include "media/audio/codec/AudioCodecFactories.h"
#include "media/video/codec/DefaultVideoCodecFactories.h"
#include "media/video/codec/VideoDecoderFactoryWrapper.h"
#include "media/video/codec/VideoEncoderFactoryWrapper.h"

#include "api/audio/builtin_audio_processing_builder.h"
#include "api/audio/create_audio_device_module.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/enable_media.h"
#include "api/media_stream_interface.h"
#include "api/rtc_event_log/rtc_event_log_factory.h"
#endif

#include "rtc_base/logging.h"
#include "rtc_base/thread.h"

#include <map>
#include <thread>
#include <memory>
#include <vector>

namespace
{
	// A connection gets a port allocator of its own when it gathers on an
	// IceUdpEndpoint or only on some local addresses. Otherwise it returns null and
	// the factory creates its default allocator.
	std::unique_ptr<webrtc::PortAllocator> CreatePortAllocator(JNIEnv * env, jobject factory,
		const jni::JavaRef<jobject> & config, const webrtc::PeerConnectionInterface::RTCConfiguration & configuration)
	{
		const auto javaClass = jni::JavaClasses::get<jni::RTCConfiguration::JavaRTCConfigurationClass>(env);
		jni::JavaObject obj(env, config);

		std::vector<webrtc::IPAddress> allowed = jni::PortAllocatorConfig::allowedAddresses(env,
			obj.getObject(javaClass->portAllocatorConfig));
		jni::JavaLocalRef<jobject> jEndpoint = obj.getObject(javaClass->iceEndpoint);
		std::shared_ptr<jni::IceUdpEndpoint> endpoint;

		if (jEndpoint.get() != nullptr) {
			auto handle = GetHandle<std::shared_ptr<jni::IceUdpEndpoint>>(env, jEndpoint.get());

			if (handle == nullptr) {
				throw jni::JavaWrappedException(jni::JavaLocalRef<jthrowable>(env,
					jni::JavaRuntimeException(env, "IceUdpEndpoint is disposed")));
			}

			endpoint = *handle;
		}

		if (endpoint == nullptr && allowed.empty()) {
			return nullptr;
		}

		jni::NetworkContext * context = GetHandle<jni::NetworkContext>(env, factory, "networkContextHandle");

		if (context == nullptr) {
			throw jni::Exception("PeerConnectionFactory has no network context");
		}

		auto allocator = std::make_unique<jni::EndpointPortAllocator>(*context, std::move(endpoint), std::move(allowed),
			configuration.turn_customizer);

		// What the factory does for its default allocator.
		allocator->SetPortRange(configuration.port_allocator_config.min_port, configuration.port_allocator_config.max_port);
		allocator->set_flags(configuration.port_allocator_config.flags);

		return allocator;
	}
}

JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_PeerConnectionFactory_initialize
(JNIEnv * env, jobject caller, jobject jFieldTrials, jobject audioModule, jobject audioProcessing,
	jobject jVideoEncoderFactory, jobject jVideoDecoderFactory, jobject jAudioEncoderFactory,
	jobject jAudioDecoderFactory, jboolean hardwareEncoders, jboolean hardwareDecoders)
{
#ifndef WEBRTC_DATA_CHANNELS_ONLY
	webrtc::AudioDeviceModule * audioDevModule = (audioModule != nullptr)
		? GetHandle<webrtc::AudioDeviceModule>(env, audioModule)
		: nullptr;
#endif

	try {
		std::map<std::string, std::string> fieldTrialsMap;

		if (jFieldTrials != nullptr) {
			for (const auto & entry : jni::JavaHashMap(env, jni::JavaLocalRef<jobject>(env, jFieldTrials))) {
				std::string key = jni::JavaString::toNative(env, jni::static_java_ref_cast<jstring>(env, entry.first));
				std::string value = jni::JavaString::toNative(env, jni::static_java_ref_cast<jstring>(env, entry.second));

				if (key.empty() || value.empty()) {
					throw jni::Exception("Invalid field trial entry: key and value must not be empty");
				}

				fieldTrialsMap.emplace(std::move(key), std::move(value));
			}
		}

		// Connections with a port allocator of their own see the same field trials.
		std::map<std::string, std::string> networkFieldTrials = fieldTrialsMap;

		std::unique_ptr<webrtc::FieldTrialsView> fieldTrials = fieldTrialsMap.empty()
			? nullptr
			: std::make_unique<jni::FieldTrialsView>(std::move(fieldTrialsMap));

#ifndef WEBRTC_DATA_CHANNELS_ONLY
		// Asks the Java factories for their codecs, which is where a broken
		// factory surfaces, before anything else is set up. Without a Java
		// factory, the built-in codecs are used directly.
		std::unique_ptr<webrtc::VideoEncoderFactory> videoEncoderFactory = (jVideoEncoderFactory != nullptr)
			? std::make_unique<jni::VideoEncoderFactoryWrapper>(env, jVideoEncoderFactory)
			: hardwareEncoders
				? jni::CreateHardwareVideoEncoderFactory()
				: jni::CreateDefaultVideoEncoderFactory();
		std::unique_ptr<webrtc::VideoDecoderFactory> videoDecoderFactory = (jVideoDecoderFactory != nullptr)
			? std::make_unique<jni::VideoDecoderFactoryWrapper>(env, jVideoDecoderFactory)
			: hardwareDecoders
				? jni::CreateHardwareVideoDecoderFactory()
				: jni::CreateDefaultVideoDecoderFactory();

		webrtc::scoped_refptr<webrtc::AudioEncoderFactory> audioEncoderFactory =
			jni::CreateAudioEncoderFactory(env, jni::JavaLocalRef<jobject>(env, jAudioEncoderFactory));
		webrtc::scoped_refptr<webrtc::AudioDecoderFactory> audioDecoderFactory =
			jni::CreateAudioDecoderFactory(env, jni::JavaLocalRef<jobject>(env, jAudioDecoderFactory));
#endif

		auto networkThread = webrtc::Thread::CreateWithSocketServer();
		networkThread->SetName("webrtc_jni_network_thread", nullptr);

		auto signalingThread = webrtc::Thread::Create();
		signalingThread->SetName("webrtc_jni_signaling_thread", nullptr);

		if (!networkThread->Start()) {
			throw jni::Exception("Start network thread failed");
		}
		if (!signalingThread->Start()) {
			throw jni::Exception("Start signaling thread failed");
		}

#ifdef WEBRTC_DATA_CHANNELS_ONLY
		// No media engine: the audio module, the audio processing and the video
		// codec factories are not used. As with media, no worker thread is
		// supplied, so the factory uses the network thread as its worker thread.
		webrtc::PeerConnectionFactoryDependencies dependencies;
		dependencies.network_thread = networkThread.get();
		dependencies.signaling_thread = signalingThread.get();
		dependencies.env = webrtc::CreateEnvironment(std::move(fieldTrials));
#else
		webrtc::AudioProcessing * processing = (audioProcessing != nullptr)
			? GetHandle<webrtc::AudioProcessing>(env, audioProcessing)
			: nullptr;
		webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm(audioDevModule);

		if (!adm) {
			// Without a module, the factory would create the platform default itself
			// and abort the process when it fails to initialize, which happens on
			// hosts without an audio system. Create it here instead, the same way
			// AudioDeviceModule does, so a failure becomes an exception that the
			// application can handle, for example by passing a module created
			// with AudioLayer.kDummyAudio.
			jni::WebRTCContext * context = static_cast<jni::WebRTCContext *>(javaContext);

			adm = webrtc::CreateAudioDeviceModule(context->webrtcEnv, webrtc::AudioDeviceModule::kPlatformDefaultAudio);

			if (!adm) {
				throw jni::Exception("Create the default AudioDeviceModule failed");
			}
			if (adm->Init() != 0) {
				throw jni::Exception("Initialize the default AudioDeviceModule failed. On a host without an audio system, pass an AudioDeviceModule created with AudioLayer.kDummyAudio.");
			}
		}

		// Hand WebRTC a proxy of the module so the device capture path can be
		// switched off once this factory sends audio from sink-fed sources. See
		// ProxyAudioDeviceModule for why both paths must never feed the same
		// send stream.
		auto proxy = jni::ProxyAudioDeviceModule::Create(adm);

		// No worker thread is supplied: WebRTC is removing support for one that
		// is distinct from the network thread, and without one the factory
		// uses the network thread as its worker thread.
		webrtc::PeerConnectionFactoryDependencies dependencies;
		dependencies.network_thread = networkThread.get();
		dependencies.signaling_thread = signalingThread.get();
		dependencies.socket_factory = networkThread->socketserver();
		dependencies.event_log_factory = std::make_unique<webrtc::RtcEventLogFactory>();
		dependencies.env = webrtc::CreateEnvironment(std::move(fieldTrials));
		dependencies.adm = proxy;
		dependencies.audio_encoder_factory = audioEncoderFactory;
		dependencies.audio_decoder_factory = audioDecoderFactory;
		// The tap of the module that processes the captured audio: that of the
		// application's module, or of the one built here.
		std::shared_ptr<jni::AudioCaptureTap> tap;

		if (processing != nullptr) {
			auto moduleTap = GetHandle<std::shared_ptr<jni::AudioCaptureTap>>(env, audioProcessing, "captureTapHandle");

			if (moduleTap == nullptr) {
				throw jni::Exception("The AudioProcessing was disposed");
			}

			tap = *moduleTap;
			dependencies.audio_processing_builder = webrtc::CustomAudioProcessing(
				webrtc::scoped_refptr<webrtc::AudioProcessing>(processing));
		}
		else {
			tap = std::make_shared<jni::AudioCaptureTap>();

			auto builder = std::make_unique<webrtc::BuiltinAudioProcessingBuilder>();
			builder->SetCapturePostProcessing(jni::AudioCaptureTap::CreateProcessing(tap));

			dependencies.audio_processing_builder = std::move(builder);
		}
		dependencies.video_encoder_factory = std::move(videoEncoderFactory);
		dependencies.video_decoder_factory = std::move(videoDecoderFactory);

		webrtc::EnableMedia(dependencies);
#endif

		auto factory = webrtc::CreateModularPeerConnectionFactory(std::move(dependencies));

		if (factory != nullptr) {
			auto networkContext = std::make_unique<jni::NetworkContext>(networkThread.get(), std::move(networkFieldTrials));

			SetHandle(env, caller, factory.release());
			SetHandle(env, caller, "networkContextHandle", networkContext.release());
			SetHandle(env, caller, "networkThreadHandle", networkThread.release());
			SetHandle(env, caller, "signalingThreadHandle", signalingThread.release());
#ifndef WEBRTC_DATA_CHANNELS_ONLY
			SetHandle(env, caller, "audioModuleHandle", proxy.release());
			SetHandle(env, caller, "captureTapHandle", new std::shared_ptr<jni::AudioCaptureTap>(std::move(tap)));
#endif
		}
		else {
			throw jni::Exception("Create PeerConnectionFactory failed");
		}
	}
	catch (...) {
		ThrowCxxJavaException(env);
	}
}

JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_PeerConnectionFactory_disposeInternal
(JNIEnv * env, jobject caller)
{
	webrtc::PeerConnectionFactoryInterface * factory = jni::TakeHandle<webrtc::PeerConnectionFactoryInterface>(env, caller);

	if (factory == nullptr) {
		return;
	}

	jni::NetworkContext * networkContext = GetHandle<jni::NetworkContext>(env, caller, "networkContextHandle");
	webrtc::Thread * networkThread = GetHandle<webrtc::Thread>(env, caller, "networkThreadHandle");
	webrtc::Thread * signalingThread = GetHandle<webrtc::Thread>(env, caller, "signalingThreadHandle");
#ifndef WEBRTC_DATA_CHANNELS_ONLY
	jni::ProxyAudioDeviceModule * audioModule = GetHandle<jni::ProxyAudioDeviceModule>(env, caller, "audioModuleHandle");
	auto tap = GetHandle<std::shared_ptr<jni::AudioCaptureTap>>(env, caller, "captureTapHandle");
#endif

	// The calls that use them hold the factory's handle, so they are freed
	// with it.
	SetHandle<std::nullptr_t>(env, caller, "networkContextHandle", nullptr);
	SetHandle<std::nullptr_t>(env, caller, "networkThreadHandle", nullptr);
	SetHandle<std::nullptr_t>(env, caller, "signalingThreadHandle", nullptr);
#ifndef WEBRTC_DATA_CHANNELS_ONLY
	SetHandle<std::nullptr_t>(env, caller, "audioModuleHandle", nullptr);
	SetHandle<std::nullptr_t>(env, caller, "captureTapHandle", nullptr);
#endif

#ifdef WEBRTC_DATA_CHANNELS_ONLY
	auto release = [factory, networkContext, networkThread, signalingThread](bool deferred) {
#else
	auto release = [factory, networkContext, networkThread, signalingThread, audioModule, tap](bool deferred) {
#endif
		if (factory->Release() != webrtc::RefCountReleaseStatus::kDroppedLastRef) {
			jni::NativeHandle::ReportNotDeleted(deferred);
		}

		if (networkContext) {
			// Its network manager and socket factory go on the network thread, which
			// has to be running for that.
			delete networkContext;
		}
		if (networkThread) {
			networkThread->Stop();
			delete networkThread;
		}
		if (signalingThread) {
			signalingThread->Stop();
			delete signalingThread;
		}
#ifndef WEBRTC_DATA_CHANNELS_ONLY
		if (audioModule) {
			// The factory dropped its reference above; this releases the last one
			// and, with it, the proxy's reference on the wrapped module.
			webrtc::RefCountReleaseStatus admStatus = audioModule->Release();

			if (admStatus != webrtc::RefCountReleaseStatus::kDroppedLastRef) {
				RTC_LOG(LS_WARNING) << "ProxyAudioDeviceModule was not deleted. A reference is still around somewhere.";
			}
		}

		// The module, if anything still holds it, keeps a share of the tap.
		delete tap;
#endif
	};

	try {
		jni::ReleaseHandle(env, factory, [release, networkThread, signalingThread](bool deferred) {
			// A call that held the factory may have run on one of its threads,
			// which cannot stop itself.
			bool onOwnThread = (networkThread && networkThread->IsCurrent()) ||
				(signalingThread && signalingThread->IsCurrent());

			if (onOwnThread) {
				std::thread([release]() { release(true); }).detach();
			}
			else {
				release(deferred);
			}
		});
	}
	catch (...) {
		ThrowCxxJavaException(env);
	}
}

#ifndef WEBRTC_DATA_CHANNELS_ONLY
JNIEXPORT jlong JNICALL Java_io_github_sendablemetatype_webrtc_PeerConnectionFactory_addVoiceActivityListenerInternal
(JNIEnv * env, jobject caller, jobject jListener, jfloat threshold, jint hangoverMs)
{
	try {
		// Keeps the tap, which a dispose frees with the factory.
		jni::NativeLease<webrtc::PeerConnectionFactoryInterface> factory(env, caller);
		CHECK_HANDLEV(factory, 0);

		auto tap = GetHandle<std::shared_ptr<jni::AudioCaptureTap>>(env, caller, "captureTapHandle");
		CHECK_HANDLEV(tap, 0);

		auto listener = std::make_unique<jni::VoiceActivityListener>(env, jni::JavaGlobalRef<jobject>(env, jListener),
			threshold, hangoverMs);

		return reinterpret_cast<jlong>((*tap)->AddListener(std::move(listener)));
	}
	catch (...) {
		ThrowCxxJavaException(env);
		return 0;
	}
}

JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_PeerConnectionFactory_removeVoiceActivityListenerInternal
(JNIEnv * env, jobject caller, jlong listenerHandle)
{
	try {
		// Keeps the tap, which a dispose frees with the factory.
		jni::NativeLease<webrtc::PeerConnectionFactoryInterface> factory(env, caller);
		CHECK_HANDLE(factory);

		auto tap = GetHandle<std::shared_ptr<jni::AudioCaptureTap>>(env, caller, "captureTapHandle");
		CHECK_HANDLE(tap);

		(*tap)->RemoveListener(reinterpret_cast<jni::VoiceActivityListener *>(listenerHandle));
	}
	catch (...) {
		ThrowCxxJavaException(env);
	}
}

JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_PeerConnectionFactory_setDeviceCaptureEnabled
(JNIEnv * env, jobject caller, jboolean enabled)
{
	// Keeps the module, which a dispose releases with the factory.
	jni::NativeLease<webrtc::PeerConnectionFactoryInterface> factory(env, caller);
	CHECK_HANDLE(factory);

	jni::ProxyAudioDeviceModule * audioModule = GetHandle<jni::ProxyAudioDeviceModule>(env, caller, "audioModuleHandle");
	CHECK_HANDLE(audioModule);

	webrtc::Thread * networkThread = GetHandle<webrtc::Thread>(env, caller, "networkThreadHandle");
	CHECK_HANDLE(networkThread);

	try {
		// AudioState starts and stops the module's recording on the worker thread,
		// which is the network thread. Switching the capture path there keeps the
		// two from interleaving.
		networkThread->BlockingCall([audioModule, enabled]() {
			audioModule->SetCaptureEnabled(enabled == JNI_TRUE);
		});
	}
	catch (...) {
		ThrowCxxJavaException(env);
	}
}

JNIEXPORT jobject JNICALL Java_io_github_sendablemetatype_webrtc_PeerConnectionFactory_createAudioSourceInternal
(JNIEnv * env, jobject caller, jobject jAudioOptions)
{
	try {
		if (jAudioOptions == nullptr) {
			env->Throw(jni::JavaNullPointerException(env, "AudioOptions is null"));
			return nullptr;
		}

		jni::NativeLease<webrtc::PeerConnectionFactoryInterface> factory(env, caller);
		CHECK_HANDLEV(factory, nullptr);

		auto audioOptions = jni::AudioOptions::toNative(env, jni::JavaLocalRef<jobject>(env, jAudioOptions));

		webrtc::scoped_refptr<webrtc::AudioSourceInterface> audioSource = factory->CreateAudioSource(audioOptions);

		if (audioSource == nullptr) {
			env->Throw(jni::JavaError(env, "Create audio-track source failed"));
			return nullptr;
		}

		return jni::JavaFactories::createOwning(env, std::move(audioSource)).release();
	}
	catch (...) {
		ThrowCxxJavaException(env);
		return nullptr;
	}
}

JNIEXPORT jobject JNICALL Java_io_github_sendablemetatype_webrtc_PeerConnectionFactory_createAudioTrackInternal
(JNIEnv * env, jobject caller, jstring jlabel, jobject jsource)
{
	try {
		if (jlabel == nullptr) {
			env->Throw(jni::JavaNullPointerException(env, "Audio track label is null"));
			return nullptr;
		}
		if (jsource == nullptr) {
			env->Throw(jni::JavaNullPointerException(env, "AudioTrackSource is null"));
			return nullptr;
		}

		jni::NativeLease<webrtc::PeerConnectionFactoryInterface> factory(env, caller);
		CHECK_HANDLEV(factory, nullptr);

		jni::NativeLease<webrtc::AudioSourceInterface> source(env, jsource);
		CHECK_HANDLEV(source, nullptr);

		std::string label = jni::JavaString::toNative(env, jni::JavaLocalRef<jstring>(env, jlabel));

		webrtc::scoped_refptr<webrtc::AudioTrackInterface> audioTrack = factory->CreateAudioTrack(label, source);

		return jni::JavaFactories::createOwning(env, std::move(audioTrack)).release();
	}
	catch (...) {
		ThrowCxxJavaException(env);
		return nullptr;
	}
}

JNIEXPORT jboolean JNICALL Java_io_github_sendablemetatype_webrtc_PeerConnectionFactory_isSinkFedAudioTrack
(JNIEnv * env, jobject caller, jobject jTrack)
{
	if (jTrack == nullptr) {
		return JNI_FALSE;
	}

	jni::NativeLease<webrtc::MediaStreamTrackInterface> track(env, jTrack);

	if (track == nullptr || track->kind() != webrtc::MediaStreamTrackInterface::kAudioKind) {
		return JNI_FALSE;
	}

	webrtc::AudioSourceInterface * source = static_cast<webrtc::AudioTrackInterface *>(track.get())->GetSource();

	if (source == nullptr) {
		return JNI_FALSE;
	}

	// A remote source hands the track's sinks the audio it decodes, and a
	// CustomAudioSource hands them the audio the application pushes. Either way
	// a sender of that track is fed through the track rather than by the audio
	// device module.
	bool sinkFed = source->remote() || dynamic_cast<jni::CustomAudioSource *>(source) != nullptr;

	return sinkFed ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jobject JNICALL Java_io_github_sendablemetatype_webrtc_PeerConnectionFactory_createVideoTrack
(JNIEnv * env, jobject caller, jstring jlabel, jobject jsource)
{
	try {
		if (jlabel == nullptr) {
			env->Throw(jni::JavaNullPointerException(env, "Video track label is null"));
			return nullptr;
		}
		if (jsource == nullptr) {
			env->Throw(jni::JavaNullPointerException(env, "VideoTrackSource is null"));
			return nullptr;
		}

		jni::NativeLease<webrtc::PeerConnectionFactoryInterface> factory(env, caller);
		CHECK_HANDLEV(factory, nullptr);

		jni::NativeLease<webrtc::VideoTrackSourceInterface> source(env, jsource);
		CHECK_HANDLEV(source, nullptr);

		std::string label = jni::JavaString::toNative(env, jni::JavaLocalRef<jstring>(env, jlabel));

		webrtc::scoped_refptr<webrtc::VideoTrackInterface> videoTrack = factory->CreateVideoTrack(
			webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface>(source), label);

		return jni::JavaFactories::createOwning(env, std::move(videoTrack)).release();
	}
	catch (...) {
		ThrowCxxJavaException(env);
		return nullptr;
	}
}

#endif

JNIEXPORT jobject JNICALL Java_io_github_sendablemetatype_webrtc_PeerConnectionFactory_createPeerConnectionInternal
(JNIEnv * env, jobject caller, jobject jConfig, jobject jobserver)
{
	if (jConfig == nullptr) {
		env->Throw(jni::JavaNullPointerException(env, "RTCConfiguration is null"));
		return nullptr;
	}
	if (jobserver == nullptr) {
		env->Throw(jni::JavaNullPointerException(env, "PeerConnectionObserver is null"));
		return nullptr;
	}

	jni::NativeLease<webrtc::PeerConnectionFactoryInterface> factory(env, caller);
	CHECK_HANDLEV(factory, nullptr);

	jni::JavaLocalRef<jobject> config(env, jConfig);
	webrtc::PeerConnectionInterface::RTCConfiguration configuration;
	std::unique_ptr<webrtc::PortAllocator> allocator;

	try {
		configuration = jni::RTCConfiguration::toNative(env, config);
		allocator = CreatePortAllocator(env, caller, config, configuration);
	}
	catch (...) {
		ThrowCxxJavaException(env);
		return nullptr;
	}

	jni::PeerConnectionObserver * observer = new jni::PeerConnectionObserver(env, jni::JavaGlobalRef<jobject>(env, jobserver));
	webrtc::PeerConnectionDependencies dependencies(observer);
	dependencies.allocator = std::move(allocator);

	auto result = factory->CreatePeerConnectionOrError(configuration, std::move(dependencies));

	if (!result.ok()) {
		// No peer connection took the observer.
		delete observer;

		// The type is a string_view, which is not terminated and cannot be
		// passed through varargs as it is.
		const std::string type(ToString(result.error().type()));

		env->Throw(jni::JavaRuntimeException(env, "Create PeerConnection failed: %s %s",
			type.c_str(), result.error().message()));

		return nullptr;
	}

	auto pc = result.MoveValue();

	if (pc == nullptr) {
		delete observer;
		return nullptr;
	}

	observer->setPeerConnection(pc.get(), GetHandle<webrtc::Thread>(env, caller, "networkThreadHandle"));

	try {
		// The observer is bound before the Java object takes the connection, so
		// that it ends up with both of them or with neither.
		auto javaPeerConnection = jni::JavaFactories::createOwning(env, std::move(pc), [&](jobject object) {
			SetHandle(env, object, "observerHandle", observer);
		});

		return javaPeerConnection.release();
	}
	catch (...) {
		// The connection is still ours. It calls the observer while it closes,
		// so it goes first.
		pc = nullptr;
		delete observer;

		ThrowCxxJavaException(env);
		return nullptr;
	}
}

#ifndef WEBRTC_DATA_CHANNELS_ONLY
JNIEXPORT jobject JNICALL Java_io_github_sendablemetatype_webrtc_PeerConnectionFactory_getRtpReceiverCapabilities
(JNIEnv * env, jobject caller, jobject mediaType)
{
	try {
		jni::NativeLease<webrtc::PeerConnectionFactoryInterface> factory(env, caller);
		CHECK_HANDLEV(factory, nullptr);

		auto type = jni::JavaEnums::toNative<webrtc::MediaType>(env, mediaType);
		auto capabilities = factory->GetRtpReceiverCapabilities(type);

		return jni::RTCRtpCapabilities::toJava(env, capabilities).release();
	}
	catch (...) {
		ThrowCxxJavaException(env);
		return nullptr;
	}
}

JNIEXPORT jobject JNICALL Java_io_github_sendablemetatype_webrtc_PeerConnectionFactory_getRtpSenderCapabilities
(JNIEnv * env, jobject caller, jobject mediaType)
{
	try {
		jni::NativeLease<webrtc::PeerConnectionFactoryInterface> factory(env, caller);
		CHECK_HANDLEV(factory, nullptr);

		auto type = jni::JavaEnums::toNative<webrtc::MediaType>(env, mediaType);
		auto capabilities = factory->GetRtpSenderCapabilities(type);

		return jni::RTCRtpCapabilities::toJava(env, capabilities).release();
	}
	catch (...) {
		ThrowCxxJavaException(env);
		return nullptr;
	}
}
#endif
