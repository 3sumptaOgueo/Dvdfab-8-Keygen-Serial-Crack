// Copyright (C) 2009-2023, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Renderer/DebugDrawer.h>
#include <AnKi/Resource/ResourceManager.h>
#include <AnKi/Renderer/RenderQueue.h>
#include <AnKi/Core/GpuMemory/RebarTransientMemoryPool.h>
#include <AnKi/Physics/PhysicsWorld.h>
#include <AnKi/Gr/Buffer.h>

namespace anki {

void allocateAndPopulateDebugBox(RebarAllocation& vertsToken, RebarAllocation& indicesToken, U32& indexCount)
{
	Vec3* verts =
		static_cast<Vec3*>(RebarTransientMemoryPool::getSingleton().allocateFrame(sizeof(Vec3) * 8, vertsToken));

	constexpr F32 kSize = 1.0f;
	verts[0] = Vec3(kSize, kSize, kSize); // front top right
	verts[1] = Vec3(-kSize, kSize, kSize); // front top left
	verts[2] = Vec3(-kSize, -kSize, kSize); // front bottom left
	verts[3] = Vec3(kSize, -kSize, kSize); // front bottom right
	verts[4] = Vec3(kSize, kSize, -kSize); // back top right
	verts[5] = Vec3(-kSize, kSize, -kSize); // back top left
	verts[6] = Vec3(-kSize, -kSize, -kSize); // back bottom left
	verts[7] = Vec3(kSize, -kSize, -kSize); // back bottom right

	constexpr U kIndexCount = 12 * 2;
	U16* indices = static_cast<U16*>(
		RebarTransientMemoryPool::getSingleton().allocateFrame(sizeof(U16) * kIndexCount, indicesToken));

	U c = 0;
	indices[c++] = 0;
	indices[c++] = 1;
	indices[c++] = 1;
	indices[c++] = 2;
	indices[c++] = 2;
	indices[c++] = 3;
	indices[c++] = 3;
	indices[c++] = 0;

	indices[c++] = 4;
	indices[c++] = 5;
	indices[c++] = 5;
	indices[c++] = 6;
	indices[c++] = 6;
	indices[c++] = 7;
	indices[c++] = 7;
	indices[c++] = 4;

	indices[c++] = 0;
	indices[c++] = 4;
	indices[c++] = 1;
	indices[c++] = 5;
	indices[c++] = 2;
	indices[c++] = 6;
	indices[c++] = 3;
	indices[c++] = 7;

	ANKI_ASSERT(c == kIndexCount);
	indexCount = kIndexCount;
}

Error DebugDrawer2::init()
{
	ANKI_CHECK(ResourceManager::getSingleton().loadResource("ShaderBinaries/SceneDebug.ankiprogbin", m_prog));

	{
		BufferInitInfo bufferInit("DebugCube");
		bufferInit.m_usage = BufferUsageBit::kVertex;
		bufferInit.m_size = sizeof(Vec3) * 8;
		bufferInit.m_mapAccess = BufferMapAccessBit::kWrite;
		m_cubePositionsBuffer = GrManager::getSingleton().newBuffer(bufferInit);

		Vec3* verts = static_cast<Vec3*>(m_cubePositionsBuffer->map(0, kMaxPtrSize, BufferMapAccessBit::kWrite));

		const F32 size = 1.0f;
		verts[0] = Vec3(size, size, size); // front top right
		verts[1] = Vec3(-size, size, size); // front top left
		verts[2] = Vec3(-size, -size, size); // front bottom left
		verts[3] = Vec3(size, -size, size); // front bottom right
		verts[4] = Vec3(size, size, -size); // back top right
		verts[5] = Vec3(-size, size, -size); // back top left
		verts[6] = Vec3(-size, -size, -size); // back bottom left
		verts[7] = Vec3(size, -size, -size); // back bottom right

		m_cubePositionsBuffer->flush(0, kMaxPtrSize);
		m_cubePositionsBuffer->unmap();
	}

	{
		constexpr U kIndexCount = 12 * 2;

		BufferInitInfo bufferInit("DebugCube");
		bufferInit.m_usage = BufferUsageBit::kVertex;
		bufferInit.m_size = sizeof(U16) * kIndexCount;
		bufferInit.m_mapAccess = BufferMapAccessBit::kWrite;
		m_cubeIndicesBuffer = GrManager::getSingleton().newBuffer(bufferInit);

		U16* indices = static_cast<U16*>(m_cubeIndicesBuffer->map(0, kMaxPtrSize, BufferMapAccessBit::kWrite));

		U32 indexCount = 0;
		indices[indexCount++] = 0;
		indices[indexCount++] = 1;
		indices[indexCount++] = 1;
		indices[indexCount++] = 2;
		indices[indexCount++] = 2;
		indices[indexCount++] = 3;
		indices[indexCount++] = 3;
		indices[indexCount++] = 0;

		indices[indexCount++] = 4;
		indices[indexCount++] = 5;
		indices[indexCount++] = 5;
		indices[indexCount++] = 6;
		indices[indexCount++] = 6;
		indices[indexCount++] = 7;
		indices[indexCount++] = 7;
		indices[indexCount++] = 4;

		indices[indexCount++] = 0;
		indices[indexCount++] = 4;
		indices[indexCount++] = 1;
		indices[indexCount++] = 5;
		indices[indexCount++] = 2;
		indices[indexCount++] = 6;
		indices[indexCount++] = 3;
		indices[indexCount++] = 7;

		m_cubeIndicesBuffer->flush(0, kMaxPtrSize);
		m_cubeIndicesBuffer->unmap();
	}

	return Error::kNone;
}

void DebugDrawer2::drawCubes(ConstWeakArray<Mat4> mvps, const Vec4& color, F32 lineSize, Bool ditherFailedDepth,
							 F32 cubeSideSize, CommandBufferPtr& cmdb) const
{
	// Set the uniforms
	RebarAllocation unisToken;
	Mat4* pmvps = static_cast<Mat4*>(
		RebarTransientMemoryPool::getSingleton().allocateFrame(sizeof(Mat4) * mvps.getSize(), unisToken));

	if(cubeSideSize == 2.0f)
	{
		memcpy(pmvps, &mvps[0], mvps.getSizeInBytes());
	}
	else
	{
		ANKI_ASSERT(!"TODO");
	}

	// Setup state
	ShaderProgramResourceVariantInitInfo variantInitInfo(m_prog);
	variantInitInfo.addMutation("COLOR_TEXTURE", 0);
	variantInitInfo.addMutation("DITHERED_DEPTH_TEST", U32(ditherFailedDepth != 0));
	const ShaderProgramResourceVariant* variant;
	m_prog->getOrCreateVariant(variantInitInfo, variant);
	cmdb->bindShaderProgram(variant->getProgram());

	cmdb->setPushConstants(&color, sizeof(color));

	cmdb->setVertexAttribute(0, 0, Format::kR32G32B32_Sfloat, 0);
	cmdb->bindVertexBuffer(0, m_cubePositionsBuffer, 0, sizeof(Vec3));
	cmdb->bindIndexBuffer(m_cubeIndicesBuffer, 0, IndexType::kU16);

	cmdb->bindStorageBuffer(0, 0, RebarTransientMemoryPool::getSingleton().getBuffer(), unisToken.m_offset,
							unisToken.m_range);

	cmdb->setLineWidth(lineSize);
	constexpr U kIndexCount = 12 * 2;
	cmdb->drawElements(PrimitiveTopology::kLines, kIndexCount, mvps.getSize());
}

void DebugDrawer2::drawLines(ConstWeakArray<Mat4> mvps, const Vec4& color, F32 lineSize, Bool ditherFailedDepth,
							 ConstWeakArray<Vec3> linePositions, CommandBufferPtr& cmdb) const
{
	ANKI_ASSERT(mvps.getSize() > 0);
	ANKI_ASSERT(linePositions.getSize() > 0 && (linePositions.getSize() % 2) == 0);

	// Verts
	RebarAllocation vertsToken;
	Vec3* verts = static_cast<Vec3*>(
		RebarTransientMemoryPool::getSingleton().allocateFrame(sizeof(Vec3) * linePositions.getSize(), vertsToken));
	memcpy(verts, linePositions.getBegin(), linePositions.getSizeInBytes());

	// Set the uniforms
	RebarAllocation unisToken;
	Mat4* pmvps = static_cast<Mat4*>(
		RebarTransientMemoryPool::getSingleton().allocateFrame(sizeof(Mat4) * mvps.getSize(), unisToken));
	memcpy(pmvps, &mvps[0], mvps.getSizeInBytes());

	// Setup state
	ShaderProgramResourceVariantInitInfo variantInitInfo(m_prog);
	variantInitInfo.addMutation("COLOR_TEXTURE", 0);
	variantInitInfo.addMutation("DITHERED_DEPTH_TEST", U32(ditherFailedDepth != 0));
	const ShaderProgramResourceVariant* variant;
	m_prog->getOrCreateVariant(variantInitInfo, variant);
	cmdb->bindShaderProgram(variant->getProgram());

	cmdb->setPushConstants(&color, sizeof(color));

	cmdb->setVertexAttribute(0, 0, Format::kR32G32B32_Sfloat, 0);
	cmdb->bindVertexBuffer(0, RebarTransientMemoryPool::getSingleton().getBuffer(), vertsToken.m_offset, sizeof(Vec3));

	cmdb->bindStorageBuffer(0, 0, RebarTransientMemoryPool::getSingleton().getBuffer(), unisToken.m_offset,
							unisToken.m_range);

	cmdb->setLineWidth(lineSize);
	cmdb->drawArrays(PrimitiveTopology::kLines, linePositions.getSize(), mvps.getSize());
}

void DebugDrawer2::drawBillboardTextures(const Mat4& projMat, const Mat3x4& viewMat, ConstWeakArray<Vec3> positions,
										 const Vec4& color, Bool ditherFailedDepth, TextureViewPtr tex,
										 SamplerPtr sampler, Vec2 billboardSize, CommandBufferPtr& cmdb) const
{
	RebarAllocation positionsToken;
	Vec3* verts =
		static_cast<Vec3*>(RebarTransientMemoryPool::getSingleton().allocateFrame(sizeof(Vec3) * 4, positionsToken));

	verts[0] = Vec3(-0.5f, -0.5f, 0.0f);
	verts[1] = Vec3(+0.5f, -0.5f, 0.0f);
	verts[2] = Vec3(-0.5f, +0.5f, 0.0f);
	verts[3] = Vec3(+0.5f, +0.5f, 0.0f);

	RebarAllocation uvsToken;
	Vec2* uvs = static_cast<Vec2*>(RebarTransientMemoryPool::getSingleton().allocateFrame(sizeof(Vec2) * 4, uvsToken));

	uvs[0] = Vec2(0.0f, 0.0f);
	uvs[1] = Vec2(1.0f, 0.0f);
	uvs[2] = Vec2(0.0f, 1.0f);
	uvs[3] = Vec2(1.0f, 1.0f);

	// Set the uniforms
	RebarAllocation unisToken;
	Mat4* pmvps = static_cast<Mat4*>(
		RebarTransientMemoryPool::getSingleton().allocateFrame(sizeof(Mat4) * positions.getSize(), unisToken));

	const Mat4 camTrf = Mat4(viewMat, Vec4(0.0f, 0.0f, 0.0f, 1.0f)).getInverse();
	const Vec3 zAxis = camTrf.getZAxis().xyz().getNormalized();
	Vec3 yAxis = Vec3(0.0f, 1.0f, 0.0f);
	const Vec3 xAxis = yAxis.cross(zAxis).getNormalized();
	yAxis = zAxis.cross(xAxis).getNormalized();
	Mat3 rot;
	rot.setColumns(xAxis, yAxis, zAxis);

	for(const Vec3& pos : positions)
	{
		Mat3 scale = Mat3::getIdentity();
		scale(0, 0) *= billboardSize.x();
		scale(1, 1) *= billboardSize.y();

		*pmvps = projMat * Mat4(viewMat, Vec4(0.0f, 0.0f, 0.0f, 1.0f)) * Mat4(pos.xyz1(), rot * scale, 1.0f);
		++pmvps;
	}

	Vec4* pcolor = reinterpret_cast<Vec4*>(pmvps);
	*pcolor = color;

	// Setup state
	ShaderProgramResourceVariantInitInfo variantInitInfo(m_prog);
	variantInitInfo.addMutation("COLOR_TEXTURE", 1);
	variantInitInfo.addMutation("DITHERED_DEPTH_TEST", U32(ditherFailedDepth != 0));
	const ShaderProgramResourceVariant* variant;
	m_prog->getOrCreateVariant(variantInitInfo, variant);
	cmdb->bindShaderProgram(variant->getProgram());

	cmdb->setPushConstants(&color, sizeof(color));

	cmdb->setVertexAttribute(0, 0, Format::kR32G32B32_Sfloat, 0);
	cmdb->setVertexAttribute(1, 1, Format::kR32G32_Sfloat, 0);
	cmdb->bindVertexBuffer(0, RebarTransientMemoryPool::getSingleton().getBuffer(), positionsToken.m_offset,
						   sizeof(Vec3));
	cmdb->bindVertexBuffer(1, RebarTransientMemoryPool::getSingleton().getBuffer(), uvsToken.m_offset, sizeof(Vec2));

	cmdb->bindStorageBuffer(0, 0, RebarTransientMemoryPool::getSingleton().getBuffer(), unisToken.m_offset,
							unisToken.m_range);
	cmdb->bindSampler(0, 3, sampler);
	cmdb->bindTexture(0, 4, tex);

	cmdb->drawArrays(PrimitiveTopology::kTriangleStrip, 4, positions.getSize());
}

void PhysicsDebugDrawer::drawLines(const Vec3* lines, const U32 vertCount, const Vec4& color)
{
	if(color != m_currentColor)
	{
		// Color have changed, flush and change the color
		flush();
		m_currentColor = color;
	}

	for(U32 i = 0; i < vertCount; ++i)
	{
		if(m_vertCount == m_vertCache.getSize())
		{
			flush();
		}

		m_vertCache[m_vertCount++] = lines[i];
	}
}

void PhysicsDebugDrawer::flush()
{
	if(m_vertCount > 0)
	{
		m_dbg->drawLines(ConstWeakArray<Mat4>(&m_mvp, 1), m_currentColor, 2.0f, false,
						 ConstWeakArray<Vec3>(&m_vertCache[0], m_vertCount), m_cmdb);

		m_vertCount = 0;
	}
}

} // end namespace anki
