// Copyright (C) 2009-2023, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Importer/GltfImporter.h>

using namespace anki;

static const char* kUsage = R"(Usage: %s in_file out_dir [options]
Options:
-rpath <string>            : Replace all absolute paths of assets with that path
-texrpath <string>         : Same as rpath but for textures
-optimize-meshes <0|1>     : Optimize meshes. Default is 1
-optimize-animations <0|1> : Optimize animations. Default is 1
-j <thread_count>          : Number of threads. Defaults to system's max
-lod-count <1|2|3>         : The number of geometry LODs to generate. Default is 1
-lod-factor <float>        : The decimate factor for each LOD. Default 0.25
-light-scale <float>       : Multiply the light intensity with this number. Default is 1.0
-import-textures <0|1>     : Import textures. Default is 0
-v                         : Enable verbose log
)";

class CmdLineArgs
{
public:
	String m_inputFname;
	String m_outDir;
	String m_rpath;
	String m_texRpath;
	Bool m_optimizeMeshes = true;
	Bool m_optimizeAnimations = true;
	Bool m_importTextures = false;
	U32 m_threadCount = kMaxU32;
	U32 m_lodCount = 1;
	F32 m_lodFactor = 0.25f;
	F32 m_lightIntensityScale = 1.0f;
};

static Error parseCommandLineArgs(int argc, char** argv, CmdLineArgs& info)
{
	Bool rpathFound = false;
	Bool texrpathFound = false;

	// Parse config
	if(argc < 3)
	{
		return Error::kUserData;
	}

	info.m_inputFname = argv[1];
	info.m_outDir.sprintf("%s/", argv[2]);

	for(I i = 3; i < argc; i++)
	{
		if(strcmp(argv[i], "-texrpath") == 0)
		{
			texrpathFound = true;
			++i;

			if(i < argc)
			{
				if(std::strlen(argv[i]) > 0)
				{
					info.m_texRpath.sprintf("%s/", argv[i]);
				}
				else
				{
					info.m_texRpath = "";
				}
			}
			else
			{
				return Error::kUserData;
			}
		}
		else if(strcmp(argv[i], "-v") == 0)
		{
			Logger::getSingleton().enableVerbosity(true);
		}
		else if(strcmp(argv[i], "-rpath") == 0)
		{
			rpathFound = true;
			++i;

			if(i < argc)
			{
				if(std::strlen(argv[i]) > 0)
				{
					info.m_rpath.sprintf("%s/", argv[i]);
				}
				else
				{
					info.m_rpath = "";
				}
			}
			else
			{
				return Error::kUserData;
			}
		}
		else if(strcmp(argv[i], "-optimize-meshes") == 0)
		{
			++i;

			if(i < argc)
			{
				I optimize = 1;
				ANKI_CHECK(CString(argv[i]).toNumber(optimize));
				info.m_optimizeMeshes = optimize != 0;
			}
			else
			{
				return Error::kUserData;
			}
		}
		else if(strcmp(argv[i], "-j") == 0)
		{
			++i;

			if(i < argc)
			{
				U32 count = 0;
				ANKI_CHECK(CString(argv[i]).toNumber(count));
				info.m_threadCount = count;
			}
			else
			{
				return Error::kUserData;
			}
		}
		else if(strcmp(argv[i], "-lod-count") == 0)
		{
			++i;

			if(i < argc)
			{
				ANKI_CHECK(CString(argv[i]).toNumber(info.m_lodCount));
			}
			else
			{
				return Error::kUserData;
			}
		}
		else if(strcmp(argv[i], "-lod-factor") == 0)
		{
			++i;

			if(i < argc)
			{
				ANKI_CHECK(CString(argv[i]).toNumber(info.m_lodFactor));
			}
			else
			{
				return Error::kUserData;
			}
		}
		else if(strcmp(argv[i], "-light-scale") == 0)
		{
			++i;

			if(i < argc)
			{
				ANKI_CHECK(CString(argv[i]).toNumber(info.m_lightIntensityScale));
			}
			else
			{
				return Error::kUserData;
			}
		}
		else if(strcmp(argv[i], "-optimize-animations") == 0)
		{
			++i;

			if(i < argc)
			{
				I optimize = 1;
				ANKI_CHECK(CString(argv[i]).toNumber(optimize));
				info.m_optimizeAnimations = optimize != 0;
			}
			else
			{
				return Error::kUserData;
			}
		}
		else if(strcmp(argv[i], "-import-textures") == 0)
		{
			++i;

			if(i < argc)
			{
				I val = 1;
				ANKI_CHECK(CString(argv[i]).toNumber(val));
				info.m_importTextures = val != 0;
			}
			else
			{
				return Error::kUserData;
			}
		}
		else
		{
			return Error::kUserData;
		}
	}

	if(!rpathFound)
	{
		info.m_rpath = info.m_outDir;
	}

	if(!texrpathFound)
	{
		info.m_texRpath = info.m_rpath;
	}

	return Error::kNone;
}

ANKI_MAIN_FUNCTION(myMain)
int myMain(int argc, char** argv)
{
	class Cleanup
	{
	public:
		~Cleanup()
		{
			DefaultMemoryPool::freeSingleton();
			ImporterMemoryPool::freeSingleton();
		}
	} cleanup;

	DefaultMemoryPool::allocateSingleton(allocAligned, nullptr);
	ImporterMemoryPool::allocateSingleton(allocAligned, nullptr);

	CmdLineArgs cmdArgs;
	if(parseCommandLineArgs(argc, argv, cmdArgs))
	{
		ANKI_IMPORTER_LOGE(kUsage, argv[0]);
		return 1;
	}

	String comment;
	for(I32 i = 0; i < argc; ++i)
	{
		if(i != 0)
		{
			comment += " ";
		}

		if(CString(argv[i]).getLength())
		{
			comment += argv[i];
		}
		else
		{
			comment += "\"\"";
		}
	}

	GltfImporterInitInfo initInfo;
	initInfo.m_inputFilename = cmdArgs.m_inputFname;
	initInfo.m_outDirectory = cmdArgs.m_outDir;
	initInfo.m_rpath = cmdArgs.m_rpath;
	initInfo.m_texrpath = cmdArgs.m_texRpath;
	initInfo.m_optimizeMeshes = cmdArgs.m_optimizeMeshes;
	initInfo.m_optimizeAnimations = cmdArgs.m_optimizeAnimations;
	initInfo.m_lodFactor = cmdArgs.m_lodFactor;
	initInfo.m_lodCount = cmdArgs.m_lodCount;
	initInfo.m_lightIntensityScale = cmdArgs.m_lightIntensityScale;
	initInfo.m_threadCount = cmdArgs.m_threadCount;
	initInfo.m_comment = comment;
	initInfo.m_importTextures = cmdArgs.m_importTextures;

	GltfImporter importer;
	if(importer.init(initInfo))
	{
		return 1;
	}

	if(importer.writeAll())
	{
		return 1;
	}

	ANKI_IMPORTER_LOGI("File written: %s", cmdArgs.m_inputFname.cstr());

	return 0;
}
