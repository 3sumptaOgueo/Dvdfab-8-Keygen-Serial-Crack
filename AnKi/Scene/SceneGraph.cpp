// Copyright (C) 2009-2023, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Scene/SceneGraph.h>
#include <AnKi/Scene/Octree.h>
#include <AnKi/Scene/RenderStateBucket.h>
#include <AnKi/Scene/Components/CameraComponent.h>
#include <AnKi/Physics/PhysicsWorld.h>
#include <AnKi/Resource/ResourceManager.h>
#include <AnKi/Renderer/MainRenderer.h>
#include <AnKi/Core/ConfigSet.h>
#include <AnKi/Core/StatsSet.h>
#include <AnKi/Util/ThreadHive.h>
#include <AnKi/Util/Tracer.h>
#include <AnKi/Util/HighRezTimer.h>

namespace anki {

static StatCounter g_sceneUpdateTime(StatCategory::kTime, "All scene update", StatFlag::kMilisecond | StatFlag::kShowAverage);
static StatCounter g_sceneVisibilityTime(StatCategory::kTime, "Scene visibility", StatFlag::kMilisecond | StatFlag::kShowAverage);
static StatCounter g_scenePhysicsTime(StatCategory::kTime, "Physics", StatFlag::kMilisecond | StatFlag::kShowAverage);

static NumericCVar<U32> g_octreeMaxDepthCVar(CVarSubsystem::kScene, "OctreeMaxDepth", 5, 2, 10, "The max depth of the octree");

NumericCVar<F32> g_probeEffectiveDistanceCVar(CVarSubsystem::kScene, "ProbeEffectiveDistance", 256.0f, 1.0f, kMaxF32,
											  "How far various probes can render");
NumericCVar<F32> g_probeShadowEffectiveDistanceCVar(CVarSubsystem::kScene, "ProbeShadowEffectiveDistance", 32.0f, 1.0f, kMaxF32,
													"How far to render shadows for the various probes");

constexpr U32 kUpdateNodeBatchSize = 10;

class SceneGraph::UpdateSceneNodesCtx
{
public:
	SceneGraph* m_scene = nullptr;

	IntrusiveList<SceneNode>::Iterator m_crntNode;
	SpinLock m_crntNodeLock;

