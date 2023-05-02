// Copyright (C) 2009-2023, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Renderer/RtShadows.h>
#include <AnKi/Renderer/GBuffer.h>
#include <AnKi/Renderer/Renderer.h>
#include <AnKi/Renderer/ShadowMapping.h>
#include <AnKi/Renderer/AccelerationStructureBuilder.h>
#include <AnKi/Renderer/MotionVectors.h>
#include <AnKi/Renderer/DepthDownscale.h>
#include <AnKi/Renderer/RenderQueue.h>
#include <AnKi/Renderer/ClusterBinning.h>
#include <AnKi/Renderer/PackVisibleClusteredObjects.h>
#include <AnKi/Util/Tracer.h>
#include <AnKi/Core/ConfigSet.h>
#include <AnKi/Shaders/Include/MaterialTypes.h>
#include <AnKi/Shaders/Include/GpuSceneTypes.h>
#include <AnKi/Core/GPUMemory/UnifiedGeometryBuffer.h>
#include <AnKi/Core/GPUMemory/GpuSceneBuffer.h>

namespace anki {

Error RtShadows::init()
{
	const Error err = initInternal();
	if(err)
	{
		ANKI_R_LOGE("Failed to initialize ray traced shadows");
	}

	return err;
}

Error RtShadows::initInternal()
{
	ANKI_R_LOGV("Initializing RT shadows");

	m_useSvgf = ConfigSet::getSingleton().getRRtShadowsSvgf();
	m_atrousPassCount = ConfigSet::getSingleton().getRRtShadowsSvgfAtrousPassCount();

	ANKI_CHECK(ResourceManager::getSingleton().loadResource("EngineAssets/BlueNoise_Rgba8_64x64.png", m_blueNoiseImage));

	// Ray gen program
	{
		ANKI_CHECK(ResourceManager::getSingleton().loadResource("ShaderBinaries/RtShadowsRayGen.ankiprogbin", m_rayGenProg));

		ShaderProgramResourceVariantInitInfo variantInitInfo(m_rayGenProg);
		variantInitInfo.addMutation("RAYS_PER_PIXEL", ConfigSet::getSingleton().getRRtShadowsRaysPerPixel());

		const ShaderProgramResourceVariant* variant;
		m_rayGenProg->getOrCreateVariant(variantInitInfo, variant);
		m_rtLibraryGrProg.reset(&variant->getProgram());
		m_rayGenShaderGroupIdx = variant->getShaderGroupHandleIndex();
	}

	// Miss prog
	{
		ANKI_CHECK(ResourceManager::getSingleton().loadResource("ShaderBinaries/RtShadowsMiss.ankiprogbin", m_missProg));
		const ShaderProgramResourceVariant* variant;
		m_missProg->getOrCreateVariant(variant);
		m_missShaderGroupIdx = variant->getShaderGroupHandleIndex();
	}

	// Denoise program
	if(!m_useSvgf)
	{
		ANKI_CHECK(ResourceManager::getSingleton().loadResource("ShaderBinaries/RtShadowsDenoise.ankiprogbin", m_denoiseProg));
		ShaderProgramResourceVariantInitInfo variantInitInfo(m_denoiseProg);
		variantInitInfo.addConstant("kOutImageSize",
									UVec2(getRenderer().getInternalResolution().x() / 2, getRenderer().getInternalResolution().y() / 2));
		variantInitInfo.addConstant("kMinSampleCount", 8u);
		variantInitInfo.addConstant("kMaxSampleCount", 32u);
		variantInitInfo.addMutation("BLUR_ORIENTATION", 0);

		const ShaderProgramResourceVariant* variant;
		m_denoiseProg->getOrCreateVariant(variantInitInfo, variant);
		m_grDenoiseHorizontalProg.reset(&variant->getProgram());

		variantInitInfo.addMutation("BLUR_ORIENTATION", 1);
		m_denoiseProg->getOrCreateVariant(variantInitInfo, variant);
		m_grDenoiseVerticalProg.reset(&variant->getProgram());
	}

	// SVGF variance program
	if(m_useSvgf)
	{
		ANKI_CHECK(ResourceManager::getSingleton().loadResource("ShaderBinaries/RtShadowsSvgfVariance.ankiprogbin", m_svgfVarianceProg));
		ShaderProgramResourceVariantInitInfo variantInitInfo(m_svgfVarianceProg);
		variantInitInfo.addConstant("kFramebufferSize",
									UVec2(getRenderer().getInternalResolution().x() / 2, getRenderer().getInternalResolution().y() / 2));

		const ShaderProgramResourceVariant* variant;
		m_svgfVarianceProg->getOrCreateVariant(variantInitInfo, variant);
		m_svgfVarianceGrProg.reset(&variant->getProgram());
	}

	// SVGF atrous program
	if(m_useSvgf)
	{
		ANKI_CHECK(ResourceManager::getSingleton().loadResource("ShaderBinaries/RtShadowsSvgfAtrous.ankiprogbin", m_svgfAtrousProg));
		ShaderProgramResourceVariantInitInfo variantInitInfo(m_svgfAtrousProg);
		variantInitInfo.addConstant("kFramebufferSize",
									UVec2(getRenderer().getInternalResolution().x() / 2, getRenderer().getInternalResolution().y() / 2));
		variantInitInfo.addMutation("LAST_PASS", 0);

		const ShaderProgramResourceVariant* variant;
		m_svgfAtrousProg->getOrCreateVariant(variantInitInfo, variant);
		m_svgfAtrousGrProg.reset(&variant->getProgram());

		variantInitInfo.addMutation("LAST_PASS", 1);
		m_svgfAtrousProg->getOrCreateVariant(variantInitInfo, variant);
		m_svgfAtrousLastPassGrProg.reset(&variant->getProgram());
	}

	// Upscale program
	{
		ANKI_CHECK(ResourceManager::getSingleton().loadResource("ShaderBinaries/RtShadowsUpscale.ankiprogbin", m_upscaleProg));
		ShaderProgramResourceVariantInitInfo variantInitInfo(m_upscaleProg);
		variantInitInfo.addConstant("kOutImageSize", UVec2(getRenderer().getInternalResolution().x(), getRenderer().getInternalResolution().y()));

		const ShaderProgramResourceVariant* variant;
		m_upscaleProg->getOrCreateVariant(variantInitInfo, variant);
		m_upscaleGrProg.reset(&variant->getProgram());
	}

	// Debug program
	ANKI_CHECK(
		ResourceManager::getSingleton().loadResource("ShaderBinaries/RtShadowsVisualizeRenderTarget.ankiprogbin", m_visualizeRenderTargetsProg));

	// Quarter rez shadow RT
	{
		TextureInitInfo texinit = getRenderer().create2DRenderTargetInitInfo(
			getRenderer().getInternalResolution().x() / 2, getRenderer().getInternalResolution().y() / 2, Format::kR32G32_Uint,
			TextureUsageBit::kAllSampled | TextureUsageBit::kImageTraceRaysWrite | TextureUsageBit::kImageComputeWrite, "RtShadows History");
		m_historyRt = getRenderer().createAndClearRenderTarget(texinit, TextureUsageBit::kSampledFragment);
	}

	// Temp shadow RT
	{
		m_intermediateShadowsRtDescr = getRenderer().create2DRenderTargetDescription(
			getRenderer().getInternalResolution().x() / 2, getRenderer().getInternalResolution().y() / 2, Format::kR32G32_Uint, "RtShadows Tmp");
		m_intermediateShadowsRtDescr.bake();
	}

	// Moments RT
	{
		TextureInitInfo texinit = getRenderer().create2DRenderTargetInitInfo(
			getRenderer().getInternalResolution().x() / 2, getRenderer().getInternalResolution().y() / 2, Format::kR32G32_Sfloat,
			TextureUsageBit::kAllSampled | TextureUsageBit::kImageTraceRaysWrite | TextureUsageBit::kImageComputeWrite, "RtShadows Moments #1");
		m_momentsRts[0] = getRenderer().createAndClearRenderTarget(texinit, TextureUsageBit::kSampledFragment);

		texinit.setName("RtShadows Moments #2");
		m_momentsRts[1] = getRenderer().createAndClearRenderTarget(texinit, TextureUsageBit::kSampledFragment);
	}

	// Variance RT
	if(m_useSvgf)
	{
		m_varianceRtDescr = getRenderer().create2DRenderTargetDescription(
			getRenderer().getInternalResolution().x() / 2, getRenderer().getInternalResolution().y() / 2, Format::kR32_Sfloat, "RtShadows Variance");
		m_varianceRtDescr.bake();
	}

	// Final RT
	{
		m_upscaledRtDescr = getRenderer().create2DRenderTargetDescription(
			getRenderer().getInternalResolution().x(), getRenderer().getInternalResolution().y(), Format::kR32G32_Uint, "RtShadows Upscaled");
		m_upscaledRtDescr.bake();
	}

	// Misc
	m_sbtRecordSize = getAlignedRoundUp(GrManager::getSingleton().getDeviceCapabilities().m_sbtRecordAlignment, m_sbtRecordSize);

	return Error::kNone;
}

void RtShadows::populateRenderGraph(RenderingContext& ctx)
{
	ANKI_TRACE_SCOPED_EVENT(RRtShadows);

	RenderGraphDescription& rgraph = ctx.m_renderGraphDescr;

	buildSbt(ctx);
	const U32 prevRtIdx = getRenderer().getFrameCount() & 1;

	// Import RTs
	{
		if(!m_rtsImportedOnce) [[unlikely]]
		{
			m_runCtx.m_historyRt = rgraph.importRenderTarget(m_historyRt.get(), TextureUsageBit::kSampledFragment);

			m_runCtx.m_prevMomentsRt = rgraph.importRenderTarget(m_momentsRts[prevRtIdx].get(), TextureUsageBit::kSampledFragment);

			m_rtsImportedOnce = true;
		}
		else
		{
			m_runCtx.m_historyRt = rgraph.importRenderTarget(m_historyRt.get());
			m_runCtx.m_prevMomentsRt = rgraph.importRenderTarget(m_momentsRts[prevRtIdx].get());
		}

		if((getPassCountWithoutUpscaling() % 2) == 1)
		{
			m_runCtx.m_intermediateShadowsRts[0] = rgraph.newRenderTarget(m_intermediateShadowsRtDescr);
			m_runCtx.m_intermediateShadowsRts[1] = rgraph.newRenderTarget(m_intermediateShadowsRtDescr);
		}
		else
		{
			// We can save a render target if we have even number of renderpasses
			m_runCtx.m_intermediateShadowsRts[0] = rgraph.newRenderTarget(m_intermediateShadowsRtDescr);
			m_runCtx.m_intermediateShadowsRts[1] = m_runCtx.m_historyRt;
		}

		m_runCtx.m_currentMomentsRt = rgraph.importRenderTarget(m_momentsRts[!prevRtIdx].get(), TextureUsageBit::kNone);

		if(m_useSvgf)
		{
			if(m_atrousPassCount > 1)
			{
				m_runCtx.m_varianceRts[0] = rgraph.newRenderTarget(m_varianceRtDescr);
			}
			m_runCtx.m_varianceRts[1] = rgraph.newRenderTarget(m_varianceRtDescr);
		}

		m_runCtx.m_upscaledRt = rgraph.newRenderTarget(m_upscaledRtDescr);
	}

#define ANKI_DEPTH_DEP \
	getRenderer().getDepthDownscale().getHiZRt(), TextureUsageBit::kSampledTraceRays | TextureUsageBit::kSampledCompute, kHiZHalfSurface

	// RT shadows pass
	{
		ComputeRenderPassDescription& rpass = rgraph.newComputeRenderPass("RtShadows");
		rpass.setWork([this](RenderPassWorkContext& rgraphCtx) {
			run(rgraphCtx);
		});

		rpass.newTextureDependency(m_runCtx.m_historyRt, TextureUsageBit::kSampledTraceRays);
		rpass.newTextureDependency(m_runCtx.m_intermediateShadowsRts[0], TextureUsageBit::kImageTraceRaysWrite);
		rpass.newAccelerationStructureDependency(getRenderer().getAccelerationStructureBuilder().getAccelerationStructureHandle(),
												 AccelerationStructureUsageBit::kTraceRaysRead);
		rpass.newTextureDependency(ANKI_DEPTH_DEP);
		rpass.newTextureDependency(getRenderer().getMotionVectors().getMotionVectorsRt(), TextureUsageBit::kSampledTraceRays);
		rpass.newTextureDependency(getRenderer().getMotionVectors().getHistoryLengthRt(), TextureUsageBit::kSampledTraceRays);
		rpass.newTextureDependency(getRenderer().getGBuffer().getColorRt(2), TextureUsageBit::kSampledTraceRays);

		rpass.newTextureDependency(m_runCtx.m_prevMomentsRt, TextureUsageBit::kSampledTraceRays);
		rpass.newTextureDependency(m_runCtx.m_currentMomentsRt, TextureUsageBit::kImageTraceRaysWrite);

		rpass.newBufferDependency(getRenderer().getClusterBinning().getClustersRenderGraphHandle(), BufferUsageBit::kStorageTraceRaysRead);
	}

	// Denoise pass horizontal
	if(!m_useSvgf)
	{
		ComputeRenderPassDescription& rpass = rgraph.newComputeRenderPass("RtShadows Denoise Horizontal");
		rpass.setWork([this, &ctx](RenderPassWorkContext& rgraphCtx) {
			runDenoise(ctx, rgraphCtx);
		});

		rpass.newTextureDependency(m_runCtx.m_intermediateShadowsRts[0], TextureUsageBit::kSampledCompute);
		rpass.newTextureDependency(ANKI_DEPTH_DEP);
		rpass.newTextureDependency(getRenderer().getGBuffer().getColorRt(2), TextureUsageBit::kSampledCompute);
		rpass.newTextureDependency(m_runCtx.m_currentMomentsRt, TextureUsageBit::kSampledCompute);
		rpass.newTextureDependency(getRenderer().getMotionVectors().getHistoryLengthRt(), TextureUsageBit::kSampledCompute);

		rpass.newTextureDependency(m_runCtx.m_intermediateShadowsRts[1], TextureUsageBit::kImageComputeWrite);
	}

	// Denoise pass vertical
	if(!m_useSvgf)
	{
		ComputeRenderPassDescription& rpass = rgraph.newComputeRenderPass("RtShadows Denoise Vertical");
		rpass.setWork([this, &ctx](RenderPassWorkContext& rgraphCtx) {
			runDenoise(ctx, rgraphCtx);
		});

		rpass.newTextureDependency(m_runCtx.m_intermediateShadowsRts[1], TextureUsageBit::kSampledCompute);
		rpass.newTextureDependency(ANKI_DEPTH_DEP);
		rpass.newTextureDependency(getRenderer().getGBuffer().getColorRt(2), TextureUsageBit::kSampledCompute);
		rpass.newTextureDependency(m_runCtx.m_currentMomentsRt, TextureUsageBit::kSampledCompute);
		rpass.newTextureDependency(getRenderer().getMotionVectors().getHistoryLengthRt(), TextureUsageBit::kSampledCompute);

		rpass.newTextureDependency(m_runCtx.m_historyRt, TextureUsageBit::kImageComputeWrite);
	}

	// Variance calculation pass
	if(m_useSvgf)
	{
		ComputeRenderPassDescription& rpass = rgraph.newComputeRenderPass("RtShadows SVGF Variance");
		rpass.setWork([this, &ctx](RenderPassWorkContext& rgraphCtx) {
			runSvgfVariance(ctx, rgraphCtx);
		});

		rpass.newTextureDependency(m_runCtx.m_intermediateShadowsRts[0], TextureUsageBit::kSampledCompute);
		rpass.newTextureDependency(m_runCtx.m_currentMomentsRt, TextureUsageBit::kSampledCompute);
		rpass.newTextureDependency(getRenderer().getMotionVectors().getHistoryLengthRt(), TextureUsageBit::kSampledCompute);
		rpass.newTextureDependency(ANKI_DEPTH_DEP);
		rpass.newTextureDependency(getRenderer().getGBuffer().getColorRt(2), TextureUsageBit::kSampledCompute);

		rpass.newTextureDependency(m_runCtx.m_intermediateShadowsRts[1], TextureUsageBit::kImageComputeWrite);
		rpass.newTextureDependency(m_runCtx.m_varianceRts[1], TextureUsageBit::kImageComputeWrite);
	}

	// SVGF Atrous
	if(m_useSvgf)
	{
		m_runCtx.m_atrousPassIdx = 0;

		for(U32 i = 0; i < m_atrousPassCount; ++i)
		{
			const Bool lastPass = i == U32(m_atrousPassCount - 1);
			const U32 readRtIdx = (i + 1) & 1;

			ComputeRenderPassDescription& rpass = rgraph.newComputeRenderPass("RtShadows SVGF Atrous");
			rpass.setWork([this, &ctx](RenderPassWorkContext& rgraphCtx) {
				runSvgfAtrous(ctx, rgraphCtx);
			});

			rpass.newTextureDependency(ANKI_DEPTH_DEP);
			rpass.newTextureDependency(getRenderer().getGBuffer().getColorRt(2), TextureUsageBit::kSampledCompute);
			rpass.newTextureDependency(m_runCtx.m_intermediateShadowsRts[readRtIdx], TextureUsageBit::kSampledCompute);
			rpass.newTextureDependency(m_runCtx.m_varianceRts[readRtIdx], TextureUsageBit::kSampledCompute);

			if(!lastPass)
			{
				rpass.newTextureDependency(m_runCtx.m_intermediateShadowsRts[!readRtIdx], TextureUsageBit::kImageComputeWrite);

				rpass.newTextureDependency(m_runCtx.m_varianceRts[!readRtIdx], TextureUsageBit::kImageComputeWrite);
			}
			else
			{
				rpass.newTextureDependency(m_runCtx.m_historyRt, TextureUsageBit::kImageComputeWrite);
			}
		}
	}

	// Upscale
	{
		ComputeRenderPassDescription& rpass = rgraph.newComputeRenderPass("RtShadows Upscale");
		rpass.setWork([this](RenderPassWorkContext& rgraphCtx) {
			runUpscale(rgraphCtx);
		});

		rpass.newTextureDependency(m_runCtx.m_historyRt, TextureUsageBit::kSampledCompute);
		rpass.newTextureDependency(getRenderer().getGBuffer().getDepthRt(), TextureUsageBit::kSampledCompute);
		rpass.newTextureDependency(ANKI_DEPTH_DEP);

		rpass.newTextureDependency(m_runCtx.m_upscaledRt, TextureUsageBit::kImageComputeWrite);
	}

	// Find out the lights that will take part in RT pass
	{
		RenderQueue& rqueue = *ctx.m_renderQueue;
		m_runCtx.m_layersWithRejectedHistory.unsetAll();

		if(rqueue.m_directionalLight.hasShadow())
		{
			U32 layerIdx;
			Bool rejectHistory;
			[[maybe_unused]] const Bool layerFound = findShadowLayer(0, layerIdx, rejectHistory);
			ANKI_ASSERT(layerFound && "Directional can't fail");

			rqueue.m_directionalLight.m_shadowLayer = U8(layerIdx);
			ANKI_ASSERT(rqueue.m_directionalLight.m_shadowLayer < kMaxRtShadowLayers);
			m_runCtx.m_layersWithRejectedHistory.set(layerIdx, rejectHistory);
		}

		for(PointLightQueueElement& light : rqueue.m_pointLights)
		{
			if(!light.hasShadow())
			{
				continue;
			}

			U32 layerIdx;
			Bool rejectHistory;
			const Bool layerFound = findShadowLayer(light.m_uuid, layerIdx, rejectHistory);

			if(layerFound)
			{
				light.m_shadowLayer = U8(layerIdx);
				ANKI_ASSERT(light.m_shadowLayer < kMaxRtShadowLayers);
				m_runCtx.m_layersWithRejectedHistory.set(layerIdx, rejectHistory);
			}
			else
			{
				// Disable shadows
				light.m_shadowRenderQueues = {};
			}
		}

		for(SpotLightQueueElement& light : rqueue.m_spotLights)
		{
			if(!light.hasShadow())
			{
				continue;
			}

			U32 layerIdx;
			Bool rejectHistory;
			const Bool layerFound = findShadowLayer(light.m_uuid, layerIdx, rejectHistory);

			if(layerFound)
			{
				light.m_shadowLayer = U8(layerIdx);
				ANKI_ASSERT(light.m_shadowLayer < kMaxRtShadowLayers);
				m_runCtx.m_layersWithRejectedHistory.set(layerIdx, rejectHistory);
			}
			else
			{
				// Disable shadows
				light.m_shadowRenderQueue = nullptr;
			}
		}
	}
}

void RtShadows::run(RenderPassWorkContext& rgraphCtx)
{
	CommandBuffer& cmdb = *rgraphCtx.m_commandBuffer;

	cmdb.bindShaderProgram(m_rtLibraryGrProg.get());

	// Allocate, set and bind global uniforms
	{
		RebarAllocation globalUniformsToken;
		MaterialGlobalUniforms* globalUniforms = static_cast<MaterialGlobalUniforms*>(
			RebarTransientMemoryPool::getSingleton().allocateFrame(sizeof(MaterialGlobalUniforms), globalUniformsToken));

		memset(globalUniforms, 0, sizeof(*globalUniforms)); // Don't care for now

		cmdb.bindUniformBuffer(U32(MaterialSet::kGlobal), U32(MaterialBinding::kGlobalUniforms),
							   &RebarTransientMemoryPool::getSingleton().getBuffer(), globalUniformsToken.m_offset, globalUniformsToken.m_range);
	}

	// More globals
	cmdb.bindAllBindless(U32(MaterialSet::kBindless));
	cmdb.bindSampler(U32(MaterialSet::kGlobal), U32(MaterialBinding::kTrilinearRepeatSampler), getRenderer().getSamplers().m_trilinearRepeat.get());
	cmdb.bindStorageBuffer(U32(MaterialSet::kGlobal), U32(MaterialBinding::kGpuScene), &GpuSceneBuffer::getSingleton().getBuffer(), 0, kMaxPtrSize);

#define ANKI_UNIFIED_GEOM_FORMAT(fmt, shaderType) \
	cmdb.bindReadOnlyTextureBuffer(U32(MaterialSet::kGlobal), U32(MaterialBinding::kUnifiedGeometry_##fmt), \
								   &UnifiedGeometryBuffer::getSingleton().getBuffer(), 0, kMaxPtrSize, Format::k##fmt);
#include <AnKi/Shaders/Include/UnifiedGeometryTypes.defs.h>

	constexpr U32 kSet = 2;

	bindUniforms(cmdb, kSet, 0, getRenderer().getClusterBinning().getClusteredUniformsRebarToken());

	getRenderer().getPackVisibleClusteredObjects().bindClusteredObjectBuffer(cmdb, kSet, 1, ClusteredObjectType::kPointLight);
	getRenderer().getPackVisibleClusteredObjects().bindClusteredObjectBuffer(cmdb, kSet, 2, ClusteredObjectType::kSpotLight);
	rgraphCtx.bindColorTexture(kSet, 3, getRenderer().getShadowMapping().getShadowmapRt());

	bindStorage(cmdb, kSet, 4, getRenderer().getClusterBinning().getClustersRebarToken());

	cmdb.bindSampler(kSet, 5, getRenderer().getSamplers().m_trilinearRepeat.get());

	rgraphCtx.bindImage(kSet, 6, m_runCtx.m_intermediateShadowsRts[0]);

	rgraphCtx.bindColorTexture(kSet, 7, m_runCtx.m_historyRt);
	cmdb.bindSampler(kSet, 8, getRenderer().getSamplers().m_trilinearClamp.get());
	cmdb.bindSampler(kSet, 9, getRenderer().getSamplers().m_nearestNearestClamp.get());
	rgraphCtx.bindTexture(kSet, 10, getRenderer().getDepthDownscale().getHiZRt(), kHiZHalfSurface);
	rgraphCtx.bindColorTexture(kSet, 11, getRenderer().getMotionVectors().getMotionVectorsRt());
	rgraphCtx.bindColorTexture(kSet, 12, getRenderer().getMotionVectors().getHistoryLengthRt());
	rgraphCtx.bindColorTexture(kSet, 13, getRenderer().getGBuffer().getColorRt(2));
	rgraphCtx.bindAccelerationStructure(kSet, 14, getRenderer().getAccelerationStructureBuilder().getAccelerationStructureHandle());
	rgraphCtx.bindColorTexture(kSet, 15, m_runCtx.m_prevMomentsRt);
	rgraphCtx.bindImage(kSet, 16, m_runCtx.m_currentMomentsRt);
	cmdb.bindTexture(kSet, 17, &m_blueNoiseImage->getTextureView());

	RtShadowsUniforms unis;
	for(U32 i = 0; i < kMaxRtShadowLayers; ++i)
	{
		unis.historyRejectFactor[i] = F32(m_runCtx.m_layersWithRejectedHistory.get(i));
	}
	cmdb.setPushConstants(&unis, sizeof(unis));

	cmdb.traceRays(m_runCtx.m_sbtBuffer.get(), m_runCtx.m_sbtOffset, m_sbtRecordSize, m_runCtx.m_hitGroupCount, 1,
				   getRenderer().getInternalResolution().x() / 2, getRenderer().getInternalResolution().y() / 2, 1);
}

void RtShadows::runDenoise(const RenderingContext& ctx, RenderPassWorkContext& rgraphCtx)
{
	CommandBuffer& cmdb = *rgraphCtx.m_commandBuffer;

	cmdb.bindShaderProgram((m_runCtx.m_denoiseOrientation == 0) ? m_grDenoiseHorizontalProg.get() : m_grDenoiseVerticalProg.get());

	cmdb.bindSampler(0, 0, getRenderer().getSamplers().m_nearestNearestClamp.get());
	cmdb.bindSampler(0, 1, getRenderer().getSamplers().m_trilinearClamp.get());
	rgraphCtx.bindColorTexture(0, 2, m_runCtx.m_intermediateShadowsRts[m_runCtx.m_denoiseOrientation]);
	rgraphCtx.bindTexture(0, 3, getRenderer().getDepthDownscale().getHiZRt(), kHiZHalfSurface);
	rgraphCtx.bindColorTexture(0, 4, getRenderer().getGBuffer().getColorRt(2));
	rgraphCtx.bindColorTexture(0, 5, m_runCtx.m_currentMomentsRt);
	rgraphCtx.bindColorTexture(0, 6, getRenderer().getMotionVectors().getHistoryLengthRt());

	rgraphCtx.bindImage(0, 7, (m_runCtx.m_denoiseOrientation == 0) ? m_runCtx.m_intermediateShadowsRts[1] : m_runCtx.m_historyRt);

	RtShadowsDenoiseUniforms unis;
	unis.invViewProjMat = ctx.m_matrices.m_invertedViewProjectionJitter;
	unis.time = F32(GlobalFrameIndex::getSingleton().m_value);
	cmdb.setPushConstants(&unis, sizeof(unis));

	dispatchPPCompute(cmdb, 8, 8, getRenderer().getInternalResolution().x() / 2, getRenderer().getInternalResolution().y() / 2);

	m_runCtx.m_denoiseOrientation = !m_runCtx.m_denoiseOrientation;
}

void RtShadows::runSvgfVariance(const RenderingContext& ctx, RenderPassWorkContext& rgraphCtx)
{
	CommandBuffer& cmdb = *rgraphCtx.m_commandBuffer;

	cmdb.bindShaderProgram(m_svgfVarianceGrProg.get());

	cmdb.bindSampler(0, 0, getRenderer().getSamplers().m_nearestNearestClamp.get());
	cmdb.bindSampler(0, 1, getRenderer().getSamplers().m_trilinearClamp.get());

	rgraphCtx.bindColorTexture(0, 2, m_runCtx.m_intermediateShadowsRts[0]);
	rgraphCtx.bindColorTexture(0, 3, m_runCtx.m_currentMomentsRt);
	rgraphCtx.bindColorTexture(0, 4, getRenderer().getMotionVectors().getHistoryLengthRt());
	rgraphCtx.bindTexture(0, 5, getRenderer().getDepthDownscale().getHiZRt(), kHiZHalfSurface);

	rgraphCtx.bindImage(0, 6, m_runCtx.m_intermediateShadowsRts[1]);
	rgraphCtx.bindImage(0, 7, m_runCtx.m_varianceRts[1]);

	const Mat4& invProjMat = ctx.m_matrices.m_projectionJitter.getInverse();
	cmdb.setPushConstants(&invProjMat, sizeof(invProjMat));

	dispatchPPCompute(cmdb, 8, 8, getRenderer().getInternalResolution().x() / 2, getRenderer().getInternalResolution().y() / 2);
}

void RtShadows::runSvgfAtrous(const RenderingContext& ctx, RenderPassWorkContext& rgraphCtx)
{
	CommandBuffer& cmdb = *rgraphCtx.m_commandBuffer;

	const Bool lastPass = m_runCtx.m_atrousPassIdx == m_atrousPassCount - 1;
	const U32 readRtIdx = (m_runCtx.m_atrousPassIdx + 1) & 1;

	if(lastPass)
	{
		cmdb.bindShaderProgram(m_svgfAtrousLastPassGrProg.get());
	}
	else
	{
		cmdb.bindShaderProgram(m_svgfAtrousGrProg.get());
	}

	cmdb.bindSampler(0, 0, getRenderer().getSamplers().m_nearestNearestClamp.get());
	cmdb.bindSampler(0, 1, getRenderer().getSamplers().m_trilinearClamp.get());

	rgraphCtx.bindTexture(0, 2, getRenderer().getDepthDownscale().getHiZRt(), kHiZHalfSurface);
	rgraphCtx.bindColorTexture(0, 3, m_runCtx.m_intermediateShadowsRts[readRtIdx]);
	rgraphCtx.bindColorTexture(0, 4, m_runCtx.m_varianceRts[readRtIdx]);

	if(!lastPass)
	{
		rgraphCtx.bindImage(0, 5, m_runCtx.m_intermediateShadowsRts[!readRtIdx]);
		rgraphCtx.bindImage(0, 6, m_runCtx.m_varianceRts[!readRtIdx]);
	}
	else
	{
		rgraphCtx.bindImage(0, 5, m_runCtx.m_historyRt);
	}

	const Mat4& invProjMat = ctx.m_matrices.m_projectionJitter.getInverse();
	cmdb.setPushConstants(&invProjMat, sizeof(invProjMat));

	dispatchPPCompute(cmdb, 8, 8, getRenderer().getInternalResolution().x() / 2, getRenderer().getInternalResolution().y() / 2);

	++m_runCtx.m_atrousPassIdx;
}

void RtShadows::runUpscale(RenderPassWorkContext& rgraphCtx)
{
	CommandBuffer& cmdb = *rgraphCtx.m_commandBuffer;

	cmdb.bindShaderProgram(m_upscaleGrProg.get());

	cmdb.bindSampler(0, 0, getRenderer().getSamplers().m_nearestNearestClamp.get());
	cmdb.bindSampler(0, 1, getRenderer().getSamplers().m_trilinearClamp.get());

	rgraphCtx.bindColorTexture(0, 2, m_runCtx.m_historyRt);
	rgraphCtx.bindImage(0, 3, m_runCtx.m_upscaledRt);
	rgraphCtx.bindTexture(0, 4, getRenderer().getDepthDownscale().getHiZRt(), kHiZHalfSurface);
	rgraphCtx.bindTexture(0, 5, getRenderer().getGBuffer().getDepthRt(), TextureSubresourceInfo(DepthStencilAspectBit::kDepth));

	dispatchPPCompute(cmdb, 8, 8, getRenderer().getInternalResolution().x(), getRenderer().getInternalResolution().y());
}

void RtShadows::buildSbt(RenderingContext& ctx)
{
	// Get some things
	ANKI_ASSERT(ctx.m_renderQueue->m_rayTracingQueue);
	ConstWeakArray<RayTracingInstanceQueueElement> instanceElements = ctx.m_renderQueue->m_rayTracingQueue->m_rayTracingInstances;
	const U32 instanceCount = instanceElements.getSize();
	ANKI_ASSERT(instanceCount > 0);

	const U32 shaderHandleSize = GrManager::getSingleton().getDeviceCapabilities().m_shaderGroupHandleSize;

	const U32 extraSbtRecords = 1 + 1; // Raygen + miss

	m_runCtx.m_hitGroupCount = instanceCount;

	// Allocate SBT
	RebarAllocation token;
	U8* sbt = allocateStorage<U8*>(PtrSize(m_sbtRecordSize) * (instanceCount + extraSbtRecords), token);
	[[maybe_unused]] const U8* sbtStart = sbt;
	m_runCtx.m_sbtBuffer.reset(const_cast<Buffer*>(&RebarTransientMemoryPool::getSingleton().getBuffer()));
	m_runCtx.m_sbtOffset = token.m_offset;

	// Set the miss and ray gen handles
	ConstWeakArray<U8> shaderGroupHandles = m_rtLibraryGrProg->getShaderGroupHandles();
	memcpy(sbt, &shaderGroupHandles[m_rayGenShaderGroupIdx * shaderHandleSize], shaderHandleSize);
	sbt += m_sbtRecordSize;
	memcpy(sbt, &shaderGroupHandles[m_missShaderGroupIdx * shaderHandleSize], shaderHandleSize);
	sbt += m_sbtRecordSize;

	// Init SBT and instances
	ANKI_ASSERT(m_sbtRecordSize >= shaderHandleSize + sizeof(ModelGpuDescriptor));
	for(U32 instanceIdx = 0; instanceIdx < instanceCount; ++instanceIdx)
	{
		const RayTracingInstanceQueueElement& element = instanceElements[instanceIdx];

		// Init SBT record
		memcpy(sbt, &shaderGroupHandles[element.m_shaderGroupHandleIndex * shaderHandleSize], shaderHandleSize);

		ANKI_ASSERT(shaderHandleSize + sizeof(GpuSceneRenderable) <= m_sbtRecordSize);
		GpuSceneRenderable* shaderRecord = reinterpret_cast<GpuSceneRenderable*>(sbt + shaderHandleSize);
		shaderRecord->m_worldTransformsOffset = element.m_worldTransformsOffset;
		shaderRecord->m_uniformsOffset = element.m_uniformsOffset;
		shaderRecord->m_geometryOffset = element.m_geometryOffset;
		shaderRecord->m_boneTransformsOffset = 0;

		sbt += m_sbtRecordSize;
	}

	ANKI_ASSERT(sbtStart + m_sbtRecordSize * (instanceCount + extraSbtRecords) == sbt);
}

Bool RtShadows::findShadowLayer(U64 lightUuid, U32& layerIdx, Bool& rejectHistoryBuffer)
{
	const U64 crntFrame = getRenderer().getFrameCount();
	layerIdx = kMaxU32;
	U32 nextBestLayerIdx = kMaxU32;
	U64 nextBestLayerFame = crntFrame;
	rejectHistoryBuffer = false;

	for(U32 i = 0; i < m_shadowLayers.getSize(); ++i)
	{
		ShadowLayer& layer = m_shadowLayers[i];
		if(layer.m_lightUuid == lightUuid && layer.m_frameLastUsed == crntFrame - 1)
		{
			// Found it being used last frame
			layerIdx = i;
			layer.m_frameLastUsed = crntFrame;
			layer.m_lightUuid = lightUuid;
			break;
		}
		else if(layer.m_lightUuid == lightUuid || layer.m_frameLastUsed == kMaxU64)
		{
			// Found an empty slot or slot used by the same light
			layerIdx = i;
			layer.m_frameLastUsed = crntFrame;
			layer.m_lightUuid = lightUuid;
			rejectHistoryBuffer = true;
			break;
		}
		else if(layer.m_frameLastUsed < nextBestLayerFame)
		{
			nextBestLayerIdx = i;
			nextBestLayerFame = crntFrame;
		}
	}

	// Not found but there is a good candidate. Use that
	if(layerIdx == kMaxU32 && nextBestLayerIdx != kMaxU32)
	{
		layerIdx = nextBestLayerIdx;
		m_shadowLayers[nextBestLayerIdx].m_frameLastUsed = crntFrame;
		m_shadowLayers[nextBestLayerIdx].m_lightUuid = lightUuid;
		rejectHistoryBuffer = true;
	}

	return layerIdx != kMaxU32;
}

void RtShadows::getDebugRenderTarget(CString rtName, Array<RenderTargetHandle, kMaxDebugRenderTargets>& handles,
									 ShaderProgramPtr& optionalShaderProgram) const
{
	U32 layerGroup = 0;
	if(rtName == "RtShadows")
	{
		layerGroup = 0;
	}
	else if(rtName == "RtShadows1")
	{
		layerGroup = 1;
	}
	else
	{
		ANKI_ASSERT(rtName == "RtShadows2");
		layerGroup = 2;
	}

	handles[0] = m_runCtx.m_upscaledRt;

	ShaderProgramResourceVariantInitInfo variantInit(m_visualizeRenderTargetsProg);
	variantInit.addMutation("LAYER_GROUP", layerGroup);

	const ShaderProgramResourceVariant* variant;
	m_visualizeRenderTargetsProg->getOrCreateVariant(variantInit, variant);
	optionalShaderProgram.reset(&variant->getProgram());
}

} // end namespace anki
