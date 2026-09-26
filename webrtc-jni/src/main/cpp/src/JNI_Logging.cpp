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

#include "JNI_Logging.h"
#include "rtc/LogSink.h"
#include "JavaEnums.h"
#include "JavaRef.h"
#include "JavaString.h"
#include "JavaUtils.h"

#include "rtc_base/logging.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
	using ForeignLogCallback = void (*)(int32_t, const char *, int64_t);

	class ForeignLogSink final : public webrtc::LogSink
	{
		public:
			explicit ForeignLogSink(ForeignLogCallback callback) : callback(callback) {}

			void OnLogMessage(const std::string & message) override
			{
				OnLogMessage(message, webrtc::LS_INFO);
			}

			void OnLogMessage(const std::string & message, webrtc::LoggingSeverity severity) override
			{
				callback(static_cast<int32_t>(severity), message.data(), static_cast<int64_t>(message.size()));
			}

		private:
			ForeignLogCallback callback;
	};

	void removeLogSink(int64_t handle)
	{
		auto sink = reinterpret_cast<webrtc::LogSink *>(static_cast<uintptr_t>(handle));
		if (sink != nullptr) {
			// Removal takes WebRTC's log lock and waits for callbacks before deletion.
			webrtc::LogMessage::RemoveLogToStream(sink);
			delete sink;
		}
	}

	// libwebrtc no longer lets LogMessage::LogToDebug change the severity of its
	// own debug output after the logging configuration is initialized. A sink
	// registered with AddLogToStream can be added and removed at any time, so
	// logToDebug is implemented with one that writes where libwebrtc used to:
	// the debugger output on Windows and stderr everywhere.
	class DebugLogSink : public webrtc::LogSink
	{
		public:
			void OnLogMessage(const std::string & message) override
			{
#ifdef _WIN32
				OutputDebugStringA(message.c_str());
#endif
				fputs(message.c_str(), stderr);
				fflush(stderr);
			}
	};

	DebugLogSink debugLogSink;
	bool debugLogSinkAttached = false;
	std::mutex debugLogSinkMutex;
}

extern "C" JNIEXPORT int64_t webrtc_java_add_log_sink(int32_t severity, ForeignLogCallback callback) noexcept
{
	try {
		auto sink = std::make_unique<ForeignLogSink>(callback);
		webrtc::LogMessage::AddLogToStream(sink.get(), static_cast<webrtc::LoggingSeverity>(severity));
		return static_cast<int64_t>(reinterpret_cast<uintptr_t>(static_cast<webrtc::LogSink *>(sink.release())));
	}
	catch (...) {
		return 0;
	}
}

extern "C" JNIEXPORT void webrtc_java_remove_log_sink(int64_t handle) noexcept
{
	removeLogSink(handle);
}

JNIEXPORT jlong JNICALL Java_io_github_sendablemetatype_webrtc_logging_Logging_addLogSinkNative
(JNIEnv * env, jclass caller, jobject jseverity, jobject jsink)
{
	try {
		auto severity = jni::JavaEnums::toNative<webrtc::LoggingSeverity>(env, jseverity);
		auto sink = std::make_unique<jni::LogSink>(env, jni::JavaGlobalRef<jobject>(env, jsink));
		if (env->ExceptionCheck()) {
			return 0;
		}
		webrtc::LogMessage::AddLogToStream(sink.get(), severity);
		return static_cast<jlong>(reinterpret_cast<uintptr_t>(static_cast<webrtc::LogSink *>(sink.release())));
	}
	catch (...) {
		ThrowCxxJavaException(env);
		return 0;
	}
}

JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_logging_Logging_removeLogSinkNative
(JNIEnv * env, jclass caller, jlong handle)
{
	removeLogSink(handle);
}

JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_logging_Logging_log
(JNIEnv * env, jclass caller, jobject jseverity, jstring jmessage)
{
	std::string message = jni::JavaString::toNative(env, jni::JavaLocalRef<jstring>(env, jmessage));
	
	auto severity = jni::JavaEnums::toNative<webrtc::LoggingSeverity>(env, jseverity);

	RTC_LOG_V(severity) << message;
}

JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_logging_Logging_logToDebug
(JNIEnv * env, jclass caller, jobject jseverity)
{
	int rtcSeverity = jni::JavaEnums::toNative<webrtc::LoggingSeverity>(env, jseverity);

	if (rtcSeverity < webrtc::LS_VERBOSE || rtcSeverity > webrtc::LS_NONE) {
		return;
	}

	auto severity = static_cast<webrtc::LoggingSeverity>(rtcSeverity);

	std::lock_guard<std::mutex> lock(debugLogSinkMutex);

	if (debugLogSinkAttached) {
		webrtc::LogMessage::RemoveLogToStream(&debugLogSink);
		debugLogSinkAttached = false;
	}

	if (severity != webrtc::LS_NONE) {
		webrtc::LogMessage::AddLogToStream(&debugLogSink, severity);
		debugLogSinkAttached = true;
	}
}

JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_logging_Logging_logThreads
(JNIEnv * env, jclass caller, jboolean enable)
{
	webrtc::LogMessage::LogThreads(static_cast<bool>(enable));
}

JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_logging_Logging_logTimestamps
(JNIEnv * env, jclass caller, jboolean enable)
{
	webrtc::LogMessage::LogTimestamps(static_cast<bool>(enable));
}