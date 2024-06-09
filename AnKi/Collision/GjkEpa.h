// Copyright (C) 2009-present, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Collision/Common.h>
#include <AnKi/Math.h>

namespace anki {

/// @addtogroup collision
/// @{

using GjkSupportCallback = Vec4 (*)(const void* shape, const Vec4& dir);

/// Return true if the two convex shapes intersect.
Bool gjkIntersection(const void* shape0, GjkSupportCallback shape0Callback, const void* shape1, GjkSupportCallback shape1Callback);
/// @}

} // end namespace anki
