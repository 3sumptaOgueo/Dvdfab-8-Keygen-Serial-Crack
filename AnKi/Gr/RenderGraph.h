// Copyright (C) 2009-2023, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Gr/GrObject.h>
#include <AnKi/Gr/TextureView.h>
#include <AnKi/Gr/Buffer.h>
#include <AnKi/Gr/GrManager.h>
#include <AnKi/Gr/Framebuffer.h>
#include <AnKi/Gr/TimestampQuery.h>
#include <AnKi/Gr/CommandBuffer.h>
#include <AnKi/Gr/AccelerationStructure.h>
#include <AnKi/Util/HashMap.h>
#include <AnKi/Util/BitSet.h>
#include <AnKi/Util/WeakArray.h>
#include <AnKi/Util/Function.h>

namespace anki {

// Forward
class RenderGraph;
class RenderGraphDescription;

/// @addtogroup graphics
/// @{

/// @name RenderGraph constants
/// @{
constexpr U32 kMaxRenderGraphPasses = 128;
constexpr U32 kMaxRenderGraphRenderTargets = 64; ///< Max imported or not render targets in RenderGraph.
constexpr U32 kMaxRenderGraphBuffers = 64;
constexpr U32 kMaxRenderGraphAccelerationStructures = 32;
/// @}

/// Render target handle used in the RenderGraph.
/// @memberof RenderGraphDescription
class RenderGraphGrObjectHandle
{
	friend class RenderPassDependency;
	friend class RenderGraphDescription;
	friend class RenderGraph;
	friend class RenderPassDescriptionBase;

public:
	Bool operator==(const RenderGraphGrObjectHandle& b) const
	{
		return m_idx == b.m_idx;
	}

	Bool operator!=(const RenderGraphGrObjectHandle& b) const
	{
		return m_idx != b.m_idx;
	}

	Bool isValid() const
	{
		return m_idx != kMaxU32;
	}

private:
	U32 m_idx = kMaxU32;
};

/// Render target (TexturePtr) handle.
/// @memberof RenderGraphDescription
class RenderTargetHandle : public RenderGraphGrObjectHandle
{
};

/// BufferPtr handle.
/// @memberof RenderGraphDescription
class BufferHandle : public RenderGraphGrObjectHandle
{
};

/// AccelerationStructurePtr handle.
/// @memberof RenderGraphDescription
class AccelerationStructureHandle : public RenderGraphGrObjectHandle
{
};

/// Describes the render target.
/// @memberof RenderGraphDescription
class RenderTargetDescription : public TextureInitInfo
{
	friend class RenderGraphDescription;

public:
	RenderTargetDescription()
	{
	}

	RenderTargetDescription(CString name)
		: TextureInitInfo(name)
	{
	}

	/// Create an internal hash.
	void bake()
	{
		ANKI_ASSERT(m_hash == 0);
		ANKI_ASSERT(m_usage == TextureUsageBit::kNone && "No need to supply the usage. RenderGraph will find out");
		m_hash = computeHash();
	}

private:
	U64 m_hash = 0;
};

/// The only parameter of RenderPassWorkCallback.
/// @memberof RenderGraph
class RenderPassWorkContext
{
	friend class RenderGraph;

public:
	CommandBufferPtr m_commandBuffer;
	U32 m_currentSecondLevelCommandBufferIndex ANKI_DEBUG_CODE(= 0);
	U32 m_secondLevelCommandBufferCount ANKI_DEBUG_CODE(= 0);

	void getBufferState(BufferHandle handle, Buffer*& buff, PtrSize& offset, PtrSize& range) const;

	void getRenderTargetState(RenderTargetHandle handle, const TextureSubresourceInfo& subresource, Texture*& tex) const;

	/// Create a whole texture view from a handle
	TextureViewPtr createTextureView(RenderTargetHandle handle)
	{
		Texture* tex = &getTexture(handle);
		TextureViewInitInfo viewInit(tex, "TmpRenderGraph"); // Use the whole texture
		getRenderTargetState(handle, viewInit, tex);
		return GrManager::getSingleton().newTextureView(viewInit);
	}

