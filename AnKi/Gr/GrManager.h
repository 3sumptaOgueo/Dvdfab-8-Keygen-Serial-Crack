// Copyright (C) 2009-2023, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Gr/Common.h>
#include <AnKi/Gr/GrObject.h>
#include <AnKi/Util/String.h>
#include <AnKi/Core/CVarSet.h>

namespace anki {

// Forward
class NativeWindow;
extern BoolCVar g_vsyncCVar;

/// @addtogroup graphics
/// @{

/// Manager initializer.
class GrManagerInitInfo
{
public:
	AllocAlignedCallback m_allocCallback = nullptr;
	void* m_allocCallbackUserData = nullptr;

	CString m_cacheDirectory;
};

/// The graphics manager, owner of all graphics objects.
class GrManager : public MakeSingletonPtr<GrManager>
{
	template<typename>
	friend class MakeSingletonPtr;

public:
	Error init(GrManagerInitInfo& init);

	const GpuDeviceCapabilities& getDeviceCapabilities() const
	{
		return m_capabilities;
	}

	/// Get next presentable image. The returned Texture is valid until the following swapBuffers. After that it might
	/// dissapear even if you hold the reference.
	TexturePtr acquireNextPresentableTexture();

	/// Swap buffers
	void swapBuffers();

	/// Wait for all work to finish.
	void finish();

	/// @name Object creation methods. They are thread-safe.
	/// @{
	[[nodiscard]] BufferPtr newBuffer(const BufferInitInfo& init);
	[[nodiscard]] TexturePtr newTexture(const TextureInitInfo& init);
	[[nodiscard]] TextureViewPtr newTextureView(const TextureViewInitInfo& init);
	[[nodiscard]] SamplerPtr newSampler(const SamplerInitInfo& init);
	[[nodiscard]] ShaderPtr newShader(const ShaderInitInfo& init);
	[[nodiscard]] ShaderProgramPtr newShaderProgram(const ShaderProgramInitInfo& init);
	[[nodiscard]] CommandBufferPtr newCommandBuffer(const CommandBufferInitInfo& init);
	[[nodiscard]] FramebufferPtr newFramebuffer(const FramebufferInitInfo& init);
	[[nodiscard]] OcclusionQueryPtr newOcclusionQuery();
	[[nodiscard]] TimestampQueryPtr newTimestampQuery();
	[[nodiscard]] RenderGraphPtr newRenderGraph();
	[[nodiscard]] GrUpscalerPtr newGrUpscaler(const GrUpscalerInitInfo& init);
	[[nodiscard]] AccelerationStructurePtr newAccelerationStructure(const AccelerationStructureInitInfo& init);
	/// @}

	ANKI_INTERNAL CString getCacheDirectory() const
	{
		return m_cacheDir.toCString();
	}

	ANKI_INTERNAL U64 getNewUuid()
	{
		return m_uuidIndex.fetchAdd(1);
	}

protected:
	GrString m_cacheDir;
	Atomic<U64> m_uuidIndex = {1};
	GpuDeviceCapabilities m_capabilities;

	GrManager();

	virtual ~GrManager();
};

template<>
template<>
GrManager& MakeSingletonPtr<GrManager>::allocateSingleton<>();

template<>
void MakeSingletonPtr<GrManager>::freeSingleton();
/// @}

} // end namespace anki
