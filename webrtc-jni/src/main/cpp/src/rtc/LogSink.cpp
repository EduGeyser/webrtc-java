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

#include "rtc/LogSink.h"
#include "JavaClasses.h"
#include "JavaEnums.h"
#include "JavaRef.h"
#include "JavaString.h"
#include "JavaUtils.h"
#include "JNI_WebRTC.h"

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

namespace jni
{
	LogSink * LogSink::instance = nullptr;

	void LogSink::OnLogMessage(const webrtc::LogLineRef & line)
	{
		const webrtc::LoggingSeverity severity = line.severity();

		if (severity >= debugSeverity.load(std::memory_order_relaxed)) {
			// Where libwebrtc writes its own debug output: the debugger output
			// on Windows, and stderr everywhere.
			const std::string & text = line.DefaultLogLine();
#ifdef _WIN32
			OutputDebugStringA(text.c_str());
#endif
			fputs(text.c_str(), stderr);
			fflush(stderr);
		}

		if (severity >= javaSeverity.load(std::memory_order_relaxed)) {
			deliverToJava(line);
		}
	}

	void LogSink::setDebugSeverity(webrtc::LoggingSeverity severity)
	{
		debugSeverity.store(severity, std::memory_order_relaxed);
	}

	void LogSink::setJavaSeverity(webrtc::LoggingSeverity severity)
	{
		javaSeverity.store(severity, std::memory_order_relaxed);
	}

	void LogSink::deliverToJava(const webrtc::LogLineRef & line)
	{
		JNIEnv * env = AttachCurrentThread();

		if (env == nullptr) {
			return;
		}

		// WebRTC is built without unwinding, so nothing may throw from here:
		// an exception passing through WebRTC's frames ends the process.
		//
		// The code that logged may have a Java exception pending, which no
		// JNI call may be made with. It is set aside and restored after.
		jthrowable pending = env->ExceptionOccurred();

		if (pending != nullptr) {
			env->ExceptionClear();
		}

		try {
			std::call_once(javaClassOnce, [&]() {
				javaClass = JavaClasses::get<JavaLogSinkRegistryClass>(env);
			});

			if (javaClass->cls != nullptr && javaClass->dispatch != nullptr) {
				JavaLocalRef<jobject> jSeverity = JavaEnums::toJava(env, line.severity());
				JavaLocalRef<jstring> jMessage = JavaString::toJava(env, line.DefaultLogLine());

				env->CallStaticVoidMethod(javaClass->cls, javaClass->dispatch, jSeverity.get(), jMessage.get());
			}
		}
		catch (...) {
			ThrowCxxJavaException(env);
		}

		// LogSinkRegistry catches what a Java sink throws, so this is what the
		// call itself threw. It is printed, but not rethrown, and not handed to
		// the uncaught exception handler, as other callbacks do: one that logs
		// would come back here. Nor is it printed with ExceptionDescribe, which
		// writes the first part to native stderr and the stack trace to
		// System.err.
		JavaLocalRef<jthrowable> exception(env, env->ExceptionOccurred());

		if (exception.get() != nullptr) {
			env->ExceptionClear();

			JavaLocalRef<jclass> throwableClass(env, env->GetObjectClass(exception.get()));
			jmethodID printStackTrace = env->GetMethodID(throwableClass.get(), "printStackTrace", "()V");

			if (printStackTrace != nullptr) {
				env->CallVoidMethod(exception.get(), printStackTrace);
			}
			env->ExceptionClear();
		}

		if (pending != nullptr) {
			env->Throw(pending);
			env->DeleteLocalRef(pending);
		}
	}

	LogSink::JavaLogSinkRegistryClass::JavaLogSinkRegistryClass(JNIEnv * env)
	{
		cls = FindClass(env, PKG_LOG"LogSinkRegistry");

		dispatch = GetStaticMethod(env, cls, "dispatch", "(L" PKG_LOG "Logging$Severity;" STRING_SIG ")V");
	}
}
