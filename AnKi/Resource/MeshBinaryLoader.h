// Copyright (C) 2009-2023, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Resource/MeshBinary.h>
#include <AnKi/Resource/ResourceFilesystem.h>
#include <AnKi/Resource/MeshBinary.h>
#include <AnKi/Util/WeakArray.h>
#include <AnKi/Shaders/Include/MeshTypes.h>

namespace anki {

/// @addtogroup resource
/// @{

/// This class loads the mesh binary file. It only supports a subset of combinations of vertex formats and buffers.
/// The file is layed out in memory:
/// * Header
/// * Submeshes
/// * LOD of max LOD
/// ** Index buffer of all sub meshes
/// ** Vertex buffer #0 of all sub meshes
/// ** ...
/// ** Meshlets of all sub meshes
/// ** Local index buffer all sub meshes
/// * LOD of max-1 LOD
/// ...
class MeshBinaryLoader
{
public:
	MeshBinaryLoader(BaseMemoryPool* pool)
		: m_subMeshes(pool)
	{
		ANKI_ASSERT(pool);
	}

	~MeshBinaryLoader() = default;

	Error load(const ResourceFilename& filename);

	Error storeIndexBuffer(U32 lod, void* ptr, PtrSize size);

	Error storeVertexBuffer(U32 lod, U32 bufferIdx, void* ptr, PtrSize size);

	/// Instead of calling storeIndexBuffer and storeVertexBuffer use this method to get those buffers into the CPU.
	Error storeIndicesAndPosition(U32 lod, ResourceDynamicArray<U32>& indices, ResourceDynamicArray<Vec3>& positions);

	const MeshBinaryHeader& getHeader() const
	{
		ANKI_ASSERT(isLoaded());
		return m_header;
	}

	ConstWeakArray<MeshBinarySubMesh> getSubMeshes() const
	{
		return ConstWeakArray<MeshBinarySubMesh>(m_subMeshes);
	}

private:
	ResourceFilePtr m_file;

	MeshBinaryHeader m_header;

	DynamicArray<MeshBinarySubMesh, MemoryPoolPtrWrapper<BaseMemoryPool>> m_subMeshes;

	Bool isLoaded() const
	{
		return m_file.get() != nullptr;
	}

	PtrSize getIndexBufferSize(U32 lod) const
	{
		ANKI_ASSERT(isLoaded());
		ANKI_ASSERT(lod < m_header.m_lodCount);
		return PtrSize(m_header.m_indexCounts[lod]) * getIndexSize(m_header.m_indexType);
	}

	PtrSize getMeshletsBufferSize(U32 lod) const
	{
		ANKI_ASSERT(isLoaded());
		ANKI_ASSERT(lod < m_header.m_lodCount);
		return PtrSize(m_header.m_meshletCounts[lod]) * sizeof(MeshBinaryMeshlet);
	}

	PtrSize getVertexBufferSize(U32 lod, U32 bufferIdx) const
	{
		ANKI_ASSERT(isLoaded());
		ANKI_ASSERT(lod < m_header.m_lodCount);
		return PtrSize(m_header.m_vertexCounts[lod]) * PtrSize(m_header.m_vertexBuffers[bufferIdx].m_vertexStride);
	}

	PtrSize getMeshletPrimitivesBufferSize(U32 lod) const
	{
		ANKI_ASSERT(isLoaded());
		ANKI_ASSERT(lod < m_header.m_lodCount);
		return PtrSize(m_header.m_meshletPrimitiveCounts[lod]) * sizeof(U8Vec4);
	}

	PtrSize getLodBuffersSize(U32 lod) const;

	Error checkHeader() const;
	Error checkFormat(VertexStreamId stream, Bool isOptional, Bool canBeTransformed) const;
	Error loadSubmeshes();
};
/// @}

} // end namespace anki
