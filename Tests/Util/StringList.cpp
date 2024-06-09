// Copyright (C) 2009-present, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <Tests/Framework/Framework.h>
#include <AnKi/Util/StringList.h>

ANKI_TEST(Util, StringList)
{
	// Test splitString
	{
		CString toSplit = "foo\n\nboo\n";

		StringList list;
		list.splitString(toSplit, '\n');

		ANKI_TEST_EXPECT_EQ(list.getSize(), 2);
		auto it = list.getBegin();
		ANKI_TEST_EXPECT_EQ(*it, "foo");
		++it;
		ANKI_TEST_EXPECT_EQ(*it, "boo");

		// Again
		list.destroy();
		list.splitString(toSplit, '\n', true);
		ANKI_TEST_EXPECT_EQ(list.getSize(), 3);
		it = list.getBegin();
		ANKI_TEST_EXPECT_EQ(*it, "foo");
		++it;
		ANKI_TEST_EXPECT_EQ(it->isEmpty(), true);
		++it;
		ANKI_TEST_EXPECT_EQ(*it, "boo");
	}
}
