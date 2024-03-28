// Copyright (C) 2009-2023, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Gr/TimestampQuery.h>
#include <AnKi/Gr/Vulkan/VkQueryFactory.h>

namespace anki {

/// @addtogroup vulkan
/// @{

/// Occlusion query.
class TimestampQueryImpl final : public TimestampQuery
{
	friend class TimestampQuery;

public:
	MicroQuery m_handle = {};

	TimestampQueryImpl(CString name)
		: TimestampQuery(name)
	{
	}

	~TimestampQueryImpl();

	/// Create the query.
	Error init();

	/// Get query result.
	TimestampQueryResult getResultInternal(Second& timestamp) const;

private:
	U64 m_timestampPeriod = 0;
};
/// @}

} // end namespace anki
