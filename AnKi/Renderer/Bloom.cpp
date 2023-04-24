// Copyright (C) 2009-2023, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Renderer/Bloom.h>
#include <AnKi/Renderer/DownscaleBlur.h>
#include <AnKi/Renderer/FinalComposite.h>
#include <AnKi/Renderer/Renderer.h>
#include <AnKi/Renderer/Tonemapping.h>
#include <AnKi/Core/ConfigSet.h>

namespace anki {

Bloom::Bloom()
{
	registerDebugRenderTarget("Bloom");
}

Bloom::~Bloom()
{
}

Error Bloom::initInternal()
{
	ANKI_R_LOGV("Initializing bloom");

	ANKI_CHECK(initExposure());
	ANKI_CHECK(initUpscale());
	m_fbDescr.m_colorAttachmentCount = 1;
	m_fbDescr.bake();
	return Error::kNone;
}

Error Bloom::initExposure()
{
	m_exposure.m_width = getRenderer().getDownscaleBlur().getPassWidth(kMaxU32) * 2;
	m_exposure.m_height = getRenderer().getDownscaleBlur().getPassHeight(kMaxU32) * 2;

	// Create RT info
	m_exposure.m_rtDescr = getRenderer().create2DRenderTargetDescription(m_exposure.m_width, m_exposure.m_height, kRtPixelFormat, "Bloom Exp");
	m_exposure.m_rtDescr.bake();

	// init shaders
	CString progFname =
		(ConfigSet::getSingleton().getRPreferCompute()) ? "ShaderBinaries/BloomCompute.ankiprogbin" : "ShaderBinaries/BloomRaster.ankiprogbin";
	ANKI_CHECK(ResourceManager::getSingleton().loadResource(progFname, m_exposure.m_prog));

	ShaderProgramResourceVariantInitInfo variantInitInfo(m_exposure.m_prog);
	if(ConfigSet::getSingleton().getRPreferCompute())
	{
		variantInitInfo.addConstant("kViewport", UVec2(m_exposure.m_width, m_exposure.m_height));
	}

	const ShaderProgramResourceVariant* variant;
	m_exposure.m_prog->getOrCreateVariant(variantInitInfo, variant);
	m_exposure.m_grProg = variant->getProgram();

	return Error::kNone;
}

Error Bloom::initUpscale()
{
	m_upscale.m_width = getRenderer().getPostProcessResolution().x() / kBloomFraction;
	m_upscale.m_height = getRenderer().getPostProcessResolution().y() / kBloomFraction;

	// Create RT descr
	m_upscale.m_rtDescr = getRenderer().create2DRenderTargetDescription(m_upscale.m_width, m_upscale.m_height, kRtPixelFormat, "Bloom Upscale");
	m_upscale.m_rtDescr.bake();

	// init shaders
	CString progFname = (ConfigSet::getSingleton().getRPreferCompute()) ? "ShaderBinaries/BloomUpscaleCompute.ankiprogbin"
																		: "ShaderBinaries/BloomUpscaleRaster.ankiprogbin";
	ANKI_CHECK(ResourceManager::getSingleton().loadResource(progFname, m_upscale.m_prog));

	ShaderProgramResourceVariantInitInfo variantInitInfo(m_upscale.m_prog);
	variantInitInfo.addConstant("kInputTextureSize", UVec2(m_exposure.m_width, m_exposure.m_height));
	if(ConfigSet::getSingleton().getRPreferCompute())
	{
		variantInitInfo.addConstant("kViewport", UVec2(m_upscale.m_width, m_upscale.m_height));
	}

	const ShaderProgramResourceVariant* variant;
	m_upscale.m_prog->getOrCreateVariant(variantInitInfo, variant);
	m_upscale.m_grProg = variant->getProgram();

	// Textures
	ANKI_CHECK(ResourceManager::getSingleton().loadResource("EngineAssets/LensDirt.ankitex", m_upscale.m_lensDirtImage));

	return Error::kNone;
}

void Bloom::populateRenderGraph(RenderingContext& ctx)
{
	RenderGraphDescription& rgraph = ctx.m_renderGraphDescr;
	const Bool preferCompute = ConfigSet::getSingleton().getRPreferCompute();

	// Main pass
	{
		// Ask for render target
		m_runCtx.m_exposureRt = rgraph.newRenderTarget(m_exposure.m_rtDescr);

		// Set the render pass
		TextureSubresourceInfo inputTexSubresource;
		inputTexSubresource.m_firstMipmap = getRenderer().getDownscaleBlur().getMipmapCount() - 1;

		RenderPassDescriptionBase* prpass;
		if(preferCompute)
		{
			ComputeRenderPassDescription& rpass = rgraph.newComputeRenderPass("Bloom Main");

			rpass.newTextureDependency(getRenderer().getDownscaleBlur().getRt(), TextureUsageBit::kSampledCompute, inputTexSubresource);
			rpass.newTextureDependency(m_runCtx.m_exposureRt, TextureUsageBit::kImageComputeWrite);

			prpass = &rpass;
		}
		else
		{
			GraphicsRenderPassDescription& rpass = rgraph.newGraphicsRenderPass("Bloom Main");
			rpass.setFramebufferInfo(m_fbDescr, {m_runCtx.m_exposureRt});

			rpass.newTextureDependency(getRenderer().getDownscaleBlur().getRt(), TextureUsageBit::kSampledFragment, inputTexSubresource);
			rpass.newTextureDependency(m_runCtx.m_exposureRt, TextureUsageBit::kFramebufferWrite);

			prpass = &rpass;
		}

		prpass->setWork([this](RenderPassWorkContext& rgraphCtx) {
			CommandBufferPtr& cmdb = rgraphCtx.m_commandBuffer;

			cmdb->bindShaderProgram(m_exposure.m_grProg);

			TextureSubresourceInfo inputTexSubresource;
			inputTexSubresource.m_firstMipmap = getRenderer().getDownscaleBlur().getMipmapCount() - 1;

			cmdb->bindSampler(0, 0, getRenderer().getSamplers().m_trilinearClamp);
			rgraphCtx.bindTexture(0, 1, getRenderer().getDownscaleBlur().getRt(), inputTexSubresource);

			const Vec4 uniforms(ConfigSet::getSingleton().getRBloomThreshold(), ConfigSet::getSingleton().getRBloomScale(), 0.0f, 0.0f);
			cmdb->setPushConstants(&uniforms, sizeof(uniforms));

			rgraphCtx.bindImage(0, 2, getRenderer().getTonemapping().getRt());

			if(ConfigSet::getSingleton().getRPreferCompute())
			{
				rgraphCtx.bindImage(0, 3, m_runCtx.m_exposureRt, TextureSubresourceInfo());

				dispatchPPCompute(cmdb, 8, 8, m_exposure.m_width, m_exposure.m_height);
			}
			else
			{
				cmdb->setViewport(0, 0, m_exposure.m_width, m_exposure.m_height);

				cmdb->drawArrays(PrimitiveTopology::kTriangles, 3);
			}
		});
	}

	// Upscale & SSLF pass
	{
		// Ask for render target
		m_runCtx.m_upscaleRt = rgraph.newRenderTarget(m_upscale.m_rtDescr);

		// Set the render pass
		RenderPassDescriptionBase* prpass;
		if(preferCompute)
		{
			ComputeRenderPassDescription& rpass = rgraph.newComputeRenderPass("Bloom Upscale");

			rpass.newTextureDependency(m_runCtx.m_exposureRt, TextureUsageBit::kSampledCompute);
			rpass.newTextureDependency(m_runCtx.m_upscaleRt, TextureUsageBit::kImageComputeWrite);

			prpass = &rpass;
		}
		else
		{
			GraphicsRenderPassDescription& rpass = rgraph.newGraphicsRenderPass("Bloom Upscale");
			rpass.setFramebufferInfo(m_fbDescr, {m_runCtx.m_upscaleRt});

			rpass.newTextureDependency(m_runCtx.m_exposureRt, TextureUsageBit::kSampledFragment);
			rpass.newTextureDependency(m_runCtx.m_upscaleRt, TextureUsageBit::kFramebufferWrite);

			prpass = &rpass;
		}

		prpass->setWork([this](RenderPassWorkContext& rgraphCtx) {
			CommandBufferPtr& cmdb = rgraphCtx.m_commandBuffer;

			cmdb->bindShaderProgram(m_upscale.m_grProg);

			cmdb->bindSampler(0, 0, getRenderer().getSamplers().m_trilinearClamp);
			rgraphCtx.bindColorTexture(0, 1, m_runCtx.m_exposureRt);
			cmdb->bindTexture(0, 2, m_upscale.m_lensDirtImage->getTextureView());

			if(ConfigSet::getSingleton().getRPreferCompute())
			{
				rgraphCtx.bindImage(0, 3, m_runCtx.m_upscaleRt, TextureSubresourceInfo());

				dispatchPPCompute(cmdb, 8, 8, m_upscale.m_width, m_upscale.m_height);
			}
			else
			{
				cmdb->setViewport(0, 0, m_upscale.m_width, m_upscale.m_height);

				cmdb->drawArrays(PrimitiveTopology::kTriangles, 3);
			}
		});
	}
}

void Bloom::getDebugRenderTarget([[maybe_unused]] CString rtName, Array<RenderTargetHandle, kMaxDebugRenderTargets>& handles,
								 [[maybe_unused]] ShaderProgramPtr& optionalShaderProgram) const
{
	ANKI_ASSERT(rtName == "Bloom");
	handles[0] = m_runCtx.m_upscaleRt;
}

} // end namespace anki
