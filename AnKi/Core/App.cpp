
// Copyright (C) 2009-2023, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Core/App.h>
#include <AnKi/Core/ConfigSet.h>
#include <AnKi/Core/GpuMemory/UnifiedGeometryBuffer.h>
#include <AnKi/Util/Logger.h>
#include <AnKi/Util/File.h>
#include <AnKi/Util/Filesystem.h>
#include <AnKi/Util/System.h>
#include <AnKi/Util/ThreadHive.h>
#include <AnKi/Util/Tracer.h>
#include <AnKi/Util/HighRezTimer.h>
#include <AnKi/Core/CoreTracer.h>
#include <AnKi/Core/GpuMemory/RebarTransientMemoryPool.h>
#include <AnKi/Core/GpuMemory/GpuVisibleTransientMemoryPool.h>
#include <AnKi/Core/DeveloperConsole.h>
#include <AnKi/Core/StatsUi.h>
#include <AnKi/Window/NativeWindow.h>
#include <AnKi/Core/MaliHwCounters.h>
#include <AnKi/Window/Input.h>
#include <AnKi/Scene/SceneGraph.h>
#include <AnKi/Renderer/RenderQueue.h>
#include <AnKi/Resource/ResourceManager.h>
#include <AnKi/Physics/PhysicsWorld.h>
#include <AnKi/Renderer/MainRenderer.h>
#include <AnKi/Script/ScriptManager.h>
#include <AnKi/Resource/ResourceFilesystem.h>
#include <AnKi/Resource/AsyncLoader.h>
#include <AnKi/Ui/UiManager.h>
#include <AnKi/Ui/Canvas.h>
#include <csignal>

#if ANKI_OS_ANDROID
#	include <android_native_app_glue.h>
#endif