	Second m_prevUpdateTime;
	Second m_crntTime;
};

SceneGraph::SceneGraph()
{
}

SceneGraph::~SceneGraph()
{
	[[maybe_unused]] const Error err = iterateSceneNodes([&](SceneNode& s) -> Error {
		s.setMarkedForDeletion();
		return Error::kNone;
	});

	deleteNodesMarkedForDeletion();

	if(m_octree)
	{
		deleteInstance(SceneMemoryPool::getSingleton(), m_octree);
	}

	GpuSceneContiguousArrays::freeSingleton();
	RenderStateBucketContainer::freeSingleton();
}

Error SceneGraph::init(AllocAlignedCallback allocCallback, void* allocCallbackData)
{
	SceneMemoryPool::allocateSingleton(allocCallback, allocCallbackData);

	m_framePool.init(allocCallback, allocCallbackData, 1_MB, 2.0, 0, true, ANKI_SAFE_ALIGNMENT, "SceneGraphFramePool");

	m_octree = newInstance<Octree>(SceneMemoryPool::getSingleton());
	m_octree->init(m_sceneMin, m_sceneMax, g_octreeMaxDepthCVar.get());

	// Init the default main camera
	ANKI_CHECK(newSceneNode<SceneNode>("mainCamera", m_defaultMainCam));
	CameraComponent* camc = m_defaultMainCam->newComponent<CameraComponent>();
	camc->setPerspective(0.1f, 1000.0f, toRad(60.0f), (1080.0f / 1920.0f) * toRad(60.0f));
	m_mainCam = m_defaultMainCam;

	GpuSceneContiguousArrays::allocateSingleton();
	RenderStateBucketContainer::allocateSingleton();

	return Error::kNone;
}

Error SceneGraph::registerNode(SceneNode* node)
{
	ANKI_ASSERT(node);

	// Add to dict if it has a name
	if(node->getName())
	{
		if(tryFindSceneNode(node->getName()))
		{
			ANKI_SCENE_LOGE("Node with the same name already exists");
			return Error::kUserData;
		}

		m_nodesDict.emplace(node->getName(), node);
	}

	// Add to vector
	m_nodes.pushBack(node);
	++m_nodesCount;

	return Error::kNone;
}

void SceneGraph::unregisterNode(SceneNode* node)
{
	// Remove from the graph
	m_nodes.erase(node);
	--m_nodesCount;

	if(m_mainCam != m_defaultMainCam && m_mainCam == node)
	{
		m_mainCam = m_defaultMainCam;
	}

	// Remove from dict
	if(node->getName())
	{
		auto it = m_nodesDict.find(node->getName());
		ANKI_ASSERT(it != m_nodesDict.getEnd());
		m_nodesDict.erase(it);
	}
}

SceneNode& SceneGraph::findSceneNode(const CString& name)
{
	SceneNode* node = tryFindSceneNode(name);
	ANKI_ASSERT(node);
	return *node;
}

SceneNode* SceneGraph::tryFindSceneNode(const CString& name)
{
	auto it = m_nodesDict.find(name);
	return (it == m_nodesDict.getEnd()) ? nullptr : (*it);
}

void SceneGraph::deleteNodesMarkedForDeletion()
{
	/// Delete all nodes pending deletion. At this point all scene threads
	/// should have finished their tasks
	while(m_objectsMarkedForDeletionCount.load() > 0)
	{
		[[maybe_unused]] Bool found = false;
		auto it = m_nodes.begin();
		auto end = m_nodes.end();
		for(; it != end; ++it)
		{
			SceneNode& node = *it;

			if(node.getMarkedForDeletion())
			{
				// Delete node
				unregisterNode(&node);
				deleteInstance(SceneMemoryPool::getSingleton(), &node);
				m_objectsMarkedForDeletionCount.fetchSub(1);
				found = true;
				break;
			}
		}

		ANKI_ASSERT(found && "Something is wrong with marked for deletion");
	}
}

Error SceneGraph::update(Second prevUpdateTime, Second crntTime)
{
	ANKI_ASSERT(m_mainCam);
	ANKI_TRACE_SCOPED_EVENT(SceneUpdate);

	GpuSceneContiguousArrays::getSingleton().endFrame();

	const Second startUpdateTime = HighRezTimer::getCurrentTime();

	// Reset the framepool
	m_framePool.reset();

	// Delete stuff
	{
		ANKI_TRACE_SCOPED_EVENT(SceneRemoveMarkedForDeletion);
		const Bool fullCleanup = m_objectsMarkedForDeletionCount.load() != 0;
		m_events.deleteEventsMarkedForDeletion(fullCleanup);
		deleteNodesMarkedForDeletion();
	}

	// Update
	{
		ANKI_TRACE_SCOPED_EVENT(ScenePhysics);
		const Second physicsUpdate = HighRezTimer::getCurrentTime();

		PhysicsWorld::getSingleton().update(crntTime - prevUpdateTime);

		g_scenePhysicsTime.set((HighRezTimer::getCurrentTime() - physicsUpdate) * 1000.0);
	}

	{
		ANKI_TRACE_SCOPED_EVENT(SceneNodesUpdate);
		ANKI_CHECK(m_events.updateAllEvents(prevUpdateTime, crntTime));

		// Then the rest
		Array<ThreadHiveTask, ThreadHive::kMaxThreads> tasks;
		UpdateSceneNodesCtx updateCtx;
		updateCtx.m_scene = this;
		updateCtx.m_crntNode = m_nodes.getBegin();
		updateCtx.m_prevUpdateTime = prevUpdateTime;
		updateCtx.m_crntTime = crntTime;

		for(U i = 0; i < CoreThreadHive::getSingleton().getThreadCount(); i++)
		{
			tasks[i] = ANKI_THREAD_HIVE_TASK(
				{
					if(self->m_scene->updateNodes(*self))
					{
						ANKI_SCENE_LOGF("Will not recover");
					}
				},
				&updateCtx, nullptr, nullptr);
		}

		CoreThreadHive::getSingleton().submitTasks(&tasks[0], CoreThreadHive::getSingleton().getThreadCount());
		CoreThreadHive::getSingleton().waitAllTasks();
	}

	g_sceneUpdateTime.set((HighRezTimer::getCurrentTime() - startUpdateTime) * 1000.0);
	return Error::kNone;
}

void SceneGraph::doVisibilityTests(RenderQueue& rqueue)
{
	const Second startTime = HighRezTimer::getCurrentTime();
	doVisibilityTests(*m_mainCam, *this, rqueue);
	g_sceneVisibilityTime.set((HighRezTimer::getCurrentTime() - startTime) * 1000.0);
}

Error SceneGraph::updateNode(Second prevTime, Second crntTime, SceneNode& node)
{
	ANKI_TRACE_INC_COUNTER(SceneNodeUpdated, 1);

	Error err = Error::kNone;

	// Components update
	SceneComponentUpdateInfo componentUpdateInfo(prevTime, crntTime);
	componentUpdateInfo.m_framePool = &m_framePool;

	Bool atLeastOneComponentUpdated = false;
	node.iterateComponents([&](SceneComponent& comp) {
		if(err)
		{
			return;
		}

		componentUpdateInfo.m_node = &node;
		Bool updated = false;
		err = comp.updateReal(componentUpdateInfo, updated);

		if(updated)
		{
			ANKI_TRACE_INC_COUNTER(SceneComponentUpdated, 1);
			comp.setTimestamp(GlobalFrameIndex::getSingleton().m_value);
			atLeastOneComponentUpdated = true;
		}
	});

	// Update children
	if(!err)
	{
		err = node.visitChildrenMaxDepth(0, [&](SceneNode& child) -> Error {
			return updateNode(prevTime, crntTime, child);
		});
	}

	// Frame update
	if(!err)
	{
		if(atLeastOneComponentUpdated)
		{
			node.setComponentMaxTimestamp(GlobalFrameIndex::getSingleton().m_value);
		}
		else
		{
			// No components or nothing updated, don't change the timestamp
		}

		err = node.frameUpdate(prevTime, crntTime);
	}

	return err;
}

Error SceneGraph::updateNodes(UpdateSceneNodesCtx& ctx)
{
	ANKI_TRACE_SCOPED_EVENT(SceneNodeUpdate);

	IntrusiveList<SceneNode>::ConstIterator end = m_nodes.getEnd();

	Bool quit = false;
	Error err = Error::kNone;
	while(!quit && !err)
	{
		// Fetch a batch of scene nodes that don't have parent
		Array<SceneNode*, kUpdateNodeBatchSize> batch;
		U batchSize = 0;

		{
			LockGuard<SpinLock> lock(ctx.m_crntNodeLock);

			while(1)
			{
				if(batchSize == batch.getSize())
				{
					break;
				}

				if(ctx.m_crntNode == end)
				{
					quit = true;
					break;
				}

				SceneNode& node = *ctx.m_crntNode;
				if(node.getParent() == nullptr)
				{
					batch[batchSize++] = &node;
				}

				++ctx.m_crntNode;
			}
		}

		// Process nodes
		for(U i = 0; i < batchSize && !err; ++i)
		{
			err = updateNode(ctx.m_prevUpdateTime, ctx.m_crntTime, *batch[i]);
		}
	}

	return err;
}

} // end namespace anki
