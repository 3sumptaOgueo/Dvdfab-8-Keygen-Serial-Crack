// Copyright (C) 2009-2023, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Renderer/ForwardShading.h>
#include <AnKi/Renderer/Renderer.h>
#include <AnKi/Renderer/RenderQueue.h>
#include <AnKi/Renderer/GBuffer.h>
#include <AnKi/Renderer/LightShading.h>
#include <AnKi/Renderer/ShadowMapping.h>
#include <AnKi/Renderer/DepthDownscale.h>
#include <AnKi/Renderer/LensFlare.h>
#include <AnKi/Renderer/ClusterBinning2.h>
#include <AnKi/Renderer/LensFlare.h>
#include <AnKi/Renderer/VolumetricLightingAccumulation.h>
#include <AnKi/Shaders/Include/MaterialTypes.h>

namespace anki {

void ForwardShading::run(const RenderingContext& ctx, RenderPassWorkContext& rgraphCtx)
{
	CommandBuffer& cmdb = *rgraphCtx.m_commandBuffer;
	const U32 threadId = rgraphCtx.m_currentSecondLevelCommandBufferIndex;
	const U32 threadCount = rgraphCtx.m_secondLevelCommandBufferCount;
	const U32 problemSize = ctx.m_renderQueue->m_forwardShadingRenderables.getSize();
	U32 start, end;
	splitThreadedProblem(threadId, threadCount, problemSize, start, end);

	if(start != end)
	{
		cmdb.setDepthWrite(false);
		cmdb.setBlendFactors(0, BlendFactor::kSrcAlpha, BlendFactor::kOneMinusSrcAlpha);

		const U32 set = U32(MaterialSet::kGlobal);
		cmdb.bindSampler(set, U32(MaterialBinding::kLinearClampSampler), getRenderer().getSamplers().m_trilinearClamp.get());
		cmdb.bindSampler(set, U32(MaterialBinding::kShadowSampler), getRenderer().getSamplers().m_trilinearClampShadow.get());

		rgraphCtx.bindTexture(set, U32(MaterialBinding::kDepthRt), getRenderer().getDepthDownscale().getHiZRt(), kHiZHalfSurface);
		rgraphCtx.bindColorTexture(set, U32(MaterialBinding::kLightVolume), getRenderer().getVolumetricLightingAccumulation().getRt());

		cmdb.bindUniformBuffer(set, U32(MaterialBinding::kClusterShadingUniforms), getRenderer().getClusterBinning2().getClusteredShadingUniforms());

		cmdb.bindStorageBuffer(set, U32(MaterialBinding::kClusterShadingLights),
							   getRenderer().getClusterBinning2().getPackedObjectsBuffer(GpuSceneNonRenderableObjectType::kLight));

		rgraphCtx.bindColorTexture(set, U32(MaterialBinding::kClusterShadingLights) + 1, getRenderer().getShadowMapping().getShadowmapRt());

		cmdb.bindStorageBuffer(set, U32(MaterialBinding::kClusters), getRenderer().getClusterBinning2().getClustersBuffer());

		RenderableDrawerArguments args;
		args.m_viewMatrix = ctx.m_matrices.m_view;
		args.m_cameraTransform = ctx.m_matrices.m_cameraTransform;
		args.m_viewProjectionMatrix = ctx.m_matrices.m_viewProjectionJitter;
		args.m_previousViewProjectionMatrix = ctx.m_prevMatrices.m_viewProjectionJitter; // Not sure about that
		args.m_sampler = getRenderer().getSamplers().m_trilinearRepeatAnisoResolutionScalingBias.get();

		// Start drawing
		getRenderer().getSceneDrawer().drawRange(args, ctx.m_renderQueue->m_forwardShadingRenderables.getBegin() + start,
												 ctx.m_renderQueue->m_forwardShadingRenderables.getBegin() + end, cmdb);

		// Restore state
		cmdb.setDepthWrite(true);
		cmdb.setBlendFactors(0, BlendFactor::kOne, BlendFactor::kZero);
	}

	if(threadId == threadCount - 1 && ctx.m_renderQueue->m_lensFlares.getSize())
	{
		getRenderer().getLensFlare().runDrawFlares(ctx, cmdb);
	}
}

void ForwardShading::setDependencies(const RenderingContext& ctx, GraphicsRenderPassDescription& pass)
{
	pass.newTextureDependency(getRenderer().getDepthDownscale().getHiZRt(), TextureUsageBit::kSampledFragment, kHiZHalfSurface);
	pass.newTextureDependency(getRenderer().getVolumetricLightingAccumulation().getRt(), TextureUsageBit::kSampledFragment);

	if(ctx.m_renderQueue->m_lensFlares.getSize())
	{
		pass.newBufferDependency(getRenderer().getLensFlare().getIndirectDrawBuffer(), BufferUsageBit::kIndirectDraw);
	}
}

} // end namespace anki