	/// Convenience method.
	void bindTextureAndSampler(U32 set, U32 binding, RenderTargetHandle handle, const TextureSubresourceInfo& subresource, Sampler* sampler)
	{
		Texture* tex;
		getRenderTargetState(handle, subresource, tex);
		TextureViewInitInfo viewInit(tex, subresource, "TmpRenderGraph");
		TextureViewPtr view = GrManager::getSingleton().newTextureView(viewInit);
		m_commandBuffer->bindTextureAndSampler(set, binding, view.get(), sampler);
	}

	/// Convenience method.
	void bindTexture(U32 set, U32 binding, RenderTargetHandle handle, const TextureSubresourceInfo& subresource)
	{
		Texture* tex;
		getRenderTargetState(handle, subresource, tex);
		TextureViewInitInfo viewInit(tex, subresource, "TmpRenderGraph");
		TextureViewPtr view = GrManager::getSingleton().newTextureView(viewInit);
		m_commandBuffer->bindTexture(set, binding, view.get());
	}

	/// Convenience method to bind the whole texture as color.
	void bindColorTextureAndSampler(U32 set, U32 binding, RenderTargetHandle handle, Sampler* sampler)
	{
		Texture* tex = &getTexture(handle);
		TextureViewInitInfo viewInit(tex); // Use the whole texture
		getRenderTargetState(handle, viewInit, tex);
		TextureViewPtr view = GrManager::getSingleton().newTextureView(viewInit);
		m_commandBuffer->bindTextureAndSampler(set, binding, view.get(), sampler);
	}

	/// Convenience method to bind the whole texture as color.
	void bindColorTexture(U32 set, U32 binding, RenderTargetHandle handle, U32 arrayIdx = 0)
	{
		Texture* tex = &getTexture(handle);
		TextureViewInitInfo viewInit(tex); // Use the whole texture
		getRenderTargetState(handle, viewInit, tex);
		TextureViewPtr view = GrManager::getSingleton().newTextureView(viewInit);
		m_commandBuffer->bindTexture(set, binding, view.get(), arrayIdx);
	}

	/// Convenience method.
	void bindImage(U32 set, U32 binding, RenderTargetHandle handle, const TextureSubresourceInfo& subresource, U32 arrayIdx = 0)
	{
		Texture* tex;
		getRenderTargetState(handle, subresource, tex);
		TextureViewInitInfo viewInit(tex, subresource, "TmpRenderGraph");
		TextureViewPtr view = GrManager::getSingleton().newTextureView(viewInit);
		m_commandBuffer->bindImage(set, binding, view.get(), arrayIdx);
	}

	/// Convenience method to bind the whole image.
	void bindImage(U32 set, U32 binding, RenderTargetHandle handle, U32 arrayIdx = 0)
	{
		Texture* tex;
#if ANKI_ENABLE_ASSERTIONS
		tex = &getTexture(handle);
		ANKI_ASSERT(tex->getLayerCount() == 1 && tex->getMipmapCount() == 1 && tex->getDepthStencilAspect() == DepthStencilAspectBit::kNone);
#endif
		const TextureSubresourceInfo subresource;
		getRenderTargetState(handle, subresource, tex);
		TextureViewInitInfo viewInit(tex, subresource, "TmpRenderGraph");
		TextureViewPtr view = GrManager::getSingleton().newTextureView(viewInit);
		m_commandBuffer->bindImage(set, binding, view.get(), arrayIdx);
	}

	/// Convenience method.
	void bindStorageBuffer(U32 set, U32 binding, BufferHandle handle)
	{
		Buffer* buff;
		PtrSize offset, range;
		getBufferState(handle, buff, offset, range);
		m_commandBuffer->bindStorageBuffer(set, binding, buff, offset, range);
	}

