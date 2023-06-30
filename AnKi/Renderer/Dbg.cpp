// Copyright (C) 2009-2023, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Renderer/Dbg.h>
#include <AnKi/Renderer/Renderer.h>
#include <AnKi/Renderer/GBuffer.h>
#include <AnKi/Renderer/LightShading.h>
#include <AnKi/Renderer/FinalComposite.h>
#include <AnKi/Renderer/RenderQueue.h>
#include <AnKi/Scene.h>
#include <AnKi/Util/Logger.h>
#include <AnKi/Util/Enum.h>
#include <AnKi/Core/ConfigSet.h>
#include <AnKi/Collision/ConvexHullShape.h>
#include <AnKi/Physics/PhysicsWorld.h>

namespace anki {

BoolCVar g_dbgCVar(CVarSubsystem::kRenderer, "Dbg", false, "Enable or not debug visualization");
static BoolCVar g_dbgPhysicsCVar(CVarSubsystem::kRenderer, "DbgPhysics", false, "Enable or not physics debug visualization");

Dbg::Dbg()
{
}

Dbg::~Dbg()
{
}

Error Dbg::init()
{
	ANKI_R_LOGV("Initializing DBG");

	// RT descr
	m_rtDescr = getRenderer().create2DRenderTargetDescription(getRenderer().getInternalResolution().x(), getRenderer().getInternalResolution().y(),
															  Format::kR8G8B8A8_Unorm, "Dbg");
	m_rtDescr.bake();

	// Create FB descr
	m_fbDescr.m_colorAttachmentCount = 1;
	m_fbDescr.m_colorAttachments[0].m_loadOperation = AttachmentLoadOperation::kClear;
	m_fbDescr.m_depthStencilAttachment.m_loadOperation = AttachmentLoadOperation::kLoad;
	m_fbDescr.m_depthStencilAttachment.m_stencilLoadOperation = AttachmentLoadOperation::kDontCare;
	m_fbDescr.m_depthStencilAttachment.m_aspect = DepthStencilAspectBit::kDepth;
	m_fbDescr.bake();

	ResourceManager& rsrcManager = ResourceManager::getSingleton();
	ANKI_CHECK(m_drawer.init());
	ANKI_CHECK(rsrcManager.loadResource("EngineAssets/GiProbe.ankitex", m_giProbeImage));
	ANKI_CHECK(rsrcManager.loadResource("EngineAssets/LightBulb.ankitex", m_pointLightImage));
	ANKI_CHECK(rsrcManager.loadResource("EngineAssets/SpotLight.ankitex", m_spotLightImage));
	ANKI_CHECK(rsrcManager.loadResource("EngineAssets/GreenDecal.ankitex", m_decalImage));
	ANKI_CHECK(rsrcManager.loadResource("EngineAssets/Mirror.ankitex", m_reflectionImage));

	return Error::kNone;
}

void Dbg::run(RenderPassWorkContext& rgraphCtx, const RenderingContext& ctx)
{
	ANKI_ASSERT(g_dbgCVar.get());

	CommandBuffer& cmdb = *rgraphCtx.m_commandBuffer;

	// Set common state
	cmdb.setViewport(0, 0, getRenderer().getInternalResolution().x(), getRenderer().getInternalResolution().y());
	cmdb.setDepthWrite(false);

	cmdb.bindSampler(0, 1, getRenderer().getSamplers().m_nearestNearestClamp.get());

	rgraphCtx.bindTexture(0, 2, getRenderer().getGBuffer().getDepthRt(), TextureSubresourceInfo(DepthStencilAspectBit::kDepth));

	cmdb.setBlendFactors(0, BlendFactor::kSrcAlpha, BlendFactor::kOneMinusSrcAlpha);
	cmdb.setDepthCompareOperation((m_depthTestOn) ? CompareOperation::kLess : CompareOperation::kAlways);

	// Draw renderables
	const U32 threadId = rgraphCtx.m_currentSecondLevelCommandBufferIndex;
	const U32 threadCount = rgraphCtx.m_secondLevelCommandBufferCount;
	const U32 problemSize = ctx.m_renderQueue->m_renderables.getSize();
	U32 start, end;
	splitThreadedProblem(threadId, threadCount, problemSize, start, end);

	// Renderables
	for(U32 i = start; i < end; ++i)
	{
		const RenderableQueueElement& el = ctx.m_renderQueue->m_renderables[i];

		const Vec3 tsl = (el.m_aabbMin + el.m_aabbMax) / 2.0f;
		constexpr F32 kMargin = 0.1f;
		const Vec3 scale = (el.m_aabbMax - el.m_aabbMin + kMargin) / 2.0f;

		// Set non uniform scale. Add a margin to avoid flickering
		Mat3 nonUniScale = Mat3::getZero();

		nonUniScale(0, 0) = scale.x();
		nonUniScale(1, 1) = scale.y();
		nonUniScale(2, 2) = scale.z();

		const Mat4 mvp = ctx.m_matrices.m_viewProjection * Mat4(tsl.xyz1(), Mat3::getIdentity() * nonUniScale, 1.0f);

		m_drawer.drawCube(mvp, Vec4(1.0f, 0.0f, 1.0f, 1.0f), 2.0f, m_ditheredDepthTestOn, 2.0f, cmdb);
	}

	// Forward shaded renderables
	if(threadId == 0)
	{
		for(const RenderableQueueElement& el : ctx.m_renderQueue->m_forwardShadingRenderables)
		{
			const Vec3 tsl = (el.m_aabbMin + el.m_aabbMax) / 2.0f;
			constexpr F32 kMargin = 0.1f;
			const Vec3 scale = (el.m_aabbMax - el.m_aabbMin + kMargin) / 2.0f;

			// Set non uniform scale. Add a margin to avoid flickering
			Mat3 nonUniScale = Mat3::getZero();

			nonUniScale(0, 0) = scale.x();
			nonUniScale(1, 1) = scale.y();
			nonUniScale(2, 2) = scale.z();

			const Mat4 mvp = ctx.m_matrices.m_viewProjection * Mat4(tsl.xyz1(), Mat3::getIdentity() * nonUniScale, 1.0f);

			m_drawer.drawCube(mvp, Vec4(1.0f, 0.0f, 1.0f, 1.0f), 2.0f, m_ditheredDepthTestOn, 2.0f, cmdb);
		}
	}

	// GI probes
	if(threadId == 0)
	{
		for(const GlobalIlluminationProbeQueueElement& el : ctx.m_renderQueue->m_giProbes)
		{
			const Vec3 tsl = (el.m_aabbMax + el.m_aabbMin) / 2.0f;
			const Vec3 scale = (tsl - el.m_aabbMin);

			// Set non uniform scale.
			Mat3 rot = Mat3::getIdentity();
			rot(0, 0) *= scale.x();
			rot(1, 1) *= scale.y();
			rot(2, 2) *= scale.z();

			const Mat4 mvp = ctx.m_matrices.m_viewProjection * Mat4(tsl.xyz1(), rot, 1.0f);

			m_drawer.drawCubes(ConstWeakArray<Mat4>(&mvp, 1), Vec4(0.729f, 0.635f, 0.196f, 1.0f), 1.0f, m_ditheredDepthTestOn, 2.0f, cmdb);

			m_drawer.drawBillboardTextures(ctx.m_matrices.m_projection, ctx.m_matrices.m_view, ConstWeakArray<Vec3>(&tsl, 1), Vec4(1.0f),
										   m_ditheredDepthTestOn, &m_giProbeImage->getTextureView(),
										   getRenderer().getSamplers().m_trilinearRepeatAniso.get(), Vec2(0.75f), cmdb);
		}
	}

	// Lights
	if(threadId == 0)
	{
		for(const PointLightQueueElement& el : ctx.m_renderQueue->m_pointLights)
		{
			Vec3 color = el.m_diffuseColor.xyz();
			color /= max(max(color.x(), color.y()), color.z());

			m_drawer.drawBillboardTexture(ctx.m_matrices.m_projection, ctx.m_matrices.m_view, el.m_worldPosition, color.xyz1(), m_ditheredDepthTestOn,
										  &m_pointLightImage->getTextureView(), getRenderer().getSamplers().m_trilinearRepeatAniso.get(), Vec2(0.75f),
										  cmdb);
		}

		for(const SpotLightQueueElement& el : ctx.m_renderQueue->m_spotLights)
		{
			Vec3 color = el.m_diffuseColor.xyz();
			color /= max(max(color.x(), color.y()), color.z());

			m_drawer.drawBillboardTexture(ctx.m_matrices.m_projection, ctx.m_matrices.m_view, el.m_worldTransform.getTranslationPart().xyz(),
										  color.xyz1(), m_ditheredDepthTestOn, &m_spotLightImage->getTextureView(),
										  getRenderer().getSamplers().m_trilinearRepeatAniso.get(), Vec2(0.75f), cmdb);
		}
	}

	// Decals
	if(threadId == 0)
	{
		for(const DecalQueueElement& el : ctx.m_renderQueue->m_decals)
		{
			const Mat3 rot = el.m_obbRotation;
			const Vec4 tsl = el.m_obbCenter.xyz1();
			const Vec3 scale = el.m_obbExtend;

			Mat3 nonUniScale = Mat3::getZero();
			nonUniScale(0, 0) = scale.x();
			nonUniScale(1, 1) = scale.y();
			nonUniScale(2, 2) = scale.z();

			const Mat4 mvp = ctx.m_matrices.m_viewProjection * Mat4(tsl, rot * nonUniScale, 1.0f);

			m_drawer.drawCubes(ConstWeakArray<Mat4>(&mvp, 1), Vec4(0.0f, 1.0f, 0.0f, 1.0f), 1.0f, m_ditheredDepthTestOn, 2.0f, cmdb);

			const Vec3 pos = el.m_obbCenter;
			m_drawer.drawBillboardTextures(ctx.m_matrices.m_projection, ctx.m_matrices.m_view, ConstWeakArray<Vec3>(&pos, 1), Vec4(1.0f),
										   m_ditheredDepthTestOn, &m_decalImage->getTextureView(),
										   getRenderer().getSamplers().m_trilinearRepeatAniso.get(), Vec2(0.75f), cmdb);
		}
	}

	// Reflection probes
	if(threadId == 0)
	{
		for(const ReflectionProbeQueueElement& el : ctx.m_renderQueue->m_reflectionProbes)
		{
			const Vec3 tsl = el.m_worldPosition;
			const Vec3 scale = (el.m_aabbMax - el.m_aabbMin);

			// Set non uniform scale.
			Mat3 rot = Mat3::getIdentity();
			rot(0, 0) *= scale.x();
			rot(1, 1) *= scale.y();
			rot(2, 2) *= scale.z();

			const Mat4 mvp = ctx.m_matrices.m_viewProjection * Mat4(tsl.xyz1(), rot, 1.0f);

			m_drawer.drawCubes(ConstWeakArray<Mat4>(&mvp, 1), Vec4(0.0f, 0.0f, 1.0f, 1.0f), 1.0f, m_ditheredDepthTestOn, 2.0f, cmdb);

			m_drawer.drawBillboardTextures(ctx.m_matrices.m_projection, ctx.m_matrices.m_view, ConstWeakArray<Vec3>(&el.m_worldPosition, 1),
										   Vec4(1.0f), m_ditheredDepthTestOn, &m_reflectionImage->getTextureView(),
										   getRenderer().getSamplers().m_trilinearRepeatAniso.get(), Vec2(0.75f), cmdb);
		}
	}

	if(threadId == (threadCount - 1) && g_dbgPhysicsCVar.get())
	{
		m_physicsDrawer.start(ctx.m_matrices.m_viewProjection, cmdb);
		m_physicsDrawer.drawWorld(PhysicsWorld::getSingleton());
		m_physicsDrawer.end();
	}

	// Restore state
	cmdb.setDepthCompareOperation(CompareOperation::kLess);
}

void Dbg::populateRenderGraph(RenderingContext& ctx)
{
	if(!g_dbgCVar.get())
	{
		return;
	}

	RenderGraphDescription& rgraph = ctx.m_renderGraphDescr;

	// Create RT
	m_runCtx.m_rt = rgraph.newRenderTarget(m_rtDescr);

	// Create pass
	GraphicsRenderPassDescription& pass = rgraph.newGraphicsRenderPass("DBG");

	pass.setWork(computeNumberOfSecondLevelCommandBuffers(ctx.m_renderQueue->m_renderables.getSize()),
				 [this, &ctx](RenderPassWorkContext& rgraphCtx) {
					 run(rgraphCtx, ctx);
				 });

	pass.setFramebufferInfo(m_fbDescr, {m_runCtx.m_rt}, getRenderer().getGBuffer().getDepthRt());

	pass.newTextureDependency(m_runCtx.m_rt, TextureUsageBit::kFramebufferWrite);
	pass.newTextureDependency(getRenderer().getGBuffer().getDepthRt(), TextureUsageBit::kSampledFragment | TextureUsageBit::kFramebufferRead);
}

} // end namespace anki
