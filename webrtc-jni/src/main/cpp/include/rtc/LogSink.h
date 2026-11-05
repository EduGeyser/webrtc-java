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

#ifndef JNI_WEBRTC_RTC_LOG_SINK_H_
#define JNI_WEBRTC_RTC_LOG_SINK_H_

#include "JavaClass.h"

#include "rtc_base/logging.h"

#include <jni.h>
#include <atomic>
#include <memory>
#include <mutex>

namespace jni
{
	// The one sink of the logging configuration. WebRTC takes its sinks once,
	// when logging is initialized, so JNI_OnLoad passes this one when the
	// system property io.github.sendablemetatype.webrtc.logging.severity asks
	// for log messages, and WebRTC hands it every message at or above that
	// severity. It writes those that Logging.logToDebug asks for to stderr, and
	// passes those that a Java log sink asks for to LogSinkRegistry. WebRTC
	// calls it on every thread that logs, also on several at once.
	class LogSink : public webrtc::LogSink
	{
		public:
			// The sink passed to WebRTC, or null if logging was not asked for.
			static LogSink * instance;

			void OnLogMessage(const webrtc::LogLineRef & line) override;

			void setDebugSeverity(webrtc::LoggingSeverity severity);
			void setJavaSeverity(webrtc::LoggingSeverity severity);

		private:
			class JavaLogSinkRegistryClass : public JavaClass
			{
				public:
					explicit JavaLogSinkRegistryClass(JNIEnv * env);

					jclass cls;
					jmethodID dispatch;
			};

			void deliverToJava(const webrtc::LogLineRef & line);

			std::atomic<webrtc::LoggingSeverity> debugSeverity { webrtc::LS_NONE };
			std::atomic<webrtc::LoggingSeverity> javaSeverity { webrtc::LS_NONE };

			// Looked up on the first message for Java, which comes only after
			// the library is loaded and a Java sink is added.
			std::once_flag javaClassOnce;
			std::shared_ptr<JavaLogSinkRegistryClass> javaClass;
	};
}

#endif
