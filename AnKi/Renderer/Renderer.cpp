// Copyright (C) 2009-2023, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Renderer/Renderer.h>
#include <AnKi/Util/Tracer.h>
#include <AnKi/Util/ThreadHive.h>
#include <AnKi/Core/CVarSet.h>
#include <AnKi/Util/HighRezTimer.h>
#include <AnKi/Collision/Aabb.h>
#include <AnKi/Collision/Plane.h>
#include <AnKi/Collision/Functions.h>
#include <AnKi/Shaders/Include/ClusteredShadingTypes.h>
#include <AnKi/Core/GpuMemory/GpuSceneBuffer.h>
#include <AnKi/Scene/Components/CameraComponent.h>
#include <AnKi/Scene/Components/LightComponent.h>

#include <AnKi/Renderer/ProbeReflections.h>
#include <AnKi/Renderer/GBuffer.h>
#include <AnKi/Renderer/GBufferPost.h>
#include <AnKi/Renderer/LightShading.h>
#include <AnKi/Renderer/ShadowMapping.h>
#include <AnKi/Renderer/FinalComposite.h>
#include <AnKi/Renderer/Bloom.h>
#include <AnKi/Renderer/Tonemapping.h>
#include <AnKi/Renderer/ForwardShading.h>
#include <AnKi/Renderer/LensFlare.h>
#include <AnKi/Renderer/Dbg.h>
#include <AnKi/Renderer/DownscaleBlur.h>
#include <AnKi/Renderer/VolumetricFog.h>
#include <AnKi/Renderer/DepthDownscale.h>
#include <AnKi/Renderer/TemporalAA.h>
#include <AnKi/Renderer/UiStage.h>
#include <AnKi/Renderer/IndirectSpecular.h>
#include <AnKi/Renderer/VolumetricLightingAccumulation.h>
#include <AnKi/Renderer/IndirectDiffuseProbes.h>
#include <AnKi/Renderer/ShadowmapsResolve.h>
#include <AnKi/Renderer/RtShadows.h>
#include <AnKi/Renderer/AccelerationStructureBuilder.h>
#include <AnKi/Renderer/MotionVectors.h>
#include <AnKi/Renderer/Scale.h>
#include <AnKi/Renderer/IndirectDiffuse.h>
#include <AnKi/Renderer/VrsSriGeneration.h>
#include <AnKi/Renderer/PrimaryNonRenderableVisibility.h>
#include <AnKi/Renderer/ClusterBinning.h>