	/// Convenience method.
	void bindUniformBuffer(U32 set, U32 binding, BufferHandle handle)
	{
		Buffer* buff;
		PtrSize offset, range;
		getBufferState(handle, buff, offset, range);
		m_commandBuffer->bindUniformBuffer(set, binding, buff, offset, range);
	}

	/// Convenience method.
	void bindAccelerationStructure(U32 set, U32 binding, AccelerationStructureHandle handle);

private:
	const RenderGraph* m_rgraph ANKI_DEBUG_CODE(= nullptr);
	U32 m_passIdx ANKI_DEBUG_CODE(= kMaxU32);
	U32 m_batchIdx ANKI_DEBUG_CODE(= kMaxU32);

	Texture& getTexture(RenderTargetHandle handle) const;
};

/// RenderGraph pass dependency.
/// @memberof RenderGraphDescription
class RenderPassDependency
{
	friend class RenderGraph;
	friend class RenderPassDescriptionBase;

public:
	/// Dependency to a texture subresource.
	RenderPassDependency(RenderTargetHandle handle, TextureUsageBit usage, const TextureSubresourceInfo& subresource)
		: m_texture({handle, usage, subresource})
		, m_type(Type::kTexture)
	{
		ANKI_ASSERT(handle.isValid());
	}

	/// Dependency to the whole texture.
	RenderPassDependency(RenderTargetHandle handle, TextureUsageBit usage, DepthStencilAspectBit aspect = DepthStencilAspectBit::kNone)
		: m_texture({handle, usage, TextureSubresourceInfo()})
		, m_type(Type::kTexture)
	{
		ANKI_ASSERT(handle.isValid());
		m_texture.m_subresource.m_mipmapCount = kMaxU32; // Mark it as "whole texture"
		m_texture.m_subresource.m_depthStencilAspect = aspect;
	}

	RenderPassDependency(BufferHandle handle, BufferUsageBit usage)
		: m_buffer({handle, usage})
		, m_type(Type::kBuffer)
	{
		ANKI_ASSERT(handle.isValid());
	}

	RenderPassDependency(AccelerationStructureHandle handle, AccelerationStructureUsageBit usage)
		: m_as({handle, usage})
		, m_type(Type::kAccelerationStructure)
	{
		ANKI_ASSERT(handle.isValid());
	}

private:
	class TextureInfo
	{
	public:
		RenderTargetHandle m_handle;
		TextureUsageBit m_usage;
		TextureSubresourceInfo m_subresource;
	};

	class BufferInfo
	{
	public:
		BufferHandle m_handle;
		BufferUsageBit m_usage;
	};

	class ASInfo
	{
	public:
		AccelerationStructureHandle m_handle;
		AccelerationStructureUsageBit m_usage;
	};

	union
	{
		TextureInfo m_texture;
		BufferInfo m_buffer;
		ASInfo m_as;
	};

	enum class Type : U8
	{
		kBuffer,
		kTexture,
		kAccelerationStructure
	};

	Type m_type;
};

/// The base of compute/transfer and graphics renderpasses for RenderGraph.
/// @memberof RenderGraphDescription
class RenderPassDescriptionBase
{
	friend class RenderGraph;
	friend class RenderGraphDescription;

public:
	template<typename TFunc>
	void setWork(U32 secondLeveCmdbCount, TFunc func)
	{
		ANKI_ASSERT(m_type == Type::kGraphics || secondLeveCmdbCount == 0);
		m_callback = {func, m_rtDeps.getMemoryPool().m_pool};
		m_secondLevelCmdbsCount = secondLeveCmdbCount;
	}

	template<typename TFunc>
	void setWork(TFunc func)
	{
		setWork(0, func);
	}

	void newTextureDependency(RenderTargetHandle handle, TextureUsageBit usage, const TextureSubresourceInfo& subresource)
	{
		newDependency<RenderPassDependency::Type::kTexture>(RenderPassDependency(handle, usage, subresource));
	}

	void newTextureDependency(RenderTargetHandle handle, TextureUsageBit usage, DepthStencilAspectBit aspect = DepthStencilAspectBit::kNone)
	{
		newDependency<RenderPassDependency::Type::kTexture>(RenderPassDependency(handle, usage, aspect));
	}

