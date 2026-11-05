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
#include "JavaBoundary.h"
#include "rtc/LogSink.h"
#include "JavaEnums.h"
#include "JavaRef.h"
#include "JavaString.h"
#include "JavaUtils.h"

#include "rtc_base/logging.h"

#include <string>

namespace
{
	// The severities a Java enum can stand for, LS_NONE included.
	bool ToSeverity(int value, webrtc::LoggingSeverity & severity)
	{
		if (value < webrtc::LS_VERBOSE || value > webrtc::LS_NONE) {
			return false;
		}

		severity = static_cast<webrtc::LoggingSeverity>(value);

		return true;
	}
}

JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_logging_Logging_setSinkSeverity
(JNIEnv * env, jclass caller, jobject jseverity)
{
	jni::CallFromJava(env, [&]() {
		webrtc::LoggingSeverity severity;

		if (jni::LogSink::instance != nullptr &&
			ToSeverity(jni::JavaEnums::toNative<webrtc::LoggingSeverity>(env, jseverity), severity)) {
			jni::LogSink::instance->setJavaSeverity(severity);
		}
	});
}

JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_logging_Logging_log
(JNIEnv * env, jclass caller, jobject jseverity, jstring jmessage)
{
	try {
		std::string message = jni::JavaString::toNative(env, jni::JavaLocalRef<jstring>(env, jmessage));
	
		auto severity = jni::JavaEnums::toNative<webrtc::LoggingSeverity>(env, jseverity);

		RTC_LOG_V(severity) << message;
	}
	catch (...) {
		ThrowCxxJavaException(env);
	}
}

JNIEXPORT void JNICALL Java_io_github_sendablemetatype_webrtc_logging_Logging_logToDebug
(JNIEnv * env, jclass caller, jobject jseverity)
{
	jni::CallFromJava(env, [&]() {
		webrtc::LoggingSeverity severity;

		if (jni::LogSink::instance != nullptr &&
			ToSeverity(jni::JavaEnums::toNative<webrtc::LoggingSeverity>(env, jseverity), severity)) {
			jni::LogSink::instance->setDebugSeverity(severity);
		}
	});
}
