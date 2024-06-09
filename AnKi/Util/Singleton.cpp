// Copyright (C) 2009-present, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Util/Singleton.h>

namespace anki {

#if ANKI_ASSERTIONS_ENABLED
I32 g_singletonsAllocated = 0;

class SingletonsAllocatedChecker
{
public:
	SingletonsAllocatedChecker()
	{
		g_singletonsAllocated = 0;
	}

	~SingletonsAllocatedChecker()
	{
		ANKI_ASSERT(g_singletonsAllocated == 0);
	}
};

SingletonsAllocatedChecker g_singletonsAllocatedChecker;
#endif

} // end namespace anki