	void newBufferDependency(BufferHandle handle, BufferUsageBit usage)
	{
		newDependency<RenderPassDependency::Type::kBuffer>(RenderPassDependency(handle, usage));
	}

	void newAccelerationStructureDependency(AccelerationStructureHandle handle, AccelerationStructureUsageBit usage)
	{
		newDependency<RenderPassDependency::Type::kAccelerationStructure>(RenderPassDependency(handle, usage));
	}

protected:
	enum class Type : U8
	{
		kGraphics,
		kNoGraphics
	};

	Type m_type;

	RenderGraphDescription* m_descr;

	Function<void(RenderPassWorkContext&), MemoryPoolPtrWrapper<StackMemoryPool>> m_callback;
	U32 m_secondLevelCmdbsCount = 0;

	DynamicArray<RenderPassDependency, MemoryPoolPtrWrapper<StackMemoryPool>> m_rtDeps;
	DynamicArray<RenderPassDependency, MemoryPoolPtrWrapper<StackMemoryPool>> m_buffDeps;
	DynamicArray<RenderPassDependency, MemoryPoolPtrWrapper<StackMemoryPool>> m_asDeps;

	BitSet<kMaxRenderGraphRenderTargets, U64> m_readRtMask{false};
	BitSet<kMaxRenderGraphRenderTargets, U64> m_writeRtMask{false};
	BitSet<kMaxRenderGraphBuffers, U64> m_readBuffMask{false};
	BitSet<kMaxRenderGraphBuffers, U64> m_writeBuffMask{false};
	BitSet<kMaxRenderGraphAccelerationStructures, U32> m_readAsMask{false};
	BitSet<kMaxRenderGraphAccelerationStructures, U32> m_writeAsMask{false};

	BaseString<MemoryPoolPtrWrapper<StackMemoryPool>> m_name;

	RenderPassDescriptionBase(Type t, RenderGraphDescription* descr, StackMemoryPool* pool)
		: m_type(t)
		, m_descr(descr)
		, m_rtDeps(pool)
		, m_buffDeps(pool)
		, m_asDeps(pool)
		, m_name(pool)
	{
		ANKI_ASSERT(descr && pool);
	}

	void setName(CString name)
	{
		m_name = (name.isEmpty()) ? "N/A" : name;
	}

	void fixSubresource(RenderPassDependency& dep) const;

	void validateDep(const RenderPassDependency& dep);

	/// Add a new consumer or producer dependency.
	template<RenderPassDependency::Type kType>
	void newDependency(const RenderPassDependency& dep);
};

/// Framebuffer attachment info.
class FramebufferDescriptionAttachment
{
public:
	TextureSurfaceInfo m_surface;
	AttachmentLoadOperation m_loadOperation = AttachmentLoadOperation::kDontCare;
	AttachmentStoreOperation m_storeOperation = AttachmentStoreOperation::kStore;
	ClearValue m_clearValue;

	AttachmentLoadOperation m_stencilLoadOperation = AttachmentLoadOperation::kDontCare;
	AttachmentStoreOperation m_stencilStoreOperation = AttachmentStoreOperation::kStore;

	DepthStencilAspectBit m_aspect = DepthStencilAspectBit::kNone; ///< Relevant only for depth stencil textures.
};

/// Describes a framebuffer.
/// @memberof RenderGraphDescription
class FramebufferDescription
{
	friend class GraphicsRenderPassDescription;
	friend class RenderGraph;

public:
	Array<FramebufferDescriptionAttachment, kMaxColorRenderTargets> m_colorAttachments;
	U32 m_colorAttachmentCount = 0;
	FramebufferDescriptionAttachment m_depthStencilAttachment;
	U32 m_shadingRateAttachmentTexelWidth = 0;
	U32 m_shadingRateAttachmentTexelHeight = 0;
	TextureSurfaceInfo m_shadingRateAttachmentSurface;