namespace anki {

static NumericCVar<F32> g_internalRenderScalingCVar(CVarSubsystem::kRenderer, "InternalRenderScaling", 1.0f, 0.5f, 1.0f,
													"A factor over the requested swapchain resolution. Applies to all passes up to TAA");
NumericCVar<F32> g_renderScalingCVar(CVarSubsystem::kRenderer, "RenderScaling", 1.0f, 0.5f, 8.0f,
									 "A factor over the requested swapchain resolution. Applies to post-processing and UI");
static NumericCVar<U32> g_zSplitCountCVar(CVarSubsystem::kRenderer, "ZSplitCount", 64, 8, kMaxZsplitCount, "Clusterer number of Z splits");
static NumericCVar<U8> g_textureAnisotropyCVar(CVarSubsystem::kRenderer, "TextureAnisotropy", (ANKI_PLATFORM_MOBILE) ? 1 : 8, 1, 16,
											   "Texture anisotropy for the main passes");
BoolCVar g_preferComputeCVar(CVarSubsystem::kRenderer, "PreferCompute", !ANKI_PLATFORM_MOBILE, "Prefer compute shaders");
static BoolCVar g_highQualityHdrCVar(CVarSubsystem::kRenderer, "HighQualityHdr", !ANKI_PLATFORM_MOBILE,
									 "If true use R16G16B16 for HDR images. Alternatively use B10G11R11");
BoolCVar g_vrsLimitTo2x2CVar(CVarSubsystem::kRenderer, "VrsLimitTo2x2", false, "If true the max rate will be 2x2");
BoolCVar g_vrsCVar(CVarSubsystem::kRenderer, "Vrs", true, "Enable VRS in multiple passes");
BoolCVar g_rayTracedShadowsCVar(CVarSubsystem::kRenderer, "RayTracedShadows", true,
								"Enable or not ray traced shadows. Ignored if RT is not supported");
NumericCVar<U8> g_shadowCascadeCountCVar(CVarSubsystem::kRenderer, "ShadowCascadeCount", (ANKI_PLATFORM_MOBILE) ? 3 : kMaxShadowCascades, 1,
										 kMaxShadowCascades, "Max number of shadow cascades for directional lights");
NumericCVar<F32> g_shadowCascade0DistanceCVar(CVarSubsystem::kRenderer, "ShadowCascade0Distance", 18.0, 1.0, kMaxF32,
											  "The distance of the 1st cascade");
NumericCVar<F32> g_shadowCascade1DistanceCVar(CVarSubsystem::kRenderer, "ShadowCascade1Distance", (ANKI_PLATFORM_MOBILE) ? 80.0f : 40.0, 1.0, kMaxF32,
											  "The distance of the 2nd cascade");
NumericCVar<F32> g_shadowCascade2DistanceCVar(CVarSubsystem::kRenderer, "ShadowCascade2Distance", (ANKI_PLATFORM_MOBILE) ? 150.0f : 80.0, 1.0,
											  kMaxF32, "The distance of the 3rd cascade");
NumericCVar<F32> g_shadowCascade3DistanceCVar(CVarSubsystem::kRenderer, "ShadowCascade3Distance", 200.0, 1.0, kMaxF32,
											  "The distance of the 4th cascade");
NumericCVar<F32> g_lod0MaxDistanceCVar(CVarSubsystem::kRenderer, "Lod0MaxDistance", 20.0f, 1.0f, kMaxF32,
									   "Distance that will be used to calculate the LOD 0");
NumericCVar<F32> g_lod1MaxDistanceCVar(CVarSubsystem::kRenderer, "Lod1MaxDistance", 40.0f, 2.0f, kMaxF32,
									   "Distance that will be used to calculate the LOD 1");

/// Generate a Halton jitter in [-0.5, 0.5]
static Vec2 generateJitter(U32 frame)
{
	// Halton jitter
	Vec2 result(0.0f);

	constexpr U32 baseX = 2;
	U32 index = frame + 1;
	F32 invBase = 1.0f / baseX;
	F32 fraction = invBase;
	while(index > 0)
	{
		result.x() += F32(index % baseX) * fraction;
		index /= baseX;
		fraction *= invBase;
	}

	constexpr U32 baseY = 3;
	index = frame + 1;
	invBase = 1.0f / baseY;
	fraction = invBase;
	while(index > 0)
	{
		result.y() += F32(index % baseY) * fraction;
		index /= baseY;
		fraction *= invBase;
	}

	result.x() -= 0.5f;
	result.y() -= 0.5f;
	return result;
}

Renderer::Renderer()
{
}

Renderer::~Renderer()
{
}

Error Renderer::init(UVec2 swapchainSize, StackMemoryPool* framePool)
{
	ANKI_TRACE_SCOPED_EVENT(RInit);

	m_framePool = framePool;
	const Error err = initInternal(swapchainSize);
	if(err)
	{
		ANKI_R_LOGE("Failed to initialize the renderer");
	}

	return err;
}

Error Renderer::initInternal(UVec2 swapchainResolution)
{
	m_frameCount = 0;

	// Set from the config
	m_postProcessResolution = UVec2(Vec2(swapchainResolution) * g_renderScalingCVar.get());
	alignRoundDown(2, m_postProcessResolution.x());
	alignRoundDown(2, m_postProcessResolution.y());

	m_internalResolution = UVec2(Vec2(m_postProcessResolution) * g_internalRenderScalingCVar.get());
	alignRoundDown(2, m_internalResolution.x());
	alignRoundDown(2, m_internalResolution.y());

	ANKI_R_LOGI("Initializing offscreen renderer. Resolution %ux%u. Internal resolution %ux%u", m_postProcessResolution.x(),
				m_postProcessResolution.y(), m_internalResolution.x(), m_internalResolution.y());

	m_tileCounts.x() = (m_internalResolution.x() + kClusteredShadingTileSize - 1) / kClusteredShadingTileSize;
	m_tileCounts.y() = (m_internalResolution.y() + kClusteredShadingTileSize - 1) / kClusteredShadingTileSize;
	m_zSplitCount = g_zSplitCountCVar.get();

	// A few sanity checks
	if(m_internalResolution.x() < 64 || m_internalResolution.y() < 64)
	{
		ANKI_R_LOGE("Incorrect sizes");
		return Error::kUserData;
	}

	ANKI_CHECK(ResourceManager::getSingleton().loadResource("ShaderBinaries/ClearTextureCompute.ankiprogbin", m_clearTexComputeProg));

	// Dummy resources
	{
		TextureInitInfo texinit("RendererDummy");
		texinit.m_width = texinit.m_height = 4;
		texinit.m_usage = TextureUsageBit::kAllSampled | TextureUsageBit::kImageComputeWrite;
		texinit.m_format = Format::kR8G8B8A8_Unorm;
		TexturePtr tex = createAndClearRenderTarget(texinit, TextureUsageBit::kAllSampled);

		TextureViewInitInfo viewinit(tex.get());
		m_dummyTexView2d = GrManager::getSingleton().newTextureView(viewinit);

		texinit.m_depth = 4;
		texinit.m_type = TextureType::k3D;
		tex = createAndClearRenderTarget(texinit, TextureUsageBit::kAllSampled);
		viewinit = TextureViewInitInfo(tex.get());
		m_dummyTexView3d = GrManager::getSingleton().newTextureView(viewinit);

		m_dummyBuff = GrManager::getSingleton().newBuffer(
			BufferInitInfo(1024, BufferUsageBit::kAllUniform | BufferUsageBit::kAllStorage, BufferMapAccessBit::kNone, "Dummy"));
	}

	// Init the stages. Careful with the order!!!!!!!!!!
	m_volumetricLightingAccumulation.reset(newInstance<VolumetricLightingAccumulation>(RendererMemoryPool::getSingleton()));
	ANKI_CHECK(m_volumetricLightingAccumulation->init());

	m_indirectDiffuseProbes.reset(newInstance<IndirectDiffuseProbes>(RendererMemoryPool::getSingleton()));
	ANKI_CHECK(m_indirectDiffuseProbes->init());

	m_probeReflections.reset(newInstance<ProbeReflections>(RendererMemoryPool::getSingleton()));
	ANKI_CHECK(m_probeReflections->init());

	m_vrsSriGeneration.reset(newInstance<VrsSriGeneration>(RendererMemoryPool::getSingleton()));
	ANKI_CHECK(m_vrsSriGeneration->init());

	m_scale.reset(newInstance<Scale>(RendererMemoryPool::getSingleton()));
	ANKI_CHECK(m_scale->init());

	m_gbuffer.reset(newInstance<GBuffer>(RendererMemoryPool::getSingleton()));
	ANKI_CHECK(m_gbuffer->init());

	m_gbufferPost.reset(newInstance<GBufferPost>(RendererMemoryPool::getSingleton()));
	ANKI_CHECK(m_gbufferPost->init());

	m_shadowMapping.reset(newInstance<ShadowMapping>(RendererMemoryPool::getSingleton()));
	ANKI_CHECK(m_shadowMapping->init());

	m_volumetricFog.reset(newInstance<VolumetricFog>(RendererMemoryPool::getSingleton()));
	ANKI_CHECK(m_volumetricFog->init());

	m_lightShading.reset(newInstance<LightShading>(RendererMemoryPool::getSingleton()));
	ANKI_CHECK(m_lightShading->init());

	m_depthDownscale.reset(newInstance<DepthDownscale>(RendererMemoryPool::getSingleton()));
	ANKI_CHECK(m_depthDownscale->init());

	m_forwardShading.reset(newInstance<ForwardShading>(RendererMemoryPool::getSingleton()));
	ANKI_CHECK(m_forwardShading->init());

	m_lensFlare.reset(newInstance<LensFlare>(RendererMemoryPool::getSingleton()));
	ANKI_CHECK(m_lensFlare->init());

	m_downscaleBlur.reset(newInstance<DownscaleBlur>(RendererMemoryPool::getSingleton()));
	ANKI_CHECK(m_downscaleBlur->init());

	m_indirectSpecular.reset(newInstance<IndirectSpecular>(RendererMemoryPool::getSingleton()));
	ANKI_CHECK(m_indirectSpecular->init());

	m_tonemapping.reset(newInstance<Tonemapping>(RendererMemoryPool::getSingleton()));
	ANKI_CHECK(m_tonemapping->init());

	m_temporalAA.reset(newInstance<TemporalAA>(RendererMemoryPool::getSingleton()));
	ANKI_CHECK(m_temporalAA->init());

	m_bloom.reset(newInstance<Bloom>(RendererMemoryPool::getSingleton()));
	ANKI_CHECK(m_bloom->init());

	m_finalComposite.reset(newInstance<FinalComposite>(RendererMemoryPool::getSingleton()));
	ANKI_CHECK(m_finalComposite->init());

	m_dbg.reset(newInstance<Dbg>(RendererMemoryPool::getSingleton()));
	ANKI_CHECK(m_dbg->init());

	m_uiStage.reset(newInstance<UiStage>(RendererMemoryPool::getSingleton()));
	ANKI_CHECK(m_uiStage->init());

	m_indirectDiffuse.reset(newInstance<IndirectDiffuse>(RendererMemoryPool::getSingleton()));
	ANKI_CHECK(m_indirectDiffuse->init());

	if(GrManager::getSingleton().getDeviceCapabilities().m_rayTracingEnabled && g_rayTracedShadowsCVar.get())
	{
		m_accelerationStructureBuilder.reset(newInstance<AccelerationStructureBuilder>(RendererMemoryPool::getSingleton()));
		ANKI_CHECK(m_accelerationStructureBuilder->init());

		m_rtShadows.reset(newInstance<RtShadows>(RendererMemoryPool::getSingleton()));
		ANKI_CHECK(m_rtShadows->init());
	}

	m_shadowmapsResolve.reset(newInstance<ShadowmapsResolve>(RendererMemoryPool::getSingleton()));
	ANKI_CHECK(m_shadowmapsResolve->init());

	m_motionVectors.reset(newInstance<MotionVectors>(RendererMemoryPool::getSingleton()));
	ANKI_CHECK(m_motionVectors->init());

	m_clusterBinning2.reset(newInstance<ClusterBinning>(RendererMemoryPool::getSingleton()));
	ANKI_CHECK(m_clusterBinning2->init());

	m_primaryNonRenderableVisibility.reset(newInstance<PrimaryNonRenderableVisibility>(RendererMemoryPool::getSingleton()));
	ANKI_CHECK(m_primaryNonRenderableVisibility->init());

	// Init samplers
	{
		SamplerInitInfo sinit("NearestNearestClamp");
		sinit.m_addressing = SamplingAddressing::kClamp;
		sinit.m_mipmapFilter = SamplingFilter::kNearest;
		sinit.m_minMagFilter = SamplingFilter::kNearest;
		m_samplers.m_nearestNearestClamp = GrManager::getSingleton().newSampler(sinit);

		sinit.setName("TrilinearClamp");
		sinit.m_minMagFilter = SamplingFilter::kLinear;
		sinit.m_mipmapFilter = SamplingFilter::kLinear;
		m_samplers.m_trilinearClamp = GrManager::getSingleton().newSampler(sinit);

		sinit.setName("TrilinearRepeat");
		sinit.m_addressing = SamplingAddressing::kRepeat;
		m_samplers.m_trilinearRepeat = GrManager::getSingleton().newSampler(sinit);

		sinit.setName("TrilinearRepeatAniso");
		sinit.m_anisotropyLevel = g_textureAnisotropyCVar.get();
		m_samplers.m_trilinearRepeatAniso = GrManager::getSingleton().newSampler(sinit);

		sinit.setName("TrilinearRepeatAnisoRezScalingBias");
		F32 scalingMipBias = log2(F32(m_internalResolution.x()) / F32(m_postProcessResolution.x()));
		if(getScale().getUsingGrUpscaler())
		{
			// DLSS wants more bias
			scalingMipBias -= 1.0f;
		}

		sinit.m_lodBias = scalingMipBias;
		m_samplers.m_trilinearRepeatAnisoResolutionScalingBias = GrManager::getSingleton().newSampler(sinit);

		sinit = {};
		sinit.setName("TrilinearClampShadow");
		sinit.m_minMagFilter = SamplingFilter::kLinear;
		sinit.m_mipmapFilter = SamplingFilter::kLinear;
		sinit.m_compareOperation = CompareOperation::kLessEqual;
		m_samplers.m_trilinearClampShadow = GrManager::getSingleton().newSampler(sinit);
	}

	for(U32 i = 0; i < m_jitterOffsets.getSize(); ++i)
	{
		m_jitterOffsets[i] = generateJitter(i);
	}

	ANKI_CHECK(m_visibility.init());
	ANKI_CHECK(m_nonRenderablesVisibility.init());
	ANKI_CHECK(m_asVisibility.init());
	ANKI_CHECK(m_hzbGenerator.init());
	ANKI_CHECK(m_sceneDrawer.init());

	return Error::kNone;
}

Error Renderer::populateRenderGraph(RenderingContext& ctx)
{
	const CameraComponent& cam = SceneGraph::getSingleton().getActiveCameraNode().getFirstComponentOfType<CameraComponent>();

	ctx.m_prevMatrices = m_prevMatrices;

	ctx.m_matrices.m_cameraTransform = Mat3x4(cam.getFrustum().getWorldTransform());
	ctx.m_matrices.m_view = cam.getFrustum().getViewMatrix();
	ctx.m_matrices.m_projection = cam.getFrustum().getProjectionMatrix();
	ctx.m_matrices.m_viewProjection = cam.getFrustum().getViewProjectionMatrix();

	Vec2 jitter = m_jitterOffsets[m_frameCount & (m_jitterOffsets.getSize() - 1)]; // In [-0.5, 0.5]
	const Vec2 ndcPixelSize = 2.0f / Vec2(m_internalResolution);
	jitter *= ndcPixelSize;
	ctx.m_matrices.m_jitter = Mat4::getIdentity();
	ctx.m_matrices.m_jitter.setTranslationPart(Vec4(jitter, 0.0f, 1.0f));

	ctx.m_matrices.m_projectionJitter = ctx.m_matrices.m_jitter * ctx.m_matrices.m_projection;
	ctx.m_matrices.m_viewProjectionJitter = ctx.m_matrices.m_projectionJitter * Mat4(ctx.m_matrices.m_view, Vec4(0.0f, 0.0f, 0.0f, 1.0f));
	ctx.m_matrices.m_invertedViewProjectionJitter = ctx.m_matrices.m_viewProjectionJitter.getInverse();
	ctx.m_matrices.m_invertedViewProjection = ctx.m_matrices.m_viewProjection.getInverse();
	ctx.m_matrices.m_invertedProjectionJitter = ctx.m_matrices.m_projectionJitter.getInverse();

	ctx.m_matrices.m_reprojection = ctx.m_matrices.m_jitter * ctx.m_prevMatrices.m_viewProjection * ctx.m_matrices.m_invertedViewProjectionJitter;

	ctx.m_matrices.m_unprojectionParameters = ctx.m_matrices.m_projection.extractPerspectiveUnprojectionParams();

	ctx.m_cameraNear = cam.getNear();
	ctx.m_cameraFar = cam.getFar();

	// Import RTs first
	m_downscaleBlur->importRenderTargets(ctx);
	m_tonemapping->importRenderTargets(ctx);
	m_vrsSriGeneration->importRenderTargets(ctx);
	m_gbuffer->importRenderTargets(ctx);

	// Populate render graph. WARNING Watch the order
	gpuSceneCopy(ctx);
	m_primaryNonRenderableVisibility->populateRenderGraph(ctx);
	if(m_accelerationStructureBuilder)
	{
		m_accelerationStructureBuilder->populateRenderGraph(ctx);
	}
	m_forwardShading->populateRenderGraph(ctx); // This may feel out of place but it's only visibility
	m_gbuffer->populateRenderGraph(ctx);
	m_shadowMapping->populateRenderGraph(ctx);
	m_clusterBinning2->populateRenderGraph(ctx);
	m_indirectDiffuseProbes->populateRenderGraph(ctx);
	m_probeReflections->populateRenderGraph(ctx);
	m_volumetricLightingAccumulation->populateRenderGraph(ctx);
	m_motionVectors->populateRenderGraph(ctx);
	m_gbufferPost->populateRenderGraph(ctx);
	m_depthDownscale->populateRenderGraph(ctx);
	if(m_rtShadows)
	{
		m_rtShadows->populateRenderGraph(ctx);
	}
	m_shadowmapsResolve->populateRenderGraph(ctx);
	m_volumetricFog->populateRenderGraph(ctx);
	m_lensFlare->populateRenderGraph(ctx);
	m_indirectSpecular->populateRenderGraph(ctx);
	m_indirectDiffuse->populateRenderGraph(ctx);
	m_lightShading->populateRenderGraph(ctx);
	if(!getScale().getUsingGrUpscaler())
	{
		m_temporalAA->populateRenderGraph(ctx);
	}
	m_vrsSriGeneration->populateRenderGraph(ctx);
	m_scale->populateRenderGraph(ctx);
	m_downscaleBlur->populateRenderGraph(ctx);
	m_tonemapping->populateRenderGraph(ctx);
	m_bloom->populateRenderGraph(ctx);
	m_dbg->populateRenderGraph(ctx);

	m_finalComposite->populateRenderGraph(ctx);

	return Error::kNone;
}

void Renderer::finalize(const RenderingContext& ctx, Fence* fence)
{
	++m_frameCount;

	m_prevMatrices = ctx.m_matrices;
	m_readbaks.endFrame(fence);
}

TextureInitInfo Renderer::create2DRenderTargetInitInfo(U32 w, U32 h, Format format, TextureUsageBit usage, CString name)
{
	ANKI_ASSERT(!!(usage & TextureUsageBit::kFramebufferWrite) || !!(usage & TextureUsageBit::kImageComputeWrite));
	TextureInitInfo init(name);

	init.m_width = w;
	init.m_height = h;
	init.m_depth = 1;
	init.m_layerCount = 1;
	init.m_type = TextureType::k2D;
	init.m_format = format;
	init.m_mipmapCount = 1;
	init.m_samples = 1;
	init.m_usage = usage;

	return init;
}

RenderTargetDescription Renderer::create2DRenderTargetDescription(U32 w, U32 h, Format format, CString name)
{
	RenderTargetDescription init(name);

	init.m_width = w;
	init.m_height = h;
	init.m_depth = 1;
	init.m_layerCount = 1;
	init.m_type = TextureType::k2D;
	init.m_format = format;
	init.m_mipmapCount = 1;
	init.m_samples = 1;
	init.m_usage = TextureUsageBit::kNone;

	return init;
}

TexturePtr Renderer::createAndClearRenderTarget(const TextureInitInfo& inf, TextureUsageBit initialUsage, const ClearValue& clearVal)
{
	ANKI_ASSERT(!!(inf.m_usage & TextureUsageBit::kFramebufferWrite) || !!(inf.m_usage & TextureUsageBit::kImageComputeWrite));

	const U faceCount = (inf.m_type == TextureType::kCube || inf.m_type == TextureType::kCubeArray) ? 6 : 1;

	Bool useCompute = false;
	if(!!(inf.m_usage & TextureUsageBit::kFramebufferWrite))
	{
		useCompute = false;
	}
	else if(!!(inf.m_usage & TextureUsageBit::kImageComputeWrite))
	{
		useCompute = true;
	}
	else
	{
		ANKI_ASSERT(!"Can't handle that");
	}

	// Create tex
	TexturePtr tex = GrManager::getSingleton().newTexture(inf);

	// Clear all surfaces
	CommandBufferInitInfo cmdbinit;
	cmdbinit.m_flags = CommandBufferFlag::kGeneralWork;
	if((inf.m_mipmapCount * faceCount * inf.m_layerCount * 4) < kCommandBufferSmallBatchMaxCommands)
	{
		cmdbinit.m_flags |= CommandBufferFlag::kSmallBatch;
	}
	CommandBufferPtr cmdb = GrManager::getSingleton().newCommandBuffer(cmdbinit);

	for(U32 mip = 0; mip < inf.m_mipmapCount; ++mip)
	{
		for(U32 face = 0; face < faceCount; ++face)
		{
			for(U32 layer = 0; layer < inf.m_layerCount; ++layer)
			{
				TextureSurfaceInfo surf(mip, 0, face, layer);

				if(!useCompute)
				{
					FramebufferInitInfo fbInit("RendererClearRT");
					Array<TextureUsageBit, kMaxColorRenderTargets> colUsage = {};
					TextureUsageBit dsUsage = TextureUsageBit::kNone;

					if(getFormatInfo(inf.m_format).isDepthStencil())
					{
						DepthStencilAspectBit aspect = DepthStencilAspectBit::kNone;
						if(getFormatInfo(inf.m_format).isDepth())
						{
							aspect |= DepthStencilAspectBit::kDepth;
						}

						if(getFormatInfo(inf.m_format).isStencil())
						{
							aspect |= DepthStencilAspectBit::kStencil;
						}

						TextureViewPtr view = GrManager::getSingleton().newTextureView(TextureViewInitInfo(tex.get(), surf, aspect));

						fbInit.m_depthStencilAttachment.m_textureView = std::move(view);
						fbInit.m_depthStencilAttachment.m_loadOperation = AttachmentLoadOperation::kClear;
						fbInit.m_depthStencilAttachment.m_stencilLoadOperation = AttachmentLoadOperation::kClear;
						fbInit.m_depthStencilAttachment.m_clearValue = clearVal;

						dsUsage = TextureUsageBit::kFramebufferWrite;
					}
					else
					{
						TextureViewPtr view = GrManager::getSingleton().newTextureView(TextureViewInitInfo(tex.get(), surf));

						fbInit.m_colorAttachmentCount = 1;
						fbInit.m_colorAttachments[0].m_textureView = view;
						fbInit.m_colorAttachments[0].m_loadOperation = AttachmentLoadOperation::kClear;
						fbInit.m_colorAttachments[0].m_clearValue = clearVal;

						colUsage[0] = TextureUsageBit::kFramebufferWrite;
					}
					FramebufferPtr fb = GrManager::getSingleton().newFramebuffer(fbInit);

					TextureBarrierInfo barrier = {tex.get(), TextureUsageBit::kNone, TextureUsageBit::kFramebufferWrite, surf};
					barrier.m_subresource.m_depthStencilAspect = tex->getDepthStencilAspect();
					cmdb->setPipelineBarrier({&barrier, 1}, {}, {});

					cmdb->beginRenderPass(fb.get(), colUsage, dsUsage);
					cmdb->endRenderPass();

					if(!!initialUsage)
					{
						barrier.m_previousUsage = TextureUsageBit::kFramebufferWrite;
						barrier.m_nextUsage = initialUsage;
						cmdb->setPipelineBarrier({&barrier, 1}, {}, {});
					}
				}
				else
				{
					// Compute
					ShaderProgramResourceVariantInitInfo variantInitInfo(m_clearTexComputeProg);
					variantInitInfo.addMutation("TEXTURE_DIMENSIONS", I32((inf.m_type == TextureType::k3D) ? 3 : 2));

					const FormatInfo formatInfo = getFormatInfo(inf.m_format);
					I32 componentType = 0;
					if(formatInfo.m_shaderType == 0)
					{
						componentType = 0;
					}
					else if(formatInfo.m_shaderType == 1)
					{
						componentType = 1;
					}
					else
					{
						ANKI_ASSERT(!"Not supported");
					}
					variantInitInfo.addMutation("COMPONENT_TYPE", componentType);

					const ShaderProgramResourceVariant* variant;
					m_clearTexComputeProg->getOrCreateVariant(variantInitInfo, variant);

					cmdb->bindShaderProgram(&variant->getProgram());

					cmdb->setPushConstants(&clearVal.m_colorf[0], sizeof(clearVal.m_colorf));

					TextureViewPtr view = GrManager::getSingleton().newTextureView(TextureViewInitInfo(tex.get(), surf));
					cmdb->bindImage(0, 0, view.get());

					const TextureBarrierInfo barrier = {tex.get(), TextureUsageBit::kNone, TextureUsageBit::kImageComputeWrite, surf};
					cmdb->setPipelineBarrier({&barrier, 1}, {}, {});

					UVec3 wgSize;
					wgSize.x() = (8 - 1 + (tex->getWidth() >> mip)) / 8;
					wgSize.y() = (8 - 1 + (tex->getHeight() >> mip)) / 8;
					wgSize.z() = (inf.m_type == TextureType::k3D) ? ((8 - 1 + (tex->getDepth() >> mip)) / 8) : 1;

					cmdb->dispatchCompute(wgSize.x(), wgSize.y(), wgSize.z());

					if(!!initialUsage)
					{
						const TextureBarrierInfo barrier = {tex.get(), TextureUsageBit::kImageComputeWrite, initialUsage, surf};

						cmdb->setPipelineBarrier({&barrier, 1}, {}, {});
					}
				}
			}
		}
	}

	cmdb->flush();

	return tex;
}

void Renderer::registerDebugRenderTarget(RendererObject* obj, CString rtName)
{
#if ANKI_ASSERTIONS_ENABLED
	for(const DebugRtInfo& inf : m_debugRts)
	{
		ANKI_ASSERT(inf.m_rtName != rtName && "Choose different name");
	}
#endif

	ANKI_ASSERT(obj);
	DebugRtInfo inf;
	inf.m_obj = obj;
	inf.m_rtName = rtName;

	m_debugRts.emplaceBack(std::move(inf));
}

Bool Renderer::getCurrentDebugRenderTarget(Array<RenderTargetHandle, kMaxDebugRenderTargets>& handles, ShaderProgramPtr& optionalShaderProgram)
{
	if(m_currentDebugRtName.isEmpty()) [[likely]]
	{
		return false;
	}

	RendererObject* obj = nullptr;
	for(const DebugRtInfo& inf : m_debugRts)
	{
		if(inf.m_rtName == m_currentDebugRtName)
		{
			obj = inf.m_obj;
		}
	}
	ANKI_ASSERT(obj);

	obj->getDebugRenderTarget(m_currentDebugRtName, handles, optionalShaderProgram);
	return true;
}

void Renderer::setCurrentDebugRenderTarget(CString rtName)
{
	m_currentDebugRtName.destroy();

	if(!rtName.isEmpty() && rtName.getLength() > 0)
	{
		m_currentDebugRtName = rtName;
	}
}

Format Renderer::getHdrFormat() const
{
	Format out;
	if(!g_highQualityHdrCVar.get())
	{
		out = Format::kB10G11R11_Ufloat_Pack32;
	}
	else if(GrManager::getSingleton().getDeviceCapabilities().m_unalignedBbpTextureFormats)
	{
		out = Format::kR16G16B16_Sfloat;
	}
	else
	{
		out = Format::kR16G16B16A16_Sfloat;
	}
	return out;
}

Format Renderer::getDepthNoStencilFormat() const
{
	if(ANKI_PLATFORM_MOBILE)
	{
		return Format::kX8D24_Unorm_Pack32;
	}
	else
	{
		return Format::kD32_Sfloat;
	}
}

void Renderer::gpuSceneCopy(RenderingContext& ctx)
{
	RenderGraphDescription& rgraph = ctx.m_renderGraphDescr;

	m_runCtx.m_gpuSceneHandle =
		rgraph.importBuffer(&GpuSceneBuffer::getSingleton().getBuffer(), GpuSceneBuffer::getSingleton().getBuffer().getBufferUsage());

	if(GpuSceneMicroPatcher::getSingleton().patchingIsNeeded())
	{
		ComputeRenderPassDescription& rpass = rgraph.newComputeRenderPass("GPU scene patching");
		rpass.newBufferDependency(m_runCtx.m_gpuSceneHandle, BufferUsageBit::kStorageComputeWrite);

		rpass.setWork([](RenderPassWorkContext& rgraphCtx) {
			GpuSceneMicroPatcher::getSingleton().patchGpuScene(*rgraphCtx.m_commandBuffer);
		});
	}
}

} // end namespace anki
