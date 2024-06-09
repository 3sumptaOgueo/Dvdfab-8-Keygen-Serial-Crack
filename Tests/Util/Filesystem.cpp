// Copyright (C) 2009-present, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <Tests/Framework/Framework.h>
#include <AnKi/Util/Filesystem.h>
#include <AnKi/Util/File.h>

ANKI_TEST(Util, FileExists)
{
	// Create file
	File file;
	ANKI_TEST_EXPECT_NO_ERR(file.open("./tmp", FileOpenFlag::kWrite));
	file.close();

	// Check
	ANKI_TEST_EXPECT_EQ(fileExists("./tmp"), true);
}

ANKI_TEST(Util, Directory)
{
	// Destroy previous
	if(directoryExists("./dir"))
	{
		ANKI_TEST_EXPECT_NO_ERR(removeDirectory("./dir"));
	}

	// Create simple directory
	ANKI_TEST_EXPECT_NO_ERR(createDirectory("./dir"));
	File file;
	ANKI_TEST_EXPECT_NO_ERR(file.open("./dir/tmp", FileOpenFlag::kWrite));
	file.close();
	ANKI_TEST_EXPECT_EQ(fileExists("./dir/tmp"), true);

	ANKI_TEST_EXPECT_NO_ERR(removeDirectory("./dir"));
	ANKI_TEST_EXPECT_EQ(fileExists("./dir/tmp"), false);
	ANKI_TEST_EXPECT_EQ(directoryExists("./dir"), false);

	// A bit more complex
	ANKI_TEST_EXPECT_NO_ERR(createDirectory("./dir"));
	ANKI_TEST_EXPECT_NO_ERR(createDirectory("./dir/rid"));
	ANKI_TEST_EXPECT_NO_ERR(file.open("./dir/rid/tmp", FileOpenFlag::kWrite));
	file.close();
	ANKI_TEST_EXPECT_EQ(fileExists("./dir/rid/tmp"), true);

	ANKI_TEST_EXPECT_NO_ERR(removeDirectory("./dir"));
	ANKI_TEST_EXPECT_EQ(fileExists("./dir/rid/tmp"), false);
	ANKI_TEST_EXPECT_EQ(directoryExists("./dir/rid"), false);
	ANKI_TEST_EXPECT_EQ(directoryExists("./dir"), false);
}

ANKI_TEST(Util, HomeDir)
{
	String out;

	ANKI_TEST_EXPECT_NO_ERR(getHomeDirectory(out));
	printf("home dir %s\n", &out[0]);
	ANKI_TEST_EXPECT_GT(out.getLength(), 0);
}

ANKI_TEST(Util, WalkDir)
{
	class Path
	{
	public:
		CString m_path;
		Bool m_isDir;
	};

	class Ctx
	{
	public:
		Array<Path, 4> m_paths = {{{"./data", true}, {"./data/dir", true}, {"./data/file1", false}, {"./data/dir/file2", false}}};
		U32 m_foundMask = 0;
	} ctx;

	[[maybe_unused]] const Error err = removeDirectory("./data");

	// Create some dirs and some files
	for(U32 i = 0; i < ctx.m_paths.getSize(); ++i)
	{
		if(ctx.m_paths[i].m_isDir)
		{
			ANKI_TEST_EXPECT_NO_ERR(createDirectory(ctx.m_paths[i].m_path));
		}
		else
		{
			File file;
			ANKI_TEST_EXPECT_NO_ERR(file.open(ctx.m_paths[i].m_path, FileOpenFlag::kWrite));
		}
	}

	// Walk crnt dir
	ANKI_TEST_EXPECT_NO_ERR(walkDirectoryTree("./data", [&](const CString& fname, Bool isDir) -> Error {
		for(U32 i = 0; i < ctx.m_paths.getSize(); ++i)
		{
			String p;
			p.sprintf("./data/%s", fname.cstr());
			if(ctx.m_paths[i].m_path == p)
			{
				ANKI_TEST_EXPECT_EQ(ctx.m_paths[i].m_isDir, isDir);
				ctx.m_foundMask |= 1 << i;
			}
		}

		return Error::kNone;
	}));

	ANKI_TEST_EXPECT_EQ(ctx.m_foundMask, 0b1110);

	// Test error
	U32 count = 0;
	ANKI_TEST_EXPECT_ERR(walkDirectoryTree("./data///dir////",
										   [&count]([[maybe_unused]] const CString& fname, [[maybe_unused]] Bool isDir) -> Error {
											   ++count;
											   return Error::kFunctionFailed;
										   }),
						 Error::kFunctionFailed);

	ANKI_TEST_EXPECT_EQ(count, 1);
}