	/// Calculate the hash for the framebuffer.
	void bake();

	Bool isBacked() const
	{
		return m_hash != 0;
	}

private:
	U64 m_hash = 0;
};

/// A graphics render pass for RenderGraph.
/// @memberof RenderGraphDescription
class GraphicsRenderPassDescription : public RenderPassDescriptionBase
{
	friend class RenderGraphDescription;
	friend class RenderGraph;

public:
	GraphicsRenderPassDescription(RenderGraphDescription* descr, StackMemoryPool* pool)
		: RenderPassDescriptionBase(Type::kGraphics, descr, pool)
	{
		memset(&m_rtHandles[0], 0xFF, sizeof(m_rtHandles));
	}

	void setFramebufferInfo(const FramebufferDescription& fbInfo, ConstWeakArray<RenderTargetHandle> colorRenderTargetHandles,
							RenderTargetHandle depthStencilRenderTargetHandle = {}, RenderTargetHandle shadingRateRenderTargetHandle = {},
							U32 minx = 0, U32 miny = 0, U32 maxx = kMaxU32, U32 maxy = kMaxU32);

	void setFramebufferInfo(const FramebufferDescription& fbInfo, std::initializer_list<RenderTargetHandle> colorRenderTargetHandles,
							RenderTargetHandle depthStencilRenderTargetHandle = {}, RenderTargetHandle shadingRateRenderTargetHandle = {},
							U32 minx = 0, U32 miny = 0, U32 maxx = kMaxU32, U32 maxy = kMaxU32);

private:
	Array<RenderTargetHandle, kMaxColorRenderTargets + 2> m_rtHandles;
	FramebufferDescription m_fbDescr;
	Array<U32, 4> m_fbRenderArea = {};

	Bool hasFramebuffer() const
	{
		return m_fbDescr.m_hash != 0;
	}
};

/// A compute render pass for RenderGraph.
/// @memberof RenderGraphDescription
class ComputeRenderPassDescription : public RenderPassDescriptionBase
{
	friend class RenderGraphDescription;

public:
	ComputeRenderPassDescription(RenderGraphDescription* descr, StackMemoryPool* pool)
		: RenderPassDescriptionBase(Type::kNoGraphics, descr, pool)
	{
	}
};

/// Builds the description of the frame's render passes and their interactions.
/// @memberof RenderGraph
class RenderGraphDescription
{
	friend class RenderGraph;
	friend class RenderPassDescriptionBase;

public:
	RenderGraphDescription(StackMemoryPool* pool)
		: m_pool(pool)
	{
	}

	~RenderGraphDescription();

	/// Create a new graphics render pass.
	GraphicsRenderPassDescription& newGraphicsRenderPass(CString name);

	/// Create a new compute render pass.
	ComputeRenderPassDescription& newComputeRenderPass(CString name);

	/// Import an existing render target and let the render graph know about it's up-to-date usage.
	RenderTargetHandle importRenderTarget(Texture* tex, TextureUsageBit usage);

	/// Import an existing render target and let the render graph find it's current usage by looking at the previous frame.
	RenderTargetHandle importRenderTarget(Texture* tex);

	/// Get or create a new render target.
	RenderTargetHandle newRenderTarget(const RenderTargetDescription& initInf);

	/// Import a buffer.
	BufferHandle importBuffer(Buffer* buff, BufferUsageBit usage, PtrSize offset = 0, PtrSize range = kMaxPtrSize);

	/// Import an AS.
	AccelerationStructureHandle importAccelerationStructure(AccelerationStructure* as, AccelerationStructureUsageBit usage);

	/// Gather statistics.
	void setStatisticsEnabled(Bool gather)
	{
		m_gatherStatistics = gather;
	}

private:
	class Resource
	{
	public:
		Array<char, kMaxGrObjectNameLength + 1> m_name;

		void setName(CString name)
		{
			const U len = name.getLength();
			ANKI_ASSERT(len <= kMaxGrObjectNameLength);
			strcpy(&m_name[0], (len) ? &name[0] : "unnamed");
		}
	};

