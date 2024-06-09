// Copyright (C) 2009-present, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <Tests/Framework/Framework.h>
#include <AnKi/Util/Tracer.h>
#include <AnKi/Core/CoreTracer.h>
#include <AnKi/Util/HighRezTimer.h>

#if ANKI_TRACING_ENABLED
ANKI_TEST(Util, Tracer)
{
	HeapMemoryPool alloc(allocAligned, nullptr);
	ANKI_TEST_EXPECT_NO_ERR(CoreTracer::allocateSingleton().init("./"));
	Tracer::getSingleton().setEnabled(true);

	// 1st frame
	CoreTracer::getSingleton().flushFrame(0);

	// 2nd frame
	// 2 events
	{
		ANKI_TRACE_SCOPED_EVENT(EVENT);
		HighRezTimer::sleep(0.5);
	}

	{
		ANKI_TRACE_SCOPED_EVENT(EVENT);
		HighRezTimer::sleep(0.25);
	}

	CoreTracer::getSingleton().flushFrame(1);

	// 4rd frame
	// 2 different events & non zero counter
	{
		ANKI_TRACE_SCOPED_EVENT(EVENT);
		HighRezTimer::sleep(0.5);
	}

	{
		ANKI_TRACE_SCOPED_EVENT(EVENT2);
		HighRezTimer::sleep(0.25);
	}

	ANKI_TRACE_INC_COUNTER(COUNTER, 100);

	CoreTracer::getSingleton().flushFrame(3);

	// 5th frame
	ANKI_TRACE_INC_COUNTER(COUNTER, 150);
	CoreTracer::getSingleton().flushFrame(4);

	CoreTracer::freeSingleton();
}
#endif