namespace anki {

#if ANKI_OS_ANDROID
/// The one and only android hack
android_app* g_androidApp = nullptr;
#endif

void* App::MemStats::allocCallback(void* userData, void* ptr, PtrSize size, [[maybe_unused]] PtrSize alignment)
{
	ANKI_ASSERT(userData);

	constexpr PtrSize kMaxAlignment = 64;

	struct alignas(kMaxAlignment) Header
	{
		PtrSize m_allocatedSize;
		Array<U8, kMaxAlignment - sizeof(PtrSize)> _m_padding;
	};
	static_assert(sizeof(Header) == kMaxAlignment, "See file");
	static_assert(alignof(Header) == kMaxAlignment, "See file");

	void* out = nullptr;

	if(ptr == nullptr)
	{
		// Need to allocate
		ANKI_ASSERT(size > 0);
		ANKI_ASSERT(alignment > 0 && alignment <= kMaxAlignment);

		const PtrSize newAlignment = kMaxAlignment;
		const PtrSize newSize = sizeof(Header) + size;

		// Allocate
		App* self = static_cast<App*>(userData);
		Header* allocation = static_cast<Header*>(
			self->m_originalAllocCallback(self->m_originalAllocUserData, nullptr, newSize, newAlignment));
		allocation->m_allocatedSize = size;
		++allocation;
		out = static_cast<void*>(allocation);

		// Update stats
		self->m_memStats.m_allocatedMem.fetchAdd(size);
		self->m_memStats.m_allocCount.fetchAdd(1);
	}
	else
	{
		// Need to free

		App* self = static_cast<App*>(userData);

		Header* allocation = static_cast<Header*>(ptr);
		--allocation;
		ANKI_ASSERT(allocation->m_allocatedSize > 0);

		// Update stats
		self->m_memStats.m_freeCount.fetchAdd(1);
		self->m_memStats.m_allocatedMem.fetchSub(allocation->m_allocatedSize);

		// Free
		self->m_originalAllocCallback(self->m_originalAllocUserData, allocation, 0, 0);
	}

	return out;
}

App::App(AllocAlignedCallback allocCb, void* allocCbUserData)
{
	m_originalAllocCallback = allocCb;
	m_originalAllocUserData = allocCbUserData;

	// Config set is a bit special so init it ASAP
	ConfigSet::allocateSingleton(allocCb, allocCbUserData);
}

App::~App()
{
	ANKI_CORE_LOGI("Destroying application");
	cleanup();
}

void App::cleanup()
{
	m_statsUi.reset(nullptr);
	m_console.reset(nullptr);

	SceneGraph::freeSingleton();
	ScriptManager::freeSingleton();
	MainRenderer::freeSingleton();
	UiManager::freeSingleton();
	GpuSceneMicroPatcher::freeSingleton();
	ResourceManager::freeSingleton();
	PhysicsWorld::freeSingleton();
	RebarTransientMemoryPool::freeSingleton();
	GpuVisibleTransientMemoryPool::freeSingleton();
	UnifiedGeometryBuffer::freeSingleton();
	GpuSceneBuffer::freeSingleton();
	CoreThreadHive::freeSingleton();
	MaliHwCounters::freeSingleton();
	GrManager::freeSingleton();
	Input::freeSingleton();
	NativeWindow::freeSingleton();

#if ANKI_ENABLE_TRACE
	CoreTracer::freeSingleton();
#endif

	GlobalFrameIndex::freeSingleton();
	ConfigSet::freeSingleton();

	m_settingsDir.destroy();
	m_cacheDir.destroy();

	CoreMemoryPool::freeSingleton();
	DefaultMemoryPool::freeSingleton();
}

Error App::init()
{
	const Error err = initInternal();
	if(err)
	{
		ANKI_CORE_LOGE("App initialization failed. Shutting down");
		cleanup();
	}

	return err;
}

Error App::initInternal()
{
	Logger::getSingleton().enableVerbosity(ConfigSet::getSingleton().getCoreVerboseLog());

	setSignalHandlers();

	AllocAlignedCallback allocCb = m_originalAllocCallback;
	void* allocCbUserData = m_originalAllocUserData;
	initMemoryCallbacks(allocCb, allocCbUserData);

	DefaultMemoryPool::allocateSingleton(allocCb, allocCbUserData);
	CoreMemoryPool::allocateSingleton(allocCb, allocCbUserData);

	ANKI_CHECK(initDirs());

	// Print a message
	const char* buildType =
#if ANKI_OPTIMIZE
		"optimized, "
#else
		"NOT optimized, "
#endif
#if ANKI_DEBUG_SYMBOLS
		"dbg symbols, "
#else
		"NO dbg symbols, "
#endif
#if ANKI_EXTRA_CHECKS
		"extra checks, "
#else
		"NO extra checks, "
#endif
#if ANKI_ENABLE_TRACE
		"built with tracing";
#else
		"NOT built with tracing";
#endif

	ANKI_CORE_LOGI("Initializing application ("
				   "version %u.%u, "
				   "%s, "
				   "compiler %s, "
				   "build date %s, "
				   "commit %s)",
				   ANKI_VERSION_MAJOR, ANKI_VERSION_MINOR, buildType, ANKI_COMPILER_STR, __DATE__, ANKI_REVISION);

// Check SIMD support
#if ANKI_SIMD_SSE && ANKI_COMPILER_GCC_COMPATIBLE
	if(!__builtin_cpu_supports("sse4.2"))
	{
		ANKI_CORE_LOGF(
			"AnKi is built with sse4.2 support but your CPU doesn't support it. Try bulding without SSE support");
	}
#endif

	ANKI_CORE_LOGI("Number of job threads: %u", ConfigSet::getSingleton().getCoreJobThreadCount());

	if(ConfigSet::getSingleton().getCoreBenchmarkMode() && ConfigSet::getSingleton().getGrVsync())
	{
		ANKI_CORE_LOGW("Vsync is enabled and benchmark mode as well. Will turn vsync off");
		ConfigSet::getSingleton().setGrVsync(false);
	}

	GlobalFrameIndex::allocateSingleton();

	//
	// Core tracer
	//
#if ANKI_ENABLE_TRACE
	ANKI_CHECK(CoreTracer::allocateSingleton().init(m_settingsDir));
#endif

	//
	// Window
	//
	NativeWindowInitInfo nwinit;
	nwinit.m_width = ConfigSet::getSingleton().getWidth();
	nwinit.m_height = ConfigSet::getSingleton().getHeight();
	nwinit.m_depthBits = 0;
	nwinit.m_stencilBits = 0;
	nwinit.m_fullscreenDesktopRez = ConfigSet::getSingleton().getWindowFullscreen() > 0;
	nwinit.m_exclusiveFullscreen = ConfigSet::getSingleton().getWindowFullscreen() == 2;
	nwinit.m_targetFps = ConfigSet::getSingleton().getCoreTargetFps();
	NativeWindow::allocateSingleton();
	ANKI_CHECK(NativeWindow::getSingleton().init(nwinit));

	//
	// Input
	//
	Input::allocateSingleton();
	ANKI_CHECK(Input::getSingleton().init());

	//
	// ThreadPool
	//
	const Bool pinThreads = !ANKI_OS_ANDROID;
	CoreThreadHive::allocateSingleton(ConfigSet::getSingleton().getCoreJobThreadCount(), pinThreads);

	//
	// Graphics API
	//
	GrManagerInitInfo grInit;
	grInit.m_allocCallback = allocCb;
	grInit.m_allocCallbackUserData = allocCbUserData;
	grInit.m_cacheDirectory = m_cacheDir.toCString();
	ANKI_CHECK(GrManager::allocateSingleton().init(grInit));

	//
	// Mali HW counters
	//
	if(GrManager::getSingleton().getDeviceCapabilities().m_gpuVendor == GpuVendor::kArm
	   && ConfigSet::getSingleton().getCoreMaliHwCounters())
	{
		MaliHwCounters::allocateSingleton();
	}

	//
	// GPU mem
	//
	UnifiedGeometryBuffer::allocateSingleton().init();
	GpuSceneBuffer::allocateSingleton().init();
	RebarTransientMemoryPool::allocateSingleton().init();
	GpuVisibleTransientMemoryPool::allocateSingleton();

	//
	// Physics
	//
	PhysicsWorld::allocateSingleton();
	ANKI_CHECK(PhysicsWorld::getSingleton().init(allocCb, allocCbUserData));

	//
	// Resources
	//
#if !ANKI_OS_ANDROID
	// Add the location of the executable where the shaders are supposed to be
	String executableFname;
	ANKI_CHECK(getApplicationPath(executableFname));
	ANKI_CORE_LOGI("Executable path is: %s", executableFname.cstr());
	String shadersPath;
	getParentFilepath(executableFname, shadersPath);
	shadersPath += ":";
	shadersPath += ConfigSet::getSingleton().getRsrcDataPaths();
	ConfigSet::getSingleton().setRsrcDataPaths(shadersPath);
#endif

	ANKI_CHECK(ResourceManager::allocateSingleton().init(allocCb, allocCbUserData));

	//
	// UI
	//
	ANKI_CHECK(UiManager::allocateSingleton().init(allocCb, allocCbUserData));

	//
	// GPU scene
	//
	ANKI_CHECK(GpuSceneMicroPatcher::allocateSingleton().init());

	//
	// Renderer
	//
	MainRendererInitInfo renderInit;
	renderInit.m_swapchainSize =
		UVec2(NativeWindow::getSingleton().getWidth(), NativeWindow::getSingleton().getHeight());
	renderInit.m_allocCallback = allocCb;
	renderInit.m_allocCallbackUserData = allocCbUserData;
	ANKI_CHECK(MainRenderer::allocateSingleton().init(renderInit));

	//
	// Script
	//
	ScriptManager::allocateSingleton(allocCb, allocCbUserData);

	//
	// Scene
	//
	ANKI_CHECK(SceneGraph::allocateSingleton().init(allocCb, allocCbUserData));

	//
	// Misc
	//
	ANKI_CHECK(UiManager::getSingleton().newInstance<StatsUi>(m_statsUi));
	ANKI_CHECK(UiManager::getSingleton().newInstance<DeveloperConsole>(m_console));

	ANKI_CORE_LOGI("Application initialized");

	return Error::kNone;
}

Error App::initDirs()
{
	// Settings path
#if !ANKI_OS_ANDROID
	String home;
	ANKI_CHECK(getHomeDirectory(home));

	m_settingsDir.sprintf("%s/.anki", &home[0]);
#else
	m_settingsDir.sprintf("%s/.anki", g_androidApp->activity->internalDataPath);
#endif

	if(!directoryExists(m_settingsDir.toCString()))
	{
		ANKI_CORE_LOGI("Creating settings dir \"%s\"", &m_settingsDir[0]);
		ANKI_CHECK(createDirectory(m_settingsDir.toCString()));
	}
	else
	{
		ANKI_CORE_LOGI("Using settings dir \"%s\"", &m_settingsDir[0]);
	}

	// Cache
	m_cacheDir.sprintf("%s/cache", &m_settingsDir[0]);

	const Bool cacheDirExists = directoryExists(m_cacheDir.toCString());
	if(ConfigSet::getSingleton().getCoreClearCaches() && cacheDirExists)
	{
		ANKI_CORE_LOGI("Will delete the cache dir and start fresh: %s", m_cacheDir.cstr());
		ANKI_CHECK(removeDirectory(m_cacheDir.toCString()));
		ANKI_CHECK(createDirectory(m_cacheDir.toCString()));
	}
	else if(!cacheDirExists)
	{
		ANKI_CORE_LOGI("Will create cache dir: %s", m_cacheDir.cstr());
		ANKI_CHECK(createDirectory(m_cacheDir.toCString()));
	}

	return Error::kNone;
}

Error App::mainLoop()
{
	ANKI_CORE_LOGI("Entering main loop");
	Bool quit = false;

	Second prevUpdateTime = HighRezTimer::getCurrentTime();
	Second crntTime = prevUpdateTime;

	// Benchmark mode stuff:
	const Bool benchmarkMode = ConfigSet::getSingleton().getCoreBenchmarkMode();
	Second aggregatedCpuTime = 0.0;
	Second aggregatedGpuTime = 0.0;
	constexpr U32 kBenchmarkFramesToGatherBeforeFlush = 60;
	U32 benchmarkFramesGathered = 0;
	File benchmarkCsvFile;
	CoreString benchmarkCsvFileFilename;
	if(benchmarkMode)
	{
		benchmarkCsvFileFilename.sprintf("%s/Benchmark.csv", m_settingsDir.cstr());
		ANKI_CHECK(benchmarkCsvFile.open(benchmarkCsvFileFilename, FileOpenFlag::kWrite));
		ANKI_CHECK(benchmarkCsvFile.writeText("CPU, GPU\n"));
	}

	while(!quit)
	{
		{
			ANKI_TRACE_SCOPED_EVENT(Frame);
			const Second startTime = HighRezTimer::getCurrentTime();

			prevUpdateTime = crntTime;
			crntTime = (!benchmarkMode) ? HighRezTimer::getCurrentTime() : (prevUpdateTime + 1.0_sec / 60.0_sec);

			// Update
			ANKI_CHECK(Input::getSingleton().handleEvents());

			// User update
			ANKI_CHECK(userMainLoop(quit, crntTime - prevUpdateTime));

			ANKI_CHECK(SceneGraph::getSingleton().update(prevUpdateTime, crntTime));

			RenderQueue rqueue;
			SceneGraph::getSingleton().doVisibilityTests(rqueue);

			// Inject stats UI
			CoreDynamicArray<UiQueueElement> newUiElementArr;
			injectUiElements(newUiElementArr, rqueue);

			// Render
			TexturePtr presentableTex = GrManager::getSingleton().acquireNextPresentableTexture();
			MainRenderer::getSingleton().setStatsEnabled(ConfigSet::getSingleton().getCoreDisplayStats() > 0
														 || benchmarkMode
#if ANKI_ENABLE_TRACE
														 || Tracer::getSingleton().getEnabled()
#endif
			);
			ANKI_CHECK(MainRenderer::getSingleton().render(rqueue, presentableTex));

			// Pause and sync async loader. That will force all tasks before the pause to finish in this frame.
			ResourceManager::getSingleton().getAsyncLoader().pause();

			// If we get stats exclude the time of GR because it forces some GPU-CPU serialization. We don't want to
			// count that
			Second grTime = 0.0;
			if(benchmarkMode || ConfigSet::getSingleton().getCoreDisplayStats() > 0) [[unlikely]]
			{
				grTime = HighRezTimer::getCurrentTime();
			}

			GrManager::getSingleton().swapBuffers();

			if(benchmarkMode || ConfigSet::getSingleton().getCoreDisplayStats() > 0) [[unlikely]]
			{
				grTime = HighRezTimer::getCurrentTime() - grTime;
			}

			const PtrSize rebarMemUsed = RebarTransientMemoryPool::getSingleton().endFrame();
			UnifiedGeometryBuffer::getSingleton().endFrame();
			GpuSceneBuffer::getSingleton().endFrame();
			GpuVisibleTransientMemoryPool::getSingleton().endFrame();

			// Update the trace info with some async loader stats
			U64 asyncTaskCount = ResourceManager::getSingleton().getAsyncLoader().getCompletedTaskCount();
			ANKI_TRACE_INC_COUNTER(RsrcAsyncTasks, asyncTaskCount - m_resourceCompletedAsyncTaskCount);
			m_resourceCompletedAsyncTaskCount = asyncTaskCount;

			// Now resume the loader
			ResourceManager::getSingleton().getAsyncLoader().resume();

			// Sleep
			const Second endTime = HighRezTimer::getCurrentTime();
			const Second frameTime = endTime - startTime;
			if(!benchmarkMode) [[likely]]
			{
				const Second timerTick = 1.0_sec / Second(ConfigSet::getSingleton().getCoreTargetFps());
				if(frameTime < timerTick)
				{
					ANKI_TRACE_SCOPED_EVENT(TimerTickSleep);
					HighRezTimer::sleep(timerTick - frameTime);
				}
			}
			// Benchmark stats
			else
			{
				aggregatedCpuTime += frameTime - grTime;
				aggregatedGpuTime += MainRenderer::getSingleton().getStats().m_renderingGpuTime;
				++benchmarkFramesGathered;
				if(benchmarkFramesGathered >= kBenchmarkFramesToGatherBeforeFlush)
				{
					aggregatedCpuTime = aggregatedCpuTime / Second(kBenchmarkFramesToGatherBeforeFlush) * 1000.0;
					aggregatedGpuTime = aggregatedGpuTime / Second(kBenchmarkFramesToGatherBeforeFlush) * 1000.0;
					ANKI_CHECK(benchmarkCsvFile.writeTextf("%f,%f\n", aggregatedCpuTime, aggregatedGpuTime));

					benchmarkFramesGathered = 0;
					aggregatedCpuTime = 0.0;
					aggregatedGpuTime = 0.0;
				}
			}

			// Stats
			if(ConfigSet::getSingleton().getCoreDisplayStats() > 0)
			{
				StatsUiInput in;
				in.m_cpuFrameTime = frameTime - grTime;
				in.m_rendererTime = MainRenderer::getSingleton().getStats().m_renderingCpuTime;
				in.m_sceneUpdateTime = SceneGraph::getSingleton().getStats().m_updateTime;
				in.m_visibilityTestsTime = SceneGraph::getSingleton().getStats().m_visibilityTestsTime;
				in.m_physicsTime = SceneGraph::getSingleton().getStats().m_physicsUpdate;

				in.m_gpuFrameTime = MainRenderer::getSingleton().getStats().m_renderingGpuTime;

				if(MaliHwCounters::isAllocated())
				{
					MaliHwCountersOut out;
					MaliHwCounters::getSingleton().sample(out);
					in.m_gpuActiveCycles = out.m_gpuActive;
					in.m_gpuReadBandwidth = out.m_readBandwidth;
					in.m_gpuWriteBandwidth = out.m_writeBandwidth;
				}

				in.m_cpuAllocatedMemory = m_memStats.m_allocatedMem.load();
				in.m_cpuAllocationCount = m_memStats.m_allocCount.load();
				in.m_cpuFreeCount = m_memStats.m_freeCount.load();

				const GrManagerStats grStats = GrManager::getSingleton().getStats();
				UnifiedGeometryBuffer::getSingleton().getStats(
					in.m_unifiedGometryExternalFragmentation, in.m_unifiedGeometryAllocated, in.m_unifiedGeometryTotal);
				GpuSceneBuffer::getSingleton().getStats(in.m_gpuSceneExternalFragmentation, in.m_gpuSceneAllocated,
														in.m_gpuSceneTotal);
				in.m_gpuDeviceMemoryAllocated = grStats.m_deviceMemoryAllocated;
				in.m_gpuDeviceMemoryInUse = grStats.m_deviceMemoryInUse;
				in.m_reBar = rebarMemUsed;

				in.m_drawableCount = rqueue.countAllRenderables();
				in.m_vkCommandBufferCount = grStats.m_commandBufferCount;

				StatsUi& statsUi = *static_cast<StatsUi*>(m_statsUi.get());
				const StatsUiDetail detail = (ConfigSet::getSingleton().getCoreDisplayStats() == 1)
												 ? StatsUiDetail::kFpsOnly
												 : StatsUiDetail::kDetailed;
				statsUi.setStats(in, detail);
			}

#if ANKI_ENABLE_TRACE
			if(MainRenderer::getSingleton().getStats().m_renderingGpuTime >= 0.0)
			{
				ANKI_TRACE_CUSTOM_EVENT(Gpu, MainRenderer::getSingleton().getStats().m_renderingGpuSubmitTimestamp,
										MainRenderer::getSingleton().getStats().m_renderingGpuTime);
			}
#endif

			++GlobalFrameIndex::getSingleton().m_value;

			if(benchmarkMode) [[unlikely]]
			{
				if(GlobalFrameIndex::getSingleton().m_value
				   >= ConfigSet::getSingleton().getCoreBenchmarkModeFrameCount())
				{
					quit = true;
				}
			}
		}

#if ANKI_ENABLE_TRACE
		static U64 frame = 1;
		CoreTracer::getSingleton().flushFrame(frame++);
#endif
	}

	if(benchmarkMode) [[unlikely]]
	{
		ANKI_CORE_LOGI("Benchmark file saved in: %s", benchmarkCsvFileFilename.cstr());
	}

	return Error::kNone;
}

void App::injectUiElements(CoreDynamicArray<UiQueueElement>& newUiElementArr, RenderQueue& rqueue)
{
	const U32 originalCount = rqueue.m_uis.getSize();
	if(ConfigSet::getSingleton().getCoreDisplayStats() > 0 || m_consoleEnabled)
	{
		const U32 extraElements = (ConfigSet::getSingleton().getCoreDisplayStats() > 0) + (m_consoleEnabled != 0);
		newUiElementArr.resize(originalCount + extraElements);

		if(originalCount > 0)
		{
			memcpy(&newUiElementArr[0], &rqueue.m_uis[0], rqueue.m_uis.getSizeInBytes());
		}

		rqueue.m_uis = WeakArray<UiQueueElement>(newUiElementArr);
	}

	U32 count = originalCount;
	if(ConfigSet::getSingleton().getCoreDisplayStats() > 0)
	{
		newUiElementArr[count].m_userData = m_statsUi.get();
		newUiElementArr[count].m_drawCallback = [](CanvasPtr& canvas, void* userData) -> void {
			static_cast<StatsUi*>(userData)->build(canvas);
		};
		++count;
	}

	if(m_consoleEnabled)
	{
		newUiElementArr[count].m_userData = m_console.get();
		newUiElementArr[count].m_drawCallback = [](CanvasPtr& canvas, void* userData) -> void {
			static_cast<DeveloperConsole*>(userData)->build(canvas);
		};
		++count;
	}
}

void App::initMemoryCallbacks(AllocAlignedCallback& allocCb, void*& allocCbUserData)
{
	if(ConfigSet::getSingleton().getCoreDisplayStats() > 1)
	{
		allocCb = MemStats::allocCallback;
		allocCbUserData = this;
	}
	else
	{
		// Leave the default
	}
}

void App::setSignalHandlers()
{
	auto handler = [](int signum) -> void {
		const char* name = nullptr;
		switch(signum)
		{
		case SIGABRT:
			name = "SIGABRT";
			break;
		case SIGSEGV:
			name = "SIGSEGV";
			break;
#if ANKI_POSIX
		case SIGBUS:
			name = "SIGBUS";
			break;
#endif
		case SIGILL:
			name = "SIGILL";
			break;
		case SIGFPE:
			name = "SIGFPE";
			break;
		}

		if(name)
			printf("Caught signal %d (%s)\n", signum, name);
		else
			printf("Caught signal %d\n", signum);

		U32 count = 0;
		printf("Backtrace:\n");
		backtrace([&count](CString symbol) {
			printf("%.2u: %s\n", count++, symbol.cstr());
		});

		ANKI_DEBUG_BREAK();
	};

	signal(SIGSEGV, handler);
	signal(SIGILL, handler);
	signal(SIGFPE, handler);
#if ANKI_POSIX
	signal(SIGBUS, handler);
#endif
	// Ignore for now: signal(SIGABRT, handler);
}

} // end namespace anki
