// Copyright (C) 2009-2023, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Renderer/ShadowmapsResolve.h>
#include <AnKi/Renderer/Renderer.h>
#include <AnKi/Renderer/GBuffer.h>
#include <AnKi/Renderer/ShadowMapping.h>
#include <AnKi/Renderer/DepthDownscale.h>
#include <AnKi/Renderer/ClusterBinning.h>
#include <AnKi/Renderer/PackVisibleClusteredObjects.h>
#include <AnKi/Core/ConfigSet.h>

namespace anki {

Error ShadowmapsResolve::init()
{
	const Error err = initInternal();
	if(err)
	{
		ANKI_R_LOGE("Failed to initialize shadow resolve pass");
	}

	return Error::kNone;
}

Error ShadowmapsResolve::initInternal()
{
	m_quarterRez = ConfigSet::getSingleton().getRSmResolveQuarterRez();
	const U32 width = getRenderer().getInternalResolution().x() / (m_quarterRez + 1);
	const U32 height = getRenderer().getInternalResolution().y() / (m_quarterRez + 1);

	ANKI_R_LOGV("Initializing shadowmaps resolve. Resolution %ux%u", width, height);

	m_rtDescr = getRenderer().create2DRenderTargetDescription(width, height, Format::kR8G8B8A8_Unorm, "SM resolve");
	m_rtDescr.bake();

	// Create FB descr
	m_fbDescr.m_colorAttachmentCount = 1;
	m_fbDescr.bake();

	// Prog
	ANKI_CHECK(ResourceManager::getSingleton().loadResource((ConfigSet::getSingleton().getRPreferCompute())
																? "ShaderBinaries/ShadowmapsResolveCompute.ankiprogbin"
																: "ShaderBinaries/ShadowmapsResolveRaster.ankiprogbin",
															m_prog));
	ShaderProgramResourceVariantInitInfo variantInitInfo(m_prog);
	variantInitInfo.addConstant("kFramebufferSize", UVec2(width, height));
	variantInitInfo.addConstant("kTileCount", getRenderer().getTileCounts());
	variantInitInfo.addConstant("kZSplitCount", getRenderer().getZSplitCount());
	variantInitInfo.addConstant("kTileSize", getRenderer().getTileSize());
	variantInitInfo.addMutation("PCF", ConfigSet::getSingleton().getRShadowMappingPcf());
	const ShaderProgramResourceVariant* variant;
	m_prog->getOrCreateVariant(variantInitInfo, variant);
	m_grProg.reset(&variant->getProgram());

	ANKI_CHECK(ResourceManager::getSingleton().loadResource("EngineAssets/BlueNoise_Rgba8_64x64.png", m_noiseImage));

	return Error::kNone;
}

void ShadowmapsResolve::populateRenderGraph(RenderingContext& ctx)
{
	RenderGraphDescription& rgraph = ctx.m_renderGraphDescr;
	m_runCtx.m_rt = rgraph.newRenderTarget(m_rtDescr);

	if(ConfigSet::getSingleton().getRPreferCompute())
	{
		ComputeRenderPassDescription& rpass = rgraph.newComputeRenderPass("ResolveShadows");

		rpass.setWork([this](RenderPassWorkContext& rgraphCtx) {
			run(rgraphCtx);
		});

		rpass.newTextureDependency(m_runCtx.m_rt, TextureUsageBit::kImageComputeWrite);
		rpass.newTextureDependency((m_quarterRez) ? getRenderer().getDepthDownscale().getHiZRt() : getRenderer().getGBuffer().getDepthRt(),
								   TextureUsageBit::kSampledCompute, TextureSurfaceInfo(0, 0, 0, 0));
		rpass.newTextureDependency(getRenderer().getShadowMapping().getShadowmapRt(), TextureUsageBit::kSampledCompute);

		rpass.newBufferDependency(getRenderer().getClusterBinning().getClustersRenderGraphHandle(), BufferUsageBit::kStorageComputeRead);
	}
	else
	{
		GraphicsRenderPassDescription& rpass = rgraph.newGraphicsRenderPass("ResolveShadows");
		rpass.setFramebufferInfo(m_fbDescr, {m_runCtx.m_rt});

		rpass.setWork([this](RenderPassWorkContext& rgraphCtx) {
			run(rgraphCtx);
		});

		rpass.newTextureDependency(m_runCtx.m_rt, TextureUsageBit::kFramebufferWrite);
		rpass.newTextureDependency((m_quarterRez) ? getRenderer().getDepthDownscale().getHiZRt() : getRenderer().getGBuffer().getDepthRt(),
								   TextureUsageBit::kSampledFragment, TextureSurfaceInfo(0, 0, 0, 0));
		rpass.newTextureDependency(getRenderer().getShadowMapping().getShadowmapRt(), TextureUsageBit::kSampledFragment);

		rpass.newBufferDependency(getRenderer().getClusterBinning().getClustersRenderGraphHandle(), BufferUsageBit::kStorageFragmentRead);
	}
}

void ShadowmapsResolve::run(RenderPassWorkContext& rgraphCtx)
{
	CommandBufferPtr& cmdb = rgraphCtx.m_commandBuffer;

	cmdb->bindShaderProgram(m_grProg.get());

	bindUniforms(cmdb, 0, 0, getRenderer().getClusterBinning().getClusteredUniformsRebarToken());
	getRenderer().getPackVisibleClusteredObjects().bindClusteredObjectBuffer(cmdb, 0, 1, ClusteredObjectType::kPointLight);
	getRenderer().getPackVisibleClusteredObjects().bindClusteredObjectBuffer(cmdb, 0, 2, ClusteredObjectType::kSpotLight);
	rgraphCtx.bindColorTexture(0, 3, getRenderer().getShadowMapping().getShadowmapRt());
	bindStorage(cmdb, 0, 4, getRenderer().getClusterBinning().getClustersRebarToken());

	cmdb->bindSampler(0, 5, getRenderer().getSamplers().m_trilinearClamp.get());
	cmdb->bindSampler(0, 6, getRenderer().getSamplers().m_trilinearClampShadow.get());
	cmdb->bindSampler(0, 7, getRenderer().getSamplers().m_trilinearRepeat.get());

	if(m_quarterRez)
	{
		rgraphCtx.bindTexture(0, 8, getRenderer().getDepthDownscale().getHiZRt(), TextureSubresourceInfo(TextureSurfaceInfo(0, 0, 0, 0)));
	}
	else
	{
		rgraphCtx.bindTexture(0, 8, getRenderer().getGBuffer().getDepthRt(), TextureSubresourceInfo(DepthStencilAspectBit::kDepth));
	}
	cmdb->bindTexture(0, 9, &m_noiseImage->getTextureView());

	if(ConfigSet::getSingleton().getRPreferCompute())
	{
		rgraphCtx.bindImage(0, 10, m_runCtx.m_rt, TextureSubresourceInfo());
		dispatchPPCompute(cmdb, 8, 8, m_rtDescr.m_width, m_rtDescr.m_height);
	}
	else
	{
		cmdb->setViewport(0, 0, m_rtDescr.m_width, m_rtDescr.m_height);
		cmdb->draw(PrimitiveTopology::kTriangles, 3);
	}
}

} // end namespace anki
