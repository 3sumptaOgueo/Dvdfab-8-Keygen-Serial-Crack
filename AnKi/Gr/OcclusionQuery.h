// Copyright (C) 2009-present, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Gr/GrObject.h>

namespace anki {

/// @addtogroup graphics
/// @{

/// Occlusion query.
class OcclusionQuery : public GrObject
{
	ANKI_GR_OBJECT

public:
	static constexpr GrObjectType kClassType = GrObjectType::kOcclusionQuery;

	/// Get the occlusion query result. It won't block.
	OcclusionQueryResult getResult() const;

protected:
	/// Construct.
	OcclusionQuery(CString name)
		: GrObject(kClassType, name)
	{
	}

	/// Destroy.
	~OcclusionQuery()
	{
	}

private:
	/// Allocate and initialize a new instance.
	[[nodiscard]] static OcclusionQuery* newInstance();
};
/// @}

} // end namespace anki
