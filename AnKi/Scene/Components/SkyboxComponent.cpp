// Copyright (C) 2009-2023, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSEJ

#include <AnKi/Scene/Components/SkyboxComponent.h>
#include <AnKi/Scene/SceneNode.h>
#include <AnKi/Scene/SceneGraph.h>
#include <AnKi/Resource/ImageResource.h>
#include <AnKi/Resource/ResourceManager.h>
#include <AnKi/Renderer/RenderQueue.h>

namespace anki {

SkyboxComponent::SkyboxComponent(SceneNode* node)
	: SceneComponent(node, getStaticClassId())
	, m_spatial(this)
{
	m_spatial.setAlwaysVisible(true);
	m_spatial.setUpdatesOctreeBounds(false);
}

SkyboxComponent::~SkyboxComponent()
{
	m_spatial.removeFromOctree(SceneGraph::getSingleton().getOctree());
}

void SkyboxComponent::loadImageResource(CString filename)
{
	ImageResourcePtr img;
	const Error err = ResourceManager::getSingleton().loadResource(filename, img);
	if(err)
	{
		ANKI_SCENE_LOGE("Setting skybox image failed. Ignoring error");
		return;
	}

	m_image = std::move(img);
	m_type = SkyboxType::kImage2D;
}

Error SkyboxComponent::update([[maybe_unused]] SceneComponentUpdateInfo& info, Bool& updated)
{
	updated = m_spatial.update(SceneGraph::getSingleton().getOctree());
	return Error::kNone;
}

void SkyboxComponent::setupSkyboxQueueElement(SkyboxQueueElement& queueElement) const
{
	if(m_type == SkyboxType::kImage2D)
	{
		queueElement.m_skyboxTexture = &m_image->getTextureView();
	}
	else
	{
		queueElement.m_skyboxTexture = nullptr;
		queueElement.m_solidColor = m_color;
	}

	queueElement.m_fog.m_minDensity = m_fog.m_minDensity;
	queueElement.m_fog.m_maxDensity = m_fog.m_maxDensity;
	queueElement.m_fog.m_heightOfMinDensity = m_fog.m_heightOfMinDensity;
	queueElement.m_fog.m_heightOfMaxDensity = m_fog.m_heightOfMaxDensity;
	queueElement.m_fog.m_scatteringCoeff = m_fog.m_scatteringCoeff;
	queueElement.m_fog.m_absorptionCoeff = m_fog.m_absorptionCoeff;
	queueElement.m_fog.m_diffuseColor = m_fog.m_diffuseColor;
}

} // end namespace anki
