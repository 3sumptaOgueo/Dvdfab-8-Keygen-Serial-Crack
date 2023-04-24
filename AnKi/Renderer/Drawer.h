// Copyright (C) 2009-2023, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Renderer/Common.h>
#include <AnKi/Resource/RenderingKey.h>
#include <AnKi/Gr.h>

namespace anki {

// Forward
class Renderer;
class RenderableQueueElement;

/// @addtogroup renderer
/// @{

/// @memberof RenderableDrawer.
class RenderableDrawerArguments
{
public:
	// The matrices are whatever the drawing needs. Sometimes they contain jittering and sometimes they don't.
	Mat3x4 m_viewMatrix;
	Mat3x4 m_cameraTransform;
	Mat4 m_viewProjectionMatrix;
	Mat4 m_previousViewProjectionMatrix;

	SamplerPtr m_sampler;
};

/// It uses the render queue to batch and render.
class RenderableDrawer
{
	friend class RenderTask;

public:
	RenderableDrawer() = default;

	~RenderableDrawer();

	void drawRange(const RenderableDrawerArguments& args, const RenderableQueueElement* begin, const RenderableQueueElement* end,
				   CommandBufferPtr& cmdb);

private:
	class Context;

	void flushDrawcall(Context& ctx);

	void drawSingle(const RenderableQueueElement* renderEl, Context& ctx);
};
/// @}

} // end namespace anki
