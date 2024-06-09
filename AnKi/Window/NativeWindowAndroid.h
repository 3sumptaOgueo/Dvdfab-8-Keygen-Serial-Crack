// Copyright (C) 2009-present, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Window/NativeWindow.h>
#include <android_native_app_glue.h>

namespace anki {

/// Native window implementation for Android
class NativeWindowAndroid : public NativeWindow
{
public:
	ANativeWindow* m_nativeWindowAndroid = nullptr;

	~NativeWindowAndroid();

	Error initInternal(const NativeWindowInitInfo& init);
};

} // end namespace anki
