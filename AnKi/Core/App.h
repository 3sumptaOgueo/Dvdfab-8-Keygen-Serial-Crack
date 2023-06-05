// Copyright (C) 2009-2023, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Core/Common.h>
#include <AnKi/Util/String.h>
#include <AnKi/Util/Ptr.h>
#include <AnKi/Ui/UiImmediateModeBuilder.h>

namespace anki {

// Forward
class UiQueueElement;
class RenderQueue;

/// The core class of the engine.
class App
{
public:
	App(AllocAlignedCallback allocCb = allocAligned, void* allocCbUserData = nullptr);

	virtual ~App();

	/// Initialize the application.
	Error init();

	CString getSettingsDirectory() const
	{
		return m_settingsDir;
	}

	CString getCacheDirectory() const
	{
		return m_cacheDir;
	}

	/// Run the main loop.
	Error mainLoop();

	/// The user code to run along with the other main loop code.
	virtual Error userMainLoop([[maybe_unused]] Bool& quit, [[maybe_unused]] Second elapsedTime)
	{
		// Do nothing
		return Error::kNone;
	}

	void setDisplayDeveloperConsole(Bool display)
	{
		m_consoleEnabled = display;
	}

	Bool getDisplayDeveloperConsole() const
	{
		return m_consoleEnabled;
	}

private:
	// Misc
	UiImmediateModeBuilderPtr m_statsUi;
	UiImmediateModeBuilderPtr m_console;
	Bool m_consoleEnabled = false;
	CoreString m_settingsDir; ///< The path that holds the configuration
	CoreString m_cacheDir; ///< This is used as a cache
	U64 m_resourceCompletedAsyncTaskCount = 0;

	void* m_originalAllocUserData = nullptr;
	AllocAlignedCallback m_originalAllocCallback = nullptr;

	static void* statsAllocCallback(void* userData, void* ptr, PtrSize size, PtrSize alignment);

	void initMemoryCallbacks(AllocAlignedCallback& allocCb, void*& allocCbUserData);

	Error initInternal();

	Error initDirs();
	void cleanup();

	/// Inject a new UI element in the render queue for displaying various stuff.
	void injectUiElements(CoreDynamicArray<UiQueueElement>& elements, RenderQueue& rqueue);

	void setSignalHandlers();
};

} // end namespace anki
