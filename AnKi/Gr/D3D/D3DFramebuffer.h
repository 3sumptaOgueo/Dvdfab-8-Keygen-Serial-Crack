// Copyright (C) 2009-2023, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Gr/Framebuffer.h>

namespace anki {

/// @addtogroup directx
/// @{

/// Framebuffer implementation.
class FramebufferImpl final : public Framebuffer
{
public:
	FramebufferImpl(CString name)
		: Framebuffer(name)
	{
	}

	~FramebufferImpl();

	Error init(const FramebufferInitInfo& init);
};
/// @}

} // end namespace anki
