// Copyright (C) 2009-2023, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Scene/VisibilityInternal.h>
#include <AnKi/Scene/SceneGraph.h>
#include <AnKi/Scene/Components/LensFlareComponent.h>
#include <AnKi/Scene/Components/ModelComponent.h>
#include <AnKi/Scene/Components/ReflectionProbeComponent.h>
#include <AnKi/Scene/Components/DecalComponent.h>
#include <AnKi/Scene/Components/MoveComponent.h>
#include <AnKi/Scene/Components/FogDensityComponent.h>
#include <AnKi/Scene/Components/LightComponent.h>
#include <AnKi/Scene/Components/GlobalIlluminationProbeComponent.h>
#include <AnKi/Scene/Components/ParticleEmitterComponent.h>
#include <AnKi/Scene/Components/UiComponent.h>
#include <AnKi/Scene/Components/SkyboxComponent.h>
#include <AnKi/Scene/Components/CameraComponent.h>
#include <AnKi/Renderer/MainRenderer.h>
#include <AnKi/Util/Logger.h>
#include <AnKi/Util/ThreadHive.h>
#include <AnKi/Core/CVarSet.h>

namespace anki {

static U8 computeLod(const Frustum& frustum, F32 distanceFromTheNearPlane)
{
	static_assert(kMaxLodCount == 3, "Wrong assumption");
	U8 lod;
	if(distanceFromTheNearPlane < 0.0f)
	{
		// In RT objects may fall behind the camera, use the max LOD on those
		lod = 2;
	}
	else if(distanceFromTheNearPlane <= frustum.getLodDistance(0))
	{
		lod = 0;
	}
	else if(distanceFromTheNearPlane <= frustum.getLodDistance(1))
	{
		lod = 1;
	}
	else
	{
		lod = 2;
	}

	return lod;
}

static FrustumFlags getCameraFrustumFlags()
{
	FrustumFlags flags;
	flags.m_gatherParticleComponents = true;
	flags.m_gatherProbeComponents = true;
	flags.m_gatherLightComponents = true;
	flags.m_gatherLensFlareComponents = true;
	flags.m_gatherDecalComponents = true;
	flags.m_gatherFogDensityComponents = true;
	flags.m_gatherUiComponents = true;
	flags.m_gatherSkyComponents = true;
	flags.m_coverageBuffer = true;
	flags.m_nonDirectionalLightsCastShadow = true;
	flags.m_directionalLightsCastShadow = true;
	return flags;
}

static FrustumFlags getCameraExtendedFrustumFlags()
{
	FrustumFlags flags;
	flags.m_gatherRayTracingModelComponents = true;
	flags.m_gatherLightComponents = true;
	flags.m_gatherSkyComponents = true;
	return flags;
}

void VisibilityContext::submitNewWork(const VisibilityFrustum& frustum, const VisibilityFrustum& primaryFrustum, RenderQueue& rqueue,
									  ThreadHive& hive)
{
	ANKI_TRACE_SCOPED_EVENT(SceneVisSubmitWork);

	rqueue.m_cameraTransform = Mat3x4(frustum.m_frustum->getWorldTransform());
	rqueue.m_viewMatrix = frustum.m_frustum->getViewMatrix();
	rqueue.m_projectionMatrix = frustum.m_frustum->getProjectionMatrix();
	rqueue.m_viewProjectionMatrix = frustum.m_frustum->getViewProjectionMatrix();
	rqueue.m_previousViewProjectionMatrix = frustum.m_frustum->getPreviousViewProjectionMatrix();
	rqueue.m_cameraNear = frustum.m_frustum->getNear();
	rqueue.m_cameraFar = frustum.m_frustum->getFar();
	if(frustum.m_frustum->getFrustumType() == FrustumType::kPerspective)
	{
		rqueue.m_cameraFovX = frustum.m_frustum->getFovX();
		rqueue.m_cameraFovY = frustum.m_frustum->getFovY();
	}
	else
	{
		rqueue.m_cameraFovX = rqueue.m_cameraFovY = 0.0f;
	}

	// Check if this frc was tested before
	{
		LockGuard<Mutex> l(m_testedFrustumsMtx);

		// Check if already in the list
		for(const Frustum* x : m_testedFrustums)
		{
			if(x == frustum.m_frustum)
			{
				return;
			}
		}

		// Not there, push it
		m_testedFrustums.pushBack(frustum.m_frustum);
	}

	// Prepare the ctx
	FrustumVisibilityContext* frcCtx = newInstance<FrustumVisibilityContext>(SceneGraph::getSingleton().getFrameMemoryPool());
	frcCtx->m_visCtx = this;
	frcCtx->m_frustum = frustum;
	frcCtx->m_primaryFrustum = primaryFrustum;
	frcCtx->m_queueViews.resize(hive.getThreadCount());
	frcCtx->m_visTestsSignalSem = hive.newSemaphore(1);
	frcCtx->m_renderQueue = &rqueue;

	// Submit new work
	//

	// Software rasterizer task
	ThreadHiveSemaphore* prepareRasterizerSem = nullptr;
	if(frustum.m_coverageBuffer && frustum.m_frustum->hasCoverageBuffer())
	{
		// Gather triangles task
		ThreadHiveTask fillDepthTask = ANKI_THREAD_HIVE_TASK(
			{ self->fill(); }, newInstance<FillRasterizerWithCoverageTask>(SceneGraph::getSingleton().getFrameMemoryPool(), frcCtx), nullptr,
			hive.newSemaphore(1));

		hive.submitTasks(&fillDepthTask, 1);

		prepareRasterizerSem = fillDepthTask.m_signalSemaphore;
	}

	if(frustum.m_coverageBuffer)
	{
		rqueue.m_fillCoverageBufferCallback = [](void* ud, F32* depthValues, U32 width, U32 height) {
			static_cast<Frustum*>(ud)->setCoverageBuffer(depthValues, width, height);
		};
		rqueue.m_fillCoverageBufferCallbackUserData = static_cast<Frustum*>(frustum.m_frustum);
	}

	// Gather visibles from the octree. No need to signal anything because it will spawn new tasks
	ThreadHiveTask gatherTask = ANKI_THREAD_HIVE_TASK(
		{ self->gather(hive); }, newInstance<GatherVisiblesFromOctreeTask>(SceneGraph::getSingleton().getFrameMemoryPool(), frcCtx),
		prepareRasterizerSem, nullptr);
	hive.submitTasks(&gatherTask, 1);

	// Combind results task
	ANKI_ASSERT(frcCtx->m_visTestsSignalSem);
	ThreadHiveTask combineTask =
		ANKI_THREAD_HIVE_TASK({ self->combine(); }, newInstance<CombineResultsTask>(SceneGraph::getSingleton().getFrameMemoryPool(), frcCtx),
							  frcCtx->m_visTestsSignalSem, nullptr);
	hive.submitTasks(&combineTask, 1);
}

void FillRasterizerWithCoverageTask::fill()
{
	ANKI_TRACE_SCOPED_EVENT(SceneVisFillDepth);

	// Get the C-Buffer
	ConstWeakArray<F32> depthBuff;
	U32 width;
	U32 height;
	m_frcCtx->m_frustum.m_frustum->getCoverageBufferInfo(depthBuff, width, height);
	ANKI_ASSERT(width > 0 && height > 0 && depthBuff.getSize() > 0);

	// Init the rasterizer
	if(true)
	{
		m_frcCtx->m_r = newInstance<SoftwareRasterizer>(SceneGraph::getSingleton().getFrameMemoryPool());
		m_frcCtx->m_r->prepare(Mat4(m_frcCtx->m_frustum.m_frustum->getPreviousViewMatrix(1), Vec4(0.0f, 0.0f, 0.0f, 1.0f)),
							   m_frcCtx->m_frustum.m_frustum->getPreviousProjectionMatrix(1), width, height);

		// Do the work
		m_frcCtx->m_r->fillDepthBuffer(depthBuff);
	}
}

void GatherVisiblesFromOctreeTask::gather(ThreadHive& hive)
{
	ANKI_TRACE_SCOPED_EVENT(SceneVisOctreeGather);

	U32 testIdx = m_frcCtx->m_visCtx->m_testsCount.fetchAdd(1);

	// Walk the tree
	SceneGraph::getSingleton().getOctree().walkTree(
		testIdx,
		[&](const Aabb& box) {
			Bool visible = m_frcCtx->m_frustum.m_frustum->insideFrustum(box);
			if(visible && m_frcCtx->m_r)
			{
				// visible = m_frcCtx->m_r->visibilityTest(box);
			}

			return visible;
		},
		[&](void* placeableUserData) {
			ANKI_ASSERT(placeableUserData);
			Spatial* spatial = static_cast<Spatial*>(placeableUserData);
			const SceneComponentType compType = spatial->getSceneComponentClassId();
			const FrustumFlags frustumFlags = m_frcCtx->m_frustum;

			Bool gather = false;

			if(compType == ModelComponent::kClassType)
			{
				gather = frustumFlags.m_gatherModelComponents || frustumFlags.m_gatherShadowCasterModelComponents
						 || frustumFlags.m_gatherRayTracingModelComponents;
			}
			else if(compType == ParticleEmitterComponent::kClassType)
			{
				gather = frustumFlags.m_gatherParticleComponents;
			}
			else if(compType == LightComponent::kClassType)
			{
				gather = frustumFlags.m_gatherLightComponents;
			}
			else if(compType == LensFlareComponent::kClassType)
			{
				gather = frustumFlags.m_gatherLensFlareComponents;
			}
			else if(compType == ReflectionProbeComponent::kClassType)
			{
				gather = frustumFlags.m_gatherProbeComponents;
			}
			else if(compType == DecalComponent::kClassType)
			{
				gather = frustumFlags.m_gatherDecalComponents;
			}
			else if(compType == FogDensityComponent::kClassType)
			{
				gather = frustumFlags.m_gatherFogDensityComponents;
			}
			else if(compType == GlobalIlluminationProbeComponent::kClassType)
			{
				gather = frustumFlags.m_gatherProbeComponents;
			}
			else if(compType == UiComponent::kClassType)
			{
				gather = frustumFlags.m_gatherUiComponents;
			}
			else if(compType == SkyboxComponent::kClassType)
			{
				gather = frustumFlags.m_gatherSkyComponents;
			}
			else
			{
				ANKI_ASSERT(0);
			}

			if(!gather)
			{
				return;
			}

			ANKI_ASSERT(m_spatialCount < m_spatials.getSize());
			m_spatials[m_spatialCount++] = spatial;

			if(m_spatialCount == m_spatials.getSize())
			{
				flush(hive);
			}
		});

	// Flush the remaining
	flush(hive);

	// Fire an additional dummy task to decrease the semaphore to zero
	GatherVisiblesFromOctreeTask* pself = this; // MSVC workaround
	ThreadHiveTask task = ANKI_THREAD_HIVE_TASK({}, pself, nullptr, m_frcCtx->m_visTestsSignalSem);
	hive.submitTasks(&task, 1);
}

void GatherVisiblesFromOctreeTask::flush(ThreadHive& hive)
{
	if(m_spatialCount)
	{
		// Create the task
		VisibilityTestTask* vis = newInstance<VisibilityTestTask>(SceneGraph::getSingleton().getFrameMemoryPool(), m_frcCtx);
		memcpy(&vis->m_spatialsToTest[0], &m_spatials[0], sizeof(m_spatials[0]) * m_spatialCount);
		vis->m_spatialToTestCount = m_spatialCount;

		// Increase the semaphore to block the CombineResultsTask
		m_frcCtx->m_visTestsSignalSem->increaseSemaphore(1);

		// Submit task
		ThreadHiveTask task = ANKI_THREAD_HIVE_TASK({ self->test(hive, threadId); }, vis, nullptr, m_frcCtx->m_visTestsSignalSem);
		hive.submitTasks(&task, 1);

		// Clear count
		m_spatialCount = 0;
	}
}

void VisibilityTestTask::test(ThreadHive& hive, U32 taskId)
{
	ANKI_TRACE_SCOPED_EVENT(SceneVisTest);

	const Frustum& testedFrustum = *m_frcCtx->m_frustum.m_frustum;
	ANKI_ASSERT(m_frcCtx->m_primaryFrustum.m_frustum);
	const FrustumFlags frustumFlags = m_frcCtx->m_frustum;
	const Frustum& primaryFrustum = *m_frcCtx->m_primaryFrustum.m_frustum;

	StackMemoryPool& framePool = SceneGraph::getSingleton().getFrameMemoryPool();

	WeakArray<RenderQueue> nextQueues;
	WeakArray<VisibilityFrustum> nextFrustums;

	// Iterate
	RenderQueueView& result = m_frcCtx->m_queueViews[taskId];
	for(U i = 0; i < m_spatialToTestCount; ++i)
	{
		Spatial* spatial = m_spatialsToTest[i];
		ANKI_ASSERT(spatial);
		SceneComponent& comp = spatial->getSceneComponent();
		const SceneComponentType compType = spatial->getSceneComponentClassId();
		const Aabb& aabb = spatial->getAabbWorldSpace();

		auto isInside = [&] {
			return spatial->getAlwaysVisible() || (testedFrustum.insideFrustum(aabb) && testAgainstRasterizer(aabb));
		};

		if(compType == ModelComponent::kClassType)
		{
			const ModelComponent& modelc = static_cast<ModelComponent&>(comp);
			const Bool isShadowFrustum = frustumFlags.m_gatherShadowCasterModelComponents;
			if(!modelc.isEnabled() || (isShadowFrustum && !modelc.getCastsShadow()) || !isInside())
			{
				continue;
			}

			const Plane& nearPlane = primaryFrustum.getViewPlanes()[FrustumPlaneType::kNear];
			const F32 distanceFromCamera = max(0.0f, testPlane(nearPlane, aabb));
			const U8 lod = computeLod(primaryFrustum, distanceFromCamera);

			WeakArray<RenderableQueueElement> elements;
			modelc.setupRenderableQueueElements(lod, (isShadowFrustum) ? RenderingTechnique::kDepth : RenderingTechnique::kGBuffer, elements);
			for(RenderableQueueElement& el : elements)
			{
				el.m_distanceFromCamera = distanceFromCamera;
				*result.m_renderables.newElement() = el;
			}

			modelc.setupRenderableQueueElements(lod, RenderingTechnique::kForward, elements);
			for(RenderableQueueElement& el : elements)
			{
				el.m_distanceFromCamera = distanceFromCamera;
				*result.m_forwardShadingRenderables.newElement() = el;
			}

			if(frustumFlags.m_gatherRayTracingModelComponents)
			{
				WeakArray<RayTracingInstanceQueueElement> rtElements;
				modelc.setupRayTracingInstanceQueueElements(lod, RenderingTechnique::kRtShadow, rtElements);

				for(RayTracingInstanceQueueElement& el : rtElements)
				{
					*result.m_rayTracingInstances.newElement() = el;
				}
			}

			// Update timestamp
			ANKI_ASSERT(comp.getTimestamp() > 0);
			m_frcCtx->m_queueViews[taskId].m_timestamp = max(m_frcCtx->m_queueViews[taskId].m_timestamp, comp.getTimestamp());
		}
		else if(compType == ParticleEmitterComponent::kClassType)
		{
			const ParticleEmitterComponent& partemitc = static_cast<ParticleEmitterComponent&>(comp);
			if(!partemitc.isEnabled() || !isInside())
			{
				continue;
			}

			const Plane& nearPlane = primaryFrustum.getViewPlanes()[FrustumPlaneType::kNear];
			const F32 distanceFromCamera = max(0.0f, testPlane(nearPlane, aabb));
			Bool updateTimestamp = false;

			WeakArray<RenderableQueueElement> elements;
			partemitc.setupRenderableQueueElements(RenderingTechnique::kGBuffer, elements);
			for(RenderableQueueElement& el : elements)
			{
				el.m_distanceFromCamera = distanceFromCamera;
				*result.m_renderables.newElement() = el;
				updateTimestamp = true;
			}

			partemitc.setupRenderableQueueElements(RenderingTechnique::kForward, elements);
			for(RenderableQueueElement& el : elements)
			{
				el.m_distanceFromCamera = distanceFromCamera;
				*result.m_forwardShadingRenderables.newElement() = el;
			}

			// Update timestamp
			if(updateTimestamp)
			{
				ANKI_ASSERT(comp.getTimestamp() > 0);
				m_frcCtx->m_queueViews[taskId].m_timestamp = max(m_frcCtx->m_queueViews[taskId].m_timestamp, comp.getTimestamp());
			}
		}
		else if(compType == LightComponent::kClassType)
		{
			ANKI_ASSERT(!"Lights use GPU visibility from now on");
		}
		else if(compType == LensFlareComponent::kClassType)
		{
			const LensFlareComponent& flarec = static_cast<LensFlareComponent&>(comp);

			if(!isInside() || !flarec.isEnabled())
			{
				continue;
			}

			LensFlareQueueElement* el = result.m_lensFlares.newElement();
			flarec.setupLensFlareQueueElement(*el);
		}
		else if(compType == ReflectionProbeComponent::kClassType)
		{
			ANKI_ASSERT(!"Refl probes use GPU visibility from now on");
		}
		else if(compType == DecalComponent::kClassType)
		{
			const DecalComponent& decalc = static_cast<DecalComponent&>(comp);

			if(!isInside() || !decalc.isEnabled())
			{
				continue;
			}

			DecalQueueElement* el = result.m_decals.newElement();
			decalc.setupDecalQueueElement(*el);
		}
		else if(compType == FogDensityComponent::kClassType)
		{
			if(!isInside())
			{
				continue;
			}

			const FogDensityComponent& fogc = static_cast<FogDensityComponent&>(comp);

			FogDensityQueueElement* el = result.m_fogDensityVolumes.newElement();
			fogc.setupFogDensityQueueElement(*el);
		}
		else if(compType == GlobalIlluminationProbeComponent::kClassType)
		{
			ANKI_ASSERT(!"GI probes use GPU visibility from now on");
		}
		else if(compType == UiComponent::kClassType)
		{
			if(!isInside())
			{
				continue;
			}

			const UiComponent& uic = static_cast<UiComponent&>(comp);
			UiQueueElement* el = result.m_uis.newElement();
			uic.setupUiQueueElement(*el);
		}
		else if(compType == SkyboxComponent::kClassType)
		{
			if(!isInside())
			{
				continue;
			}

			const SkyboxComponent& skyboxc = static_cast<SkyboxComponent&>(comp);
			skyboxc.setupSkyboxQueueElement(result.m_skybox);
			result.m_skyboxSet = true;
		}
		else
		{
			ANKI_ASSERT(0);
		}

		// Add more frustums to the list
		if(nextQueues.getSize() > 0)
		{
			ANKI_ASSERT(nextFrustums.getSize() == nextQueues.getSize());
			for(U32 i = 0; i < nextQueues.getSize(); ++i)
			{
				m_frcCtx->m_visCtx->submitNewWork(nextFrustums[i], m_frcCtx->m_primaryFrustum, nextQueues[i], hive);
			}
		}
	} // end for
}

void CombineResultsTask::combine()
{
	ANKI_TRACE_SCOPED_EVENT(SceneVisCombine);

	RenderQueue& results = *m_frcCtx->m_renderQueue;

	// Compute the timestamp
	const U32 threadCount = m_frcCtx->m_queueViews.getSize();
	results.m_shadowRenderablesLastUpdateTimestamp = 0;
	U32 renderableCount = 0;
	for(U32 i = 0; i < threadCount; ++i)
	{
		results.m_shadowRenderablesLastUpdateTimestamp = max(results.m_shadowRenderablesLastUpdateTimestamp, m_frcCtx->m_queueViews[i].m_timestamp);

		renderableCount += m_frcCtx->m_queueViews[i].m_renderables.m_elementCount;
	}

	if(renderableCount)
	{
		ANKI_ASSERT(results.m_shadowRenderablesLastUpdateTimestamp > 0);
	}
	else
	{
		ANKI_ASSERT(results.m_shadowRenderablesLastUpdateTimestamp == 0);
	}

	if(m_frcCtx->m_frustum.m_frustum->getUpdatedThisFrame())
	{
		results.m_shadowRenderablesLastUpdateTimestamp = GlobalFrameIndex::getSingleton().m_value;
	}

#define ANKI_VIS_COMBINE(t_, member_) \
	{ \
		Array<TRenderQueueElementStorage<t_>, 64> subStorages; \
		for(U32 i = 0; i < threadCount; ++i) \
		{ \
			subStorages[i] = m_frcCtx->m_queueViews[i].member_; \
		} \
		combineQueueElements<t_>(WeakArray<TRenderQueueElementStorage<t_>>(&subStorages[0], threadCount), nullptr, results.member_, nullptr); \
	}

	ANKI_VIS_COMBINE(RenderableQueueElement, m_renderables);
	ANKI_VIS_COMBINE(RenderableQueueElement, m_forwardShadingRenderables);
	ANKI_VIS_COMBINE(PointLightQueueElement, m_pointLights);
	ANKI_VIS_COMBINE(SpotLightQueueElement, m_spotLights);
	ANKI_VIS_COMBINE(ReflectionProbeQueueElement, m_reflectionProbes);
	ANKI_VIS_COMBINE(LensFlareQueueElement, m_lensFlares);
	ANKI_VIS_COMBINE(DecalQueueElement, m_decals);
	ANKI_VIS_COMBINE(FogDensityQueueElement, m_fogDensityVolumes);
	ANKI_VIS_COMBINE(GlobalIlluminationProbeQueueElement, m_giProbes);
	ANKI_VIS_COMBINE(GenericGpuComputeJobQueueElement, m_genericGpuComputeJobs);
	ANKI_VIS_COMBINE(RayTracingInstanceQueueElement, m_rayTracingInstances);
	ANKI_VIS_COMBINE(UiQueueElement, m_uis);

#undef ANKI_VIS_COMBINE

	results.m_reflectionProbeForRefresh = m_frcCtx->m_reflectionProbeForRefresh;
	results.m_giProbeForRefresh = m_frcCtx->m_giProbeForRefresh;

	for(U32 i = 0; i < threadCount; ++i)
	{
		if(m_frcCtx->m_queueViews[i].m_directionalLight.m_uuid != 0)
		{
			results.m_directionalLight = m_frcCtx->m_queueViews[i].m_directionalLight;
		}

		if(m_frcCtx->m_queueViews[i].m_skyboxSet)
		{
			results.m_skybox = m_frcCtx->m_queueViews[i].m_skybox;
		}
	}

	const Bool isShadowFrustum = m_frcCtx->m_frustum.m_gatherShadowCasterModelComponents;

	// Sort some of the arrays
	if(!isShadowFrustum)
	{
		std::sort(results.m_renderables.getBegin(), results.m_renderables.getEnd(), MaterialDistanceSortFunctor());

		std::sort(results.m_forwardShadingRenderables.getBegin(), results.m_forwardShadingRenderables.getEnd(),
				  RevDistanceSortFunctor<RenderableQueueElement>());
	}

	std::sort(results.m_giProbes.getBegin(), results.m_giProbes.getEnd());

	// Sort the ligths as well because some rendering effects expect the same order from frame to frame
	std::sort(results.m_pointLights.getBegin(), results.m_pointLights.getEnd(),
			  [](const PointLightQueueElement& a, const PointLightQueueElement& b) -> Bool {
				  if(a.hasShadow() != b.hasShadow())
				  {
					  return a.hasShadow() < b.hasShadow();
				  }
				  else
				  {
					  return a.m_uuid < b.m_uuid;
				  }
			  });

	std::sort(results.m_spotLights.getBegin(), results.m_spotLights.getEnd(),
			  [](const SpotLightQueueElement& a, const SpotLightQueueElement& b) -> Bool {
				  if(a.hasShadow() != b.hasShadow())
				  {
					  return a.hasShadow() > b.hasShadow();
				  }
				  else
				  {
					  return a.m_uuid < b.m_uuid;
				  }
			  });

	// Cleanup
	if(m_frcCtx->m_r)
	{
		m_frcCtx->m_r->~SoftwareRasterizer();
	}
}

template<typename T>
void CombineResultsTask::combineQueueElements(WeakArray<TRenderQueueElementStorage<T>> subStorages,
											  WeakArray<TRenderQueueElementStorage<U32>>* ptrSubStorages, WeakArray<T>& combined,
											  WeakArray<T*>* ptrCombined)
{
	StackMemoryPool& framePool = SceneGraph::getSingleton().getFrameMemoryPool();

	U32 totalElCount = subStorages[0].m_elementCount;
	U32 biggestSubStorageIdx = 0;
	for(U32 i = 1; i < subStorages.getSize(); ++i)
	{
		totalElCount += subStorages[i].m_elementCount;

		if(subStorages[i].m_elementStorage > subStorages[biggestSubStorageIdx].m_elementStorage)
		{
			biggestSubStorageIdx = i;
		}
	}

	if(totalElCount == 0)
	{
		return;
	}

	// Count ptrSubStorage elements
	T** ptrIt = nullptr;
	if(ptrSubStorages != nullptr)
	{
		ANKI_ASSERT(ptrCombined);
		U32 ptrTotalElCount = (*ptrSubStorages)[0].m_elementCount;

		for(U32 i = 1; i < ptrSubStorages->getSize(); ++i)
		{
			ptrTotalElCount += (*ptrSubStorages)[i].m_elementCount;
		}

		// Create the new storage
		if(ptrTotalElCount > 0)
		{
			ptrIt = newArray<T*>(framePool, ptrTotalElCount);
			*ptrCombined = WeakArray<T*>(ptrIt, ptrTotalElCount);
		}
	}

	T* it;
	if(totalElCount > subStorages[biggestSubStorageIdx].m_elementStorage)
	{
		// Can't reuse any of the existing storage, will allocate a brand new one

		it = newArray<T>(framePool, totalElCount);
		biggestSubStorageIdx = kMaxU32;

		combined = WeakArray<T>(it, totalElCount);
	}
	else
	{
		// Will reuse existing storage

		it = subStorages[biggestSubStorageIdx].m_elements + subStorages[biggestSubStorageIdx].m_elementCount;

		combined = WeakArray<T>(subStorages[biggestSubStorageIdx].m_elements, totalElCount);
	}

	for(U32 i = 0; i < subStorages.getSize(); ++i)
	{
		if(subStorages[i].m_elementCount == 0)
		{
			continue;
		}

		// Copy the pointers
		if(ptrIt)
		{
			T* base = (i != biggestSubStorageIdx) ? it : subStorages[biggestSubStorageIdx].m_elements;

			for(U32 x = 0; x < (*ptrSubStorages)[i].m_elementCount; ++x)
			{
				ANKI_ASSERT((*ptrSubStorages)[i].m_elements[x] < subStorages[i].m_elementCount);

				*ptrIt = base + (*ptrSubStorages)[i].m_elements[x];

				++ptrIt;
			}

			ANKI_ASSERT(ptrIt <= ptrCombined->getEnd());
		}

		// Copy the elements
		if(i != biggestSubStorageIdx)
		{
			memcpy(it, subStorages[i].m_elements, sizeof(T) * subStorages[i].m_elementCount);
			it += subStorages[i].m_elementCount;
		}
	}
}

void SceneGraph::doVisibilityTests(SceneNode& camera, SceneGraph& scene, RenderQueue& rqueue)
{
	ANKI_TRACE_SCOPED_EVENT(SceneVisTests);

	ThreadHive& hive = CoreThreadHive::getSingleton();

	VisibilityContext ctx;
	CameraComponent& camerac = camera.getFirstComponentOfType<CameraComponent>();
	VisibilityFrustum visFrustum;
	visFrustum.m_frustum = &camerac.getFrustum();
	static_cast<FrustumFlags&>(visFrustum) = getCameraFrustumFlags();
	ctx.submitNewWork(visFrustum, visFrustum, rqueue, hive);

	if(camerac.getHasExtendedFrustum())
	{
		VisibilityFrustum evisFrustum;
		evisFrustum.m_frustum = &camerac.getExtendedFrustum();
		static_cast<FrustumFlags&>(evisFrustum) = getCameraExtendedFrustumFlags();

		rqueue.m_rayTracingQueue = newInstance<RenderQueue>(scene.getFrameMemoryPool());
		ctx.submitNewWork(evisFrustum, visFrustum, *rqueue.m_rayTracingQueue, hive);
	}

	hive.waitAllTasks();
}

} // end namespace anki
