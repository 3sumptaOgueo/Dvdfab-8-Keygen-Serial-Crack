// Copyright (C) 2009-2023, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Renderer/DepthDownscale.h>
#include <AnKi/Renderer/Renderer.h>
#include <AnKi/Renderer/GBuffer.h>
#include <AnKi/Core/ConfigSet.h>

#if ANKI_COMPILER_GCC_COMPATIBLE
#	pragma GCC diagnostic push
#	pragma GCC diagnostic ignored "-Wunused-function"
#	pragma GCC diagnostic ignored "-Wignored-qualifiers"
#elif ANKI_COMPILER_MSVC
#	pragma warning(push)
#	pragma warning(disable : 4505)
#endif
#define A_CPU
#include <ThirdParty/FidelityFX/ffx_a.h>
#include <ThirdParty/FidelityFX/ffx_spd.h>
#if ANKI_COMPILER_GCC_COMPATIBLE
#	pragma GCC diagnostic pop
#elif ANKI_COMPILER_MSVC
#	pragma warning(pop)
#endif

namespace anki {

DepthDownscale::~DepthDownscale()
{
	if(m_clientBufferAddr)
	{
		m_clientBuffer->unmap();
	}
}

Error DepthDownscale::initInternal()
{
	const U32 width = getRenderer().getInternalResolution().x() >> 1;
	const U32 height = getRenderer().getInternalResolution().y() >> 1;

	m_mipCount = computeMaxMipmapCount2d(width, height, hHierachicalZMinHeight);

	m_lastMipSize.x() = width >> (m_mipCount - 1);
	m_lastMipSize.y() = height >> (m_mipCount - 1);

	ANKI_R_LOGV("Initializing HiZ. Mip count %u, last mip size %ux%u", m_mipCount, m_lastMipSize.x(), m_lastMipSize.y());

	const Bool preferCompute = ConfigSet::getSingleton().getRPreferCompute();
	const Bool supportsReductionSampler = GrManager::getSingleton().getDeviceCapabilities().m_samplingFilterMinMax;

	// Create RT descr
	{
		TextureUsageBit usage = TextureUsageBit::kAllSampled;
		if(preferCompute)
		{
			usage |= TextureUsageBit::kImageComputeWrite;
		}
		else
		{
			usage |= TextureUsageBit::kFramebufferWrite;
		}

		TextureInitInfo texInit = getRenderer().create2DRenderTargetInitInfo(width, height, Format::kR32_Sfloat, usage, "HiZ");
		texInit.m_mipmapCount = U8(m_mipCount);
		m_hizTex = getRenderer().createAndClearRenderTarget(texInit, TextureUsageBit::kSampledFragment);
	}

	// Progs
	if(preferCompute)
	{
		ANKI_CHECK(ResourceManager::getSingleton().loadResource("ShaderBinaries/DepthDownscaleCompute.ankiprogbin", m_prog));

		ShaderProgramResourceVariantInitInfo variantInitInfo(m_prog);
		variantInitInfo.addMutation("WAVE_OPERATIONS", 0);

		const ShaderProgramResourceVariant* variant;
		m_prog->getOrCreateVariant(variantInitInfo, variant);
		m_grProg.reset(&variant->getProgram());
	}
	else
	{
		ANKI_CHECK(ResourceManager::getSingleton().loadResource("ShaderBinaries/DepthDownscaleRaster.ankiprogbin", m_prog));

		ShaderProgramResourceVariantInitInfo variantInitInfo(m_prog);
		variantInitInfo.addMutation("REDUCTION_SAMPLER", supportsReductionSampler);

		const ShaderProgramResourceVariant* variant;
		m_prog->getOrCreateVariant(variantInitInfo, variant);
		m_grProg.reset(&variant->getProgram());

		// 1st mip prog
		variantInitInfo.addMutation("REDUCTION_SAMPLER", 1);
		m_prog->getOrCreateVariant(variantInitInfo, variant);
		m_firstMipGrProg.reset(&variant->getProgram());
	}

	// Counter buffer
	if(preferCompute)
	{
		BufferInitInfo buffInit("HiZCounterBuffer");
		buffInit.m_size = sizeof(U32);
		buffInit.m_usage = BufferUsageBit::kStorageComputeWrite | BufferUsageBit::kTransferDestination;
		m_counterBuffer = GrManager::getSingleton().newBuffer(buffInit);
	}

	// Client buffer
	{
		// Create buffer
		BufferInitInfo buffInit("HiZ Client");
		buffInit.m_mapAccess = BufferMapAccessBit::kRead;
		buffInit.m_size = PtrSize(m_lastMipSize.y()) * PtrSize(m_lastMipSize.x()) * sizeof(F32);
		buffInit.m_usage = BufferUsageBit::kStorageComputeWrite | BufferUsageBit::kStorageFragmentWrite;
		m_clientBuffer = GrManager::getSingleton().newBuffer(buffInit);

		m_clientBufferAddr = m_clientBuffer->map(0, buffInit.m_size, BufferMapAccessBit::kRead);

		// Fill the buffer with 1.0f
		for(U32 i = 0; i < m_lastMipSize.x() * m_lastMipSize.y(); ++i)
		{
			static_cast<F32*>(m_clientBufferAddr)[i] = 1.0f;
		}
	}

	// Reduction sampler
	if(!preferCompute && supportsReductionSampler)
	{
		SamplerInitInfo sinit("HiZReductionMax");
		sinit.m_addressing = SamplingAddressing::kClamp;
		sinit.m_mipmapFilter = SamplingFilter::kMax;
		sinit.m_minMagFilter = SamplingFilter::kMax;
		m_reductionSampler = GrManager::getSingleton().newSampler(sinit);
	}

	if(!preferCompute)
	{
		m_fbDescrs.resize(m_mipCount);
		for(U32 mip = 0; mip < m_mipCount; ++mip)
		{
			FramebufferDescription& fbDescr = m_fbDescrs[mip];
			fbDescr.m_colorAttachmentCount = 1;
			fbDescr.m_colorAttachments[0].m_surface.m_level = mip;
			fbDescr.bake();
		}
	}

	return Error::kNone;
}

Error DepthDownscale::init()
{
	const Error err = initInternal();
	if(err)
	{
		ANKI_R_LOGE("Failed to initialize depth downscale passes");
	}

	return err;
}

void DepthDownscale::importRenderTargets(RenderingContext& ctx)
{
	RenderGraphDescription& rgraph = ctx.m_renderGraphDescr;

	// Import RT
	if(m_hizTexImportedOnce)
	{
		m_runCtx.m_hizRt = rgraph.importRenderTarget(m_hizTex.get());
	}
	else
	{
		m_runCtx.m_hizRt = rgraph.importRenderTarget(m_hizTex.get(), TextureUsageBit::kSampledFragment);
		m_hizTexImportedOnce = true;
	}
}

void DepthDownscale::populateRenderGraph(RenderingContext& ctx)
{
	RenderGraphDescription& rgraph = ctx.m_renderGraphDescr;

	if(ConfigSet::getSingleton().getRPreferCompute())
	{
		// Do it with compute

		ComputeRenderPassDescription& pass = rgraph.newComputeRenderPass("HiZ");

		pass.newTextureDependency(getRenderer().getGBuffer().getDepthRt(), TextureUsageBit::kSampledCompute,
								  TextureSubresourceInfo(DepthStencilAspectBit::kDepth));

		for(U32 mip = 0; mip < m_mipCount; ++mip)
		{
			TextureSubresourceInfo subresource;
			subresource.m_firstMipmap = mip;
			pass.newTextureDependency(m_runCtx.m_hizRt, TextureUsageBit::kImageComputeWrite, subresource);
		}

		pass.setWork([this](RenderPassWorkContext& rgraphCtx) {
			runCompute(rgraphCtx);
		});
	}
	else
	{
		// Do it with raster

		for(U32 mip = 0; mip < m_mipCount; ++mip)
		{
			static constexpr Array<CString, 8> passNames = {"HiZ #1", "HiZ #2", "HiZ #3", "HiZ #4", "HiZ #5", "HiZ #6", "HiZ #7", "HiZ #8"};
			GraphicsRenderPassDescription& pass = rgraph.newGraphicsRenderPass(passNames[mip]);
			pass.setFramebufferInfo(m_fbDescrs[mip], {m_runCtx.m_hizRt});

			if(mip == 0)
			{
				pass.newTextureDependency(getRenderer().getGBuffer().getDepthRt(), TextureUsageBit::kSampledFragment,
										  TextureSubresourceInfo(DepthStencilAspectBit::kDepth));
			}
			else
			{
				TextureSurfaceInfo subresource;
				subresource.m_level = mip - 1;
				pass.newTextureDependency(m_runCtx.m_hizRt, TextureUsageBit::kSampledFragment, subresource);
			}

			TextureSurfaceInfo subresource;
			subresource.m_level = mip;
			pass.newTextureDependency(m_runCtx.m_hizRt, TextureUsageBit::kFramebufferWrite, subresource);

			pass.setWork([this, mip](RenderPassWorkContext& rgraphCtx) {
				runGraphics(mip, rgraphCtx);
			});
		}
	}
}

void DepthDownscale::runCompute(RenderPassWorkContext& rgraphCtx)
{
	CommandBufferPtr& cmdb = rgraphCtx.m_commandBuffer;

	// Zero the counter buffer before everything else
	if(!m_counterBufferZeroed) [[unlikely]]
	{
		m_counterBufferZeroed = true;

		cmdb->fillBuffer(m_counterBuffer.get(), 0, kMaxPtrSize, 0);

		const BufferBarrierInfo barrier = {m_counterBuffer.get(), BufferUsageBit::kTransferDestination, BufferUsageBit::kStorageComputeWrite, 0,
										   kMaxPtrSize};
		cmdb->setPipelineBarrier({}, {&barrier, 1}, {});
	}

	cmdb->bindShaderProgram(m_grProg.get());

	varAU2(dispatchThreadGroupCountXY);
	varAU2(workGroupOffset); // needed if Left and Top are not 0,0
	varAU2(numWorkGroupsAndMips);
	varAU4(rectInfo) = initAU4(0, 0, getRenderer().getInternalResolution().x(), getRenderer().getInternalResolution().y());
	SpdSetup(dispatchThreadGroupCountXY, workGroupOffset, numWorkGroupsAndMips, rectInfo);
	SpdSetup(dispatchThreadGroupCountXY, workGroupOffset, numWorkGroupsAndMips, rectInfo, m_mipCount);

	DepthDownscaleUniforms pc;
	pc.m_workgroupCount = numWorkGroupsAndMips[0];
	pc.m_mipmapCount = numWorkGroupsAndMips[1];
	pc.m_srcTexSizeOverOne = 1.0f / Vec2(getRenderer().getInternalResolution());
	pc.m_lastMipWidth = m_lastMipSize.x();

	cmdb->setPushConstants(&pc, sizeof(pc));

	constexpr U32 maxMipsSpdCanProduce = 12;
	for(U32 mip = 0; mip < maxMipsSpdCanProduce; ++mip)
	{
		TextureSubresourceInfo subresource;
		if(mip < m_mipCount)
		{
			subresource.m_firstMipmap = mip;
		}
		else
		{
			subresource.m_firstMipmap = 0;
		}

		rgraphCtx.bindImage(0, 0, m_runCtx.m_hizRt, subresource, mip);
	}

	if(m_mipCount >= 5)
	{
		TextureSubresourceInfo subresource;
		subresource.m_firstMipmap = 4;
		rgraphCtx.bindImage(0, 1, m_runCtx.m_hizRt, subresource);
	}
	else
	{
		// Bind something random
		TextureSubresourceInfo subresource;
		subresource.m_firstMipmap = 0;
		rgraphCtx.bindImage(0, 1, m_runCtx.m_hizRt, subresource);
	}

	cmdb->bindStorageBuffer(0, 2, m_counterBuffer.get(), 0, kMaxPtrSize);
	cmdb->bindStorageBuffer(0, 3, m_clientBuffer.get(), 0, kMaxPtrSize);

	cmdb->bindSampler(0, 4, getRenderer().getSamplers().m_trilinearClamp.get());
	rgraphCtx.bindTexture(0, 5, getRenderer().getGBuffer().getDepthRt(), TextureSubresourceInfo(DepthStencilAspectBit::kDepth));

	cmdb->dispatchCompute(dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1], 1);
}

