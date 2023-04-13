// Copyright (C) 2009-2023, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Scene/Components/SceneComponent.h>
#include <AnKi/Scene/RenderStateBucket.h>
#include <AnKi/Scene/Spatial.h>
#include <AnKi/Resource/Forward.h>
#include <AnKi/Util/WeakArray.h>
#include <AnKi/Renderer/RenderQueue.h>

namespace anki {

/// @addtogroup scene
/// @{

/// Holds geometry and material information.
class ModelComponent final : public SceneComponent
{
	ANKI_SCENE_COMPONENT(ModelComponent)

public:
	ModelComponent(SceneNode* node);

	~ModelComponent();

	void loadModelResource(CString filename);

	const ModelResourcePtr& getModelResource() const
	{
		return m_model;
	}

	Bool isEnabled() const
	{
		return m_model.isCreated();
	}

	Bool getCastsShadow() const
	{
		return m_castsShadow;
	}

	void setupRenderableQueueElements(U32 lod, RenderingTechnique technique,
									  WeakArray<RenderableQueueElement>& outRenderables) const;

	void setupRayTracingInstanceQueueElements(U32 lod, RenderingTechnique technique,
											  WeakArray<RayTracingInstanceQueueElement>& outRenderables) const;

private:
	class PatchInfo
	{
	public:
		U32 m_gpuSceneUniformsOffset = kMaxU32;
		U32 m_gpuSceneMeshLodsIndex = kMaxU32;
		U32 m_gpuSceneRenderableIndex = kMaxU32;
		U32 m_gpuSceneRenderableBoundingVolumeIndex = kMaxU32;
		Array2d<RenderStateBucketIndex, U32(RenderingTechnique::kCount), 2> m_renderStateBucketIndices;
		RenderingTechniqueBit m_techniques;
	};

	SceneNode* m_node = nullptr;
	SkinComponent* m_skinComponent = nullptr;
	Spatial m_spatial;

	ModelResourcePtr m_model;

	SegregatedListsGpuMemoryPoolToken m_gpuSceneUniforms;
	U32 m_gpuSceneTransformsIndex = kMaxU32;
	SceneDynamicArray<PatchInfo> m_patchInfos;

	Bool m_dirty : 1 = true;
	Bool m_castsShadow : 1 = false;
	Bool m_movedLastFrame : 1 = true;
	Bool m_firstTimeUpdate : 1 = true; ///< Extra flag in case the component is added in a node that hasn't been moved.

	RenderingTechniqueBit m_presentRenderingTechniques = RenderingTechniqueBit::kNone;

	Error update(SceneComponentUpdateInfo& info, Bool& updated);

	void onOtherComponentRemovedOrAdded(SceneComponent* other, Bool added);
};
/// @}

} // end namespace anki
