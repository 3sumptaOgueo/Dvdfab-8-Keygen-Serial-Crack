// Copyright (C) 2009-present, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Util/StdTypes.h>
#include <AnKi/Util/Function.h>
#include <AnKi/Util/String.h>
#include <ctime>

namespace anki {

/// @addtogroup util_system
/// @{

/// Get the number of CPU cores
ANKI_PURE U32 getCpuCoresCount();

/// @internal
void backtraceInternal(const Function<void(CString)>& lambda);

/// Get a backtrace.
template<typename TFunc>
void backtrace(TFunc func)
{
	Function<void(CString)> f(func);
	backtraceInternal(f);
}

/// Return true if the engine is running from a terminal emulator.
Bool runningFromATerminal();

/// Return the local time in a thread safe way.
std::tm getLocalTime();

#if ANKI_OS_ANDROID
/// This function reads what is passed to "am" and interprets them as command line arguments. Should be called by
/// android_main(). It's not thread safe. Don't call it more than once.
/// Executing an apk using:
/// @code
/// adb shell am start XXX -e cmd "arg0 arg1 arg2"
/// @endcode
/// Whatever follows "cmd" will be a command line argument.
[[nodiscard]] void* getAndroidCommandLineArguments(int& argc, char**& argv);

/// Takes the return value of getAndroidCommandLineArguments() for cleanup.
void cleanupGetAndroidCommandLineArguments(void* ptr);
#endif

/// Some common code to be called before main.
void preMainInit();

#if ANKI_OS_WINDOWS
/// Convert windows errors (from GetLastError) to strings.
String errorMessageToString(DWORD errorMessageID);
#endif
/// @}

} // end namespace anki