	class RT : public Resource
	{
	public:
		TextureInitInfo m_initInfo;
		U64 m_hash = 0;
		TexturePtr m_importedTex;
		TextureUsageBit m_importedLastKnownUsage = TextureUsageBit::kNone;
		/// Derived by the deps of this RT and will be used to set its usage.
		TextureUsageBit m_usageDerivedByDeps = TextureUsageBit::kNone;
		Bool m_importedAndUndefinedUsage = false;
	};

	class BufferRsrc : public Resource
	{
	public:
		BufferUsageBit m_usage;
		BufferPtr m_importedBuff;
		PtrSize m_offset;
		PtrSize m_range;
	};

	class AS : public Resource
	{
	public:
		AccelerationStructurePtr m_importedAs;
		AccelerationStructureUsageBit m_usage;
	};

	StackMemoryPool* m_pool = nullptr;
	DynamicArray<RenderPassDescriptionBase*, MemoryPoolPtrWrapper<StackMemoryPool>> m_passes{m_pool};
	DynamicArray<RT, MemoryPoolPtrWrapper<StackMemoryPool>> m_renderTargets{m_pool};
	DynamicArray<BufferRsrc, MemoryPoolPtrWrapper<StackMemoryPool>> m_buffers{m_pool};
	DynamicArray<AS, MemoryPoolPtrWrapper<StackMemoryPool>> m_as{m_pool};
	Bool m_gatherStatistics = false;

	/// Return true if 2 buffer ranges overlap.
	static Bool bufferRangeOverlaps(PtrSize offsetA, PtrSize rangeA, PtrSize offsetB, PtrSize rangeB)
	{
		ANKI_ASSERT(rangeA > 0 && rangeB > 0);
		if(rangeA == kMaxPtrSize || rangeB == kMaxPtrSize)
		{
			return true;
		}
		else if(offsetA <= offsetB)
		{
			return offsetA + rangeA >= offsetB;
		}
		else
		{
			return offsetB + rangeB >= offsetA;
		}
	}
};

/// Statistics.
/// @memberof RenderGraph
class RenderGraphStatistics
{
public:
	Second m_gpuTime; ///< Time spent in the GPU.
	Second m_cpuStartTime; ///< Time the work was submited from the CPU (almost)
};

/// Accepts a descriptor of the frame's render passes and sets the dependencies between them.
///
/// The idea for the RenderGraph is to automate:
/// - Synchronization (barriers, events etc) between passes.
/// - Command buffer creation for primary and secondary command buffers.
/// - Framebuffer creation.
/// - Render target creation (optional since textures can be imported as well).
///
/// It accepts a description of the frame's render passes (compute and graphics), compiles that description to calculate
/// dependencies and then populates command buffers with the help of multiple RenderPassWorkCallback.
class RenderGraph final : public GrObject
{
	ANKI_GR_OBJECT

	friend class RenderPassWorkContext;

public:
	static constexpr GrObjectType kClassType = GrObjectType::kRenderGraph;

	/// @name 1st step methods
	/// @{
	void compileNewGraph(const RenderGraphDescription& descr, StackMemoryPool& pool);
	/// @}

	/// @name 2nd step methods
	/// @{

	/// Will call a number of RenderPassWorkCallback that populate 2nd level command buffers.
	void runSecondLevel(U32 threadIdx);
	/// @}

	/// @name 3rd step methods
	/// @{

	/// Will call a number of RenderPassWorkCallback that populate 1st level command buffers.
	void run() const;
	/// @}

	/// @name 3rd step methods
	/// @{
	void flush();
	/// @}

	/// @name 4th step methods
	/// @{

	/// Reset the graph for a new frame. All previously created RenderGraphHandle are invalid after that call.
	void reset();
	/// @}

	/// @name 5th step methods [OPTIONAL]
	/// @{

