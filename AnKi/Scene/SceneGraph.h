// Copyright (C) 2009-2023, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Scene/Common.h>
#include <AnKi/Scene/SceneNode.h>
#include <AnKi/Math.h>
#include <AnKi/Util/HashMap.h>
#include <AnKi/Util/BlockArray.h>
#include <AnKi/Scene/Events/EventManager.h>
#include <AnKi/Resource/Common.h>
#include <AnKi/Core/CVarSet.h>

namespace anki {

// Forward
class Octree;
class RenderQueue;
extern NumericCVar<F32> g_probeEffectiveDistanceCVar;
extern NumericCVar<F32> g_probeShadowEffectiveDistanceCVar;

/// @addtogroup scene
/// @{

class SceneComponentArrays
{
public:
#define ANKI_DEFINE_SCENE_COMPONENT(name, weight) \
	SceneBlockArray<name##Component>& get##name##s() \
	{ \
		return m_##name##Array; \
	}
#include <AnKi/Scene/Components/SceneComponentClasses.def.h>

private:
#define ANKI_DEFINE_SCENE_COMPONENT(name, weight) SceneBlockArray<name##Component> m_##name##Array;
#include <AnKi/Scene/Components/SceneComponentClasses.def.h>
};

/// The scene graph that  all the scene entities
class SceneGraph : public MakeSingleton<SceneGraph>
{
	template<typename>
	friend class MakeSingleton;

	friend class SceneNode;
	friend class UpdateSceneNodesTask;
	friend class Event;

public:
	Error init(AllocAlignedCallback allocCallback, void* allocCallbackData);

	StackMemoryPool& getFrameMemoryPool() const
	{
		return m_framePool;
	}

	SceneNode& getActiveCameraNode()
	{
		ANKI_ASSERT(m_mainCam != nullptr);
		return *m_mainCam;
	}
	const SceneNode& getActiveCameraNode() const
	{
		return *m_mainCam;
	}
	void setActiveCameraNode(SceneNode* cam)
	{
		m_mainCam = cam;
		m_activeCameraChangeTimestamp = GlobalFrameIndex::getSingleton().m_value;
	}
	Timestamp getActiveCameraNodeChangeTimestamp() const
	{
		return m_activeCameraChangeTimestamp;
	}

	U32 getSceneNodesCount() const
	{
		return m_nodesCount;
	}

	EventManager& getEventManager()
	{
		return m_events;
	}
	const EventManager& getEventManager() const
	{
		return m_events;
	}

	Error update(Second prevUpdateTime, Second crntTime);

	void doVisibilityTests(RenderQueue& rqueue);

	SceneNode& findSceneNode(const CString& name);
	SceneNode* tryFindSceneNode(const CString& name);

	/// Iterate the scene nodes using a lambda
	template<typename Func>
	Error iterateSceneNodes(Func func)
	{
		for(SceneNode& psn : m_nodes)
		{
			Error err = func(psn);
			if(err)
			{
				return err;
			}
		}

		return Error::kNone;
	}

	/// Iterate a range of scene nodes using a lambda
	template<typename Func>
	Error iterateSceneNodes(PtrSize begin, PtrSize end, Func func);

	/// Create a new SceneNode
	template<typename Node, typename... Args>
	Error newSceneNode(const CString& name, Node*& node, Args&&... args);

	/// Delete a scene node. It actualy marks it for deletion
	void deleteSceneNode(SceneNode* node)
	{
		node->setMarkedForDeletion();
	}

	void increaseObjectsMarkedForDeletion()
	{
		m_objectsMarkedForDeletionCount.fetchAdd(1);
	}

	const Vec3& getSceneMin() const
	{
		return m_sceneMin;
	}

	const Vec3& getSceneMax() const
	{
		return m_sceneMax;
	}

	/// Get a unique UUID.
	/// @note It's thread-safe.
	U32 getNewUuid()
	{
		return m_nodesUuid.fetchAdd(1);
	}

	Octree& getOctree()
	{
		ANKI_ASSERT(m_octree);
		return *m_octree;
	}

	SceneComponentArrays& getComponentArrays()
	{
		return m_componentArrays;
	}

private:
	class UpdateSceneNodesCtx;

	class InitMemPoolDummy
	{
	public:
		~InitMemPoolDummy()
		{
			SceneMemoryPool::freeSingleton();
		}
	} m_initMemPoolDummy;

	mutable StackMemoryPool m_framePool;

	IntrusiveList<SceneNode> m_nodes;
	U32 m_nodesCount = 0;
	GrHashMap<CString, SceneNode*> m_nodesDict;

	SceneNode* m_mainCam = nullptr;
	Timestamp m_activeCameraChangeTimestamp = 0;
	SceneNode* m_defaultMainCam = nullptr;

	EventManager m_events;

	Octree* m_octree = nullptr;

	Vec3 m_sceneMin = Vec3(-1000.0f, -200.0f, -1000.0f);
	Vec3 m_sceneMax = Vec3(1000.0f, 200.0f, 1000.0f);

	Atomic<U32> m_objectsMarkedForDeletionCount = {0};

	Atomic<U32> m_nodesUuid = {1};

	SceneComponentArrays m_componentArrays;

	SceneGraph();

	~SceneGraph();

	/// Put a node in the appropriate containers
	Error registerNode(SceneNode* node);
	void unregisterNode(SceneNode* node);

	/// Delete the nodes that are marked for deletion
	void deleteNodesMarkedForDeletion();

	Error updateNodes(UpdateSceneNodesCtx& ctx);
	Error updateNode(Second prevTime, Second crntTime, SceneNode& node);

	/// Do visibility tests.
	static void doVisibilityTests(SceneNode& frustumable, SceneGraph& scene, RenderQueue& rqueue);
};

template<typename Node, typename... Args>
inline Error SceneGraph::newSceneNode(const CString& name, Node*& node, Args&&... args)
{
	Error err = Error::kNone;

	node = newInstance<Node>(SceneMemoryPool::getSingleton(), name);
	if(node)
	{
		err = node->init(std::forward<Args>(args)...);
	}
	else
	{
		err = Error::kOutOfMemory;
	}

	if(!err)
	{
		err = registerNode(node);
	}

	if(err)
	{
		ANKI_SCENE_LOGE("Failed to create scene node: %s", (name.isEmpty()) ? "unnamed" : &name[0]);

		if(node)
		{
			deleteInstance(SceneMemoryPool::getSingleton(), node);
			node = nullptr;
		}
	}

	return err;
}

template<typename Func>
Error SceneGraph::iterateSceneNodes(PtrSize begin, PtrSize end, Func func)
{
	ANKI_ASSERT(begin < m_nodesCount && end <= m_nodesCount);
	auto it = m_nodes.getBegin() + begin;

	PtrSize count = end - begin;
	Error err = Error::kNone;
	while(count-- != 0 && !err)
	{
		ANKI_ASSERT(it != m_nodes.getEnd());
		err = func(*it);

		++it;
	}

	return Error::kNone;
}
/// @}

} // end namespace anki
