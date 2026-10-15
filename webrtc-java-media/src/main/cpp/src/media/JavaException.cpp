/*
 * Copyright 2026 Alex Andres
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

#include "media/JavaException.h"

namespace ffmpeg
{
	namespace
	{
		void ReportUncaughtException(JNIEnv * env, jthrowable exception)
		{
			jclass threadClass = env->FindClass("java/lang/Thread");

			if (threadClass == nullptr) {
				return;
			}

			jclass handlerClass = env->FindClass("java/lang/Thread$UncaughtExceptionHandler");

			if (handlerClass == nullptr) {
				return;
			}

			jmethodID currentThread = env->GetStaticMethodID(threadClass, "currentThread", "()Ljava/lang/Thread;");
			jmethodID getHandler = currentThread == nullptr ? nullptr : env->GetMethodID(threadClass,
				"getUncaughtExceptionHandler", "()Ljava/lang/Thread$UncaughtExceptionHandler;");
			jmethodID uncaughtException = getHandler == nullptr ? nullptr : env->GetMethodID(handlerClass,
				"uncaughtException", "(Ljava/lang/Thread;Ljava/lang/Throwable;)V");

			if (uncaughtException == nullptr) {
				return;
			}

			jobject thread = env->CallStaticObjectMethod(threadClass, currentThread);
			jobject handler = thread == nullptr ? nullptr : env->CallObjectMethod(thread, getHandler);

			if (handler != nullptr) {
				env->CallVoidMethod(handler, uncaughtException, thread, exception);
			}
		}
	}

	void ReportPendingException(JNIEnv * env) noexcept
	{
		jthrowable exception = env->ExceptionOccurred();

		if (exception == nullptr) {
			return;
		}

		env->ExceptionClear();

		// The frame frees the local references made while reporting.
		if (env->PushLocalFrame(8) == JNI_OK) {
			ReportUncaughtException(env, exception);

			env->PopLocalFrame(nullptr);
		}

		// Like the JVM, ignore an exception thrown by the handler itself.
		env->ExceptionClear();
		env->DeleteLocalRef(exception);
	}
}
