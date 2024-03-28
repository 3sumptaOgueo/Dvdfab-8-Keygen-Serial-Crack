// Copyright (C) 2009-2023, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Gr/Fence.h>
#include <AnKi/Gr/Vulkan/VkFence.h>
#include <AnKi/Gr/GrManager.h>

namespace anki {

Fence* Fence::newInstance()
{
	return anki::newInstance<FenceImpl>(GrMemoryPool::getSingleton(), "N/A");
}

Bool Fence::clientWait(Second seconds)
{
	return static_cast<FenceImpl*>(this)->m_semaphore->clientWait(seconds);
}

} // end namespace anki
