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

#include "JNI_WebRTC.h"
#include "JavaContext.h"
#include "JavaRef.h"
#include "JavaUtils.h"
#include "WebRTCContext.h"

#include "rtc_base/logging.h"

#include <jni.h>

jni::JavaContext * javaContext = nullptr;

namespace
{
	// True only if the system property is set to "true", ignoring case, as
	// with Boolean.getBoolean.
	bool GetBooleanProperty(JNIEnv * env, const char * name)
	{
		jboolean value = JNI_FALSE;

		jni::JavaLocalRef<jclass> booleanClass(env, env->FindClass("java/lang/Boolean"));

		if (booleanClass.get() != nullptr) {
			jmethodID getBoolean = env->GetStaticMethodID(booleanClass.get(), "getBoolean", "(Ljava/lang/String;)Z");
			jni::JavaLocalRef<jstring> propertyName(env, getBoolean != nullptr ? env->NewStringUTF(name) : nullptr);

			if (propertyName.get() != nullptr) {
				value = env->CallStaticBooleanMethod(booleanClass.get(), getBoolean, propertyName.get());
			}
		}

		if (env->ExceptionCheck()) {
			env->ExceptionClear();

			return false;
		}

		return value == JNI_TRUE;
	}
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM * vm, void * reserved)
{
	JNIEnv * env = nullptr;

	if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
		return -1;
	}

	// libwebrtc initializes its logging configuration on the first log line,
	// with everything from LS_INFO upwards written to stderr, and the
	// configuration can be set only once. Initialize it here, before anything
	// can log, so nothing is written unless the application asks for it through
	// Logging.logToDebug or a log sink. For the same reason, the thread IDs and
	// timestamps in log lines are turned on with system properties, which must
	// be set before the library is loaded.
	webrtc::LoggingConfig loggingConfig;
	loggingConfig.set_min_severity(webrtc::LS_NONE);
	loggingConfig.set_debug_severity(webrtc::LS_NONE);
	loggingConfig.set_log_thread(GetBooleanProperty(env, "io.github.sendablemetatype.webrtc.logging.threads"));
	loggingConfig.set_log_timestamp(GetBooleanProperty(env, "io.github.sendablemetatype.webrtc.logging.timestamps"));
	webrtc::InitializeLogging(std::move(loggingConfig));

	javaContext = new jni::WebRTCContext(vm);

	try {
		javaContext->initialize(env);
	}
	catch (...) {
		ThrowCxxJavaException(env);
	}

	return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM * vm, void * reserved)
{
	if (javaContext != nullptr) {
		JNIEnv * env = nullptr;

		if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
			return;
		}

		try {
			javaContext->destroy(env);
		}
		catch (...) {
			ThrowCxxJavaException(env);
		}

		delete javaContext;
	}
}