void DepthDownscale::runGraphics(U32 mip, RenderPassWorkContext& rgraphCtx)
{
	CommandBufferPtr& cmdb = rgraphCtx.m_commandBuffer;

	if(mip == 0)
	{
		rgraphCtx.bindTexture(0, 0, getRenderer().getGBuffer().getDepthRt(), TextureSubresourceInfo(DepthStencilAspectBit::kDepth));

		cmdb->bindSampler(0, 1, getRenderer().getSamplers().m_trilinearClamp.get());

		cmdb->bindShaderProgram(m_firstMipGrProg.get());
	}
	else
	{
		TextureSubresourceInfo subresource;
		subresource.m_firstMipmap = mip - 1;
		rgraphCtx.bindTexture(0, 0, m_runCtx.m_hizRt, subresource);

		if(m_reductionSampler.isCreated())
		{
			cmdb->bindSampler(0, 1, m_reductionSampler.get());
		}
		else
		{
			cmdb->bindSampler(0, 1, getRenderer().getSamplers().m_trilinearClamp.get());
		}

		cmdb->bindShaderProgram(m_grProg.get());
	}

	cmdb->bindStorageBuffer(0, 2, m_clientBuffer.get(), 0, kMaxPtrSize);

	const UVec4 pc((mip != m_mipCount - 1) ? 0 : m_lastMipSize.x());
	cmdb->setPushConstants(&pc, sizeof(pc));

	const UVec2 size = (getRenderer().getInternalResolution() / 2) >> mip;
	cmdb->setViewport(0, 0, size.x(), size.y());
	cmdb->draw(PrimitiveTopology::kTriangles, 3);
}

} // end namespace anki
