// Copyright (C) 2009-2023, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Renderer/VolumetricLightingAccumulation.h>
#include <AnKi/Renderer/ShadowMapping.h>
#include <AnKi/Renderer/IndirectDiffuseProbes.h>
#include <AnKi/Renderer/Renderer.h>
#include <AnKi/Renderer/ClusterBinning.h>
#include <AnKi/Renderer/PackVisibleClusteredObjects.h>
#include <AnKi/Resource/ImageResource.h>
#include <AnKi/Core/ConfigSet.h>

namespace anki {

NumericCVar<F32> g_volumetricLightingAccumulationQualityXYCVar(CVarSubsystem::kRenderer, "VolumetricLightingAccumulationQualityXY", 4.0f, 1.0f, 16.0f,
															   "Quality of XY dimensions of volumetric lights");
NumericCVar<F32> g_volumetricLightingAccumulationQualityZCVar(CVarSubsystem::kRenderer, "VolumetricLightingAccumulationQualityZ", 4.0f, 1.0f, 16.0f,
															  "Quality of Z dimension of volumetric lights");
NumericCVar<U32> g_volumetricLightingAccumulationFinalZSplitCVar(CVarSubsystem::kRenderer, "VolumetricLightingAccumulationFinalZSplit", 26, 1, 256,
																 "Final cluster split that will recieve volumetric lights");

Error VolumetricLightingAccumulation::init()
{
	// Misc
	const F32 qualityXY = g_volumetricLightingAccumulationQualityXYCVar.get();
	const F32 qualityZ = g_volumetricLightingAccumulationQualityZCVar.get();
	m_finalZSplit = min(getRenderer().getZSplitCount() - 1, g_volumetricLightingAccumulationFinalZSplitCVar.get());

	m_volumeSize[0] = U32(F32(getRenderer().getTileCounts().x()) * qualityXY);
	m_volumeSize[1] = U32(F32(getRenderer().getTileCounts().y()) * qualityXY);
	m_volumeSize[2] = U32(F32(m_finalZSplit + 1) * qualityZ);
	ANKI_R_LOGV("Initializing volumetric lighting accumulation. Size %ux%ux%u", m_volumeSize[0], m_volumeSize[1], m_volumeSize[2]);

	if(!isAligned(getRenderer().getTileCounts().x(), m_volumeSize[0]) || !isAligned(getRenderer().getTileCounts().y(), m_volumeSize[1])
	   || m_volumeSize[0] == 0 || m_volumeSize[1] == 0 || m_volumeSize[2] == 0)
	{
		ANKI_R_LOGE("Wrong input");
		return Error::kUserData;
	}

	ANKI_CHECK(ResourceManager::getSingleton().loadResource("EngineAssets/BlueNoise_Rgba8_64x64.png", m_noiseImage));

	// Shaders
	ANKI_CHECK(ResourceManager::getSingleton().loadResource("ShaderBinaries/VolumetricLightingAccumulation.ankiprogbin", m_prog));

	ShaderProgramResourceVariantInitInfo variantInitInfo(m_prog);
	variantInitInfo.addMutation("ENABLE_SHADOWS", 1);

	const ShaderProgramResourceVariant* variant;
	m_prog->getOrCreateVariant(variantInitInfo, variant);
	m_grProg.reset(&variant->getProgram());
	m_workgroupSize = variant->getWorkgroupSizes();

	// Create RTs
	TextureInitInfo texinit = getRenderer().create2DRenderTargetInitInfo(m_volumeSize[0], m_volumeSize[1], Format::kR16G16B16A16_Sfloat,
																		 TextureUsageBit::kImageComputeRead | TextureUsageBit::kImageComputeWrite
																			 | TextureUsageBit::kSampledFragment | TextureUsageBit::kSampledCompute,
																		 "VolLight");
	texinit.m_depth = m_volumeSize[2];
	texinit.m_type = TextureType::k3D;
	m_rtTextures[0] = getRenderer().createAndClearRenderTarget(texinit, TextureUsageBit::kSampledFragment);
	m_rtTextures[1] = getRenderer().createAndClearRenderTarget(texinit, TextureUsageBit::kSampledFragment);

	return Error::kNone;
}

void VolumetricLightingAccumulation::populateRenderGraph(RenderingContext& ctx)
{
	RenderGraphDescription& rgraph = ctx.m_renderGraphDescr;

	const U readRtIdx = getRenderer().getFrameCount() & 1;

	m_runCtx.m_rts[0] = rgraph.importRenderTarget(m_rtTextures[readRtIdx].get(), TextureUsageBit::kSampledFragment);
	m_runCtx.m_rts[1] = rgraph.importRenderTarget(m_rtTextures[!readRtIdx].get(), TextureUsageBit::kNone);

	ComputeRenderPassDescription& pass = rgraph.newComputeRenderPass("Vol light");

	pass.setWork([this, &ctx](RenderPassWorkContext& rgraphCtx) {
		run(ctx, rgraphCtx);
	});

	pass.newTextureDependency(m_runCtx.m_rts[0], TextureUsageBit::kSampledCompute);
	pass.newTextureDependency(m_runCtx.m_rts[1], TextureUsageBit::kImageComputeWrite);
	pass.newTextureDependency(getRenderer().getShadowMapping().getShadowmapRt(), TextureUsageBit::kSampledCompute);

	pass.newBufferDependency(getRenderer().getClusterBinning().getClustersRenderGraphHandle(), BufferUsageBit::kStorageComputeRead);

	if(getRenderer().getIndirectDiffuseProbes().hasCurrentlyRefreshedVolumeRt())
	{
		pass.newTextureDependency(getRenderer().getIndirectDiffuseProbes().getCurrentlyRefreshedVolumeRt(), TextureUsageBit::kSampledCompute);
	}
}

void VolumetricLightingAccumulation::run(const RenderingContext& ctx, RenderPassWorkContext& rgraphCtx)
{
	CommandBuffer& cmdb = *rgraphCtx.m_commandBuffer;

	cmdb.bindShaderProgram(m_grProg.get());

	// Bind all
	cmdb.bindSampler(0, 0, getRenderer().getSamplers().m_trilinearRepeat.get());
	cmdb.bindSampler(0, 1, getRenderer().getSamplers().m_trilinearClamp.get());
	cmdb.bindSampler(0, 2, getRenderer().getSamplers().m_trilinearClampShadow.get());

	rgraphCtx.bindImage(0, 3, m_runCtx.m_rts[1], TextureSubresourceInfo());

	cmdb.bindTexture(0, 4, &m_noiseImage->getTextureView());

	rgraphCtx.bindColorTexture(0, 5, m_runCtx.m_rts[0]);

	bindUniforms(cmdb, 0, 6, getRenderer().getClusterBinning().getClusteredUniformsRebarToken());
	getRenderer().getPackVisibleClusteredObjects().bindClusteredObjectBuffer(cmdb, 0, 7, ClusteredObjectType::kPointLight);
	getRenderer().getPackVisibleClusteredObjects().bindClusteredObjectBuffer(cmdb, 0, 8, ClusteredObjectType::kSpotLight);
	rgraphCtx.bindColorTexture(0, 9, getRenderer().getShadowMapping().getShadowmapRt());

	getRenderer().getPackVisibleClusteredObjects().bindClusteredObjectBuffer(cmdb, 0, 10, ClusteredObjectType::kGlobalIlluminationProbe);

	getRenderer().getPackVisibleClusteredObjects().bindClusteredObjectBuffer(cmdb, 0, 11, ClusteredObjectType::kFogDensityVolume);
	bindStorage(cmdb, 0, 12, getRenderer().getClusterBinning().getClustersRebarToken());

	cmdb.bindAllBindless(1);

	VolumetricLightingUniforms unis;
	const SkyboxQueueElement& queueEl = ctx.m_renderQueue->m_skybox;
	if(queueEl.m_fog.m_heightOfMaxDensity > queueEl.m_fog.m_heightOfMinDensity)
	{
		unis.m_minHeight = queueEl.m_fog.m_heightOfMinDensity;
		unis.m_oneOverMaxMinusMinHeight = 1.0f / (queueEl.m_fog.m_heightOfMaxDensity - unis.m_minHeight + kEpsilonf);
		unis.m_densityAtMinHeight = queueEl.m_fog.m_minDensity;
		unis.m_densityAtMaxHeight = queueEl.m_fog.m_maxDensity;
	}
	else
	{
		unis.m_minHeight = queueEl.m_fog.m_heightOfMaxDensity;
		unis.m_oneOverMaxMinusMinHeight = 1.0f / (queueEl.m_fog.m_heightOfMinDensity - unis.m_minHeight + kEpsilonf);
		unis.m_densityAtMinHeight = queueEl.m_fog.m_maxDensity;
		unis.m_densityAtMaxHeight = queueEl.m_fog.m_minDensity;
	}
	unis.m_volumeSize = UVec3(m_volumeSize);
	unis.m_maxZSplitsToProcessf = F32(m_finalZSplit + 1);
	cmdb.setPushConstants(&unis, sizeof(unis));

	dispatchPPCompute(cmdb, m_workgroupSize[0], m_workgroupSize[1], m_workgroupSize[2], m_volumeSize[0], m_volumeSize[1], m_volumeSize[2]);
}

} // end namespace anki