	/// Get some statistics.
	void getStatistics(RenderGraphStatistics& statistics) const;
	/// @}

private:
	static constexpr U kPeriodicCleanupEvery = 60; ///< How many frames between cleanups.

	// Forward declarations of internal classes.
	class BakeContext;
	class Pass;
	class Batch;
	class RT;
	class BufferRange;
	class AS;
	class TextureBarrier;
	class BufferBarrier;
	class ASBarrier;

	/// Render targets of the same type+size+format.
	class RenderTargetCacheEntry
	{
	public:
		GrDynamicArray<TexturePtr> m_textures;
		U32 m_texturesInUse = 0;
	};

	/// Info on imported render targets that are kept between runs.
	class ImportedRenderTargetInfo
	{
	public:
		GrDynamicArray<TextureUsageBit> m_surfOrVolLastUsages; ///< Last TextureUsageBit of the imported RT.
	};

	GrHashMap<U64, RenderTargetCacheEntry> m_renderTargetCache; ///< Non-imported render targets.
	GrHashMap<U64, FramebufferPtr> m_fbCache; ///< Framebuffer cache.
	GrHashMap<U64, ImportedRenderTargetInfo> m_importedRenderTargets;

	BakeContext* m_ctx = nullptr;
	U64 m_version = 0;

	static constexpr U kMaxBufferedTimestamps = kMaxFramesInFlight + 1;
	class
	{
	public:
		Array<TimestampQueryPtr, kMaxBufferedTimestamps * 2> m_timestamps;
		Array<Second, kMaxBufferedTimestamps> m_cpuStartTimes;
		U8 m_nextTimestamp = 0;
	} m_statistics;

	RenderGraph(CString name);

	~RenderGraph();

	[[nodiscard]] static RenderGraph* newInstance();

	BakeContext* newContext(const RenderGraphDescription& descr, StackMemoryPool& pool);
	void initRenderPassesAndSetDeps(const RenderGraphDescription& descr);
	void initBatches();
	void initGraphicsPasses(const RenderGraphDescription& descr);
	void setBatchBarriers(const RenderGraphDescription& descr);

	TexturePtr getOrCreateRenderTarget(const TextureInitInfo& initInf, U64 hash);
	FramebufferPtr getOrCreateFramebuffer(const FramebufferDescription& fbDescr, const RenderTargetHandle* rtHandles, CString name,
										  Bool& drawsToPresentableTex);

	/// Every N number of frames clean unused cached items.
	void periodicCleanup();

	ANKI_HOT static Bool passADependsOnB(const RenderPassDescriptionBase& a, const RenderPassDescriptionBase& b);

	static Bool overlappingTextureSubresource(const TextureSubresourceInfo& suba, const TextureSubresourceInfo& subb);

	static Bool passHasUnmetDependencies(const BakeContext& ctx, U32 passIdx);

	void setTextureBarrier(Batch& batch, const RenderPassDependency& consumer);

	template<typename TFunc>
	static void iterateSurfsOrVolumes(const Texture& tex, const TextureSubresourceInfo& subresource, TFunc func);

	void getCrntUsage(RenderTargetHandle handle, U32 batchIdx, const TextureSubresourceInfo& subresource, TextureUsageBit& usage) const;

	/// @name Dump the dependency graph into a file.
	/// @{
	Error dumpDependencyDotFile(const RenderGraphDescription& descr, const BakeContext& ctx, CString path) const;
	static GrString textureUsageToStr(StackMemoryPool& pool, TextureUsageBit usage);
	static GrString bufferUsageToStr(StackMemoryPool& pool, BufferUsageBit usage);
	static GrString asUsageToStr(StackMemoryPool& pool, AccelerationStructureUsageBit usage);
	/// @}

	Texture& getTexture(RenderTargetHandle handle) const;
	void getCachedBuffer(BufferHandle handle, Buffer*& buff, PtrSize& offset, PtrSize& range) const;
	AccelerationStructure* getAs(AccelerationStructureHandle handle) const;
};
/// @}

} // end namespace anki

#include <AnKi/Gr/RenderGraph.inl.h>
