// Copyright (C) 2009-2023, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Renderer/Common.h>
#include <AnKi/Resource/RenderingKey.h>
#include <AnKi/Ui/Canvas.h>
#include <AnKi/Shaders/Include/ClusteredShadingTypes.h>
#include <AnKi/Shaders/Include/ModelTypes.h>

namespace anki {

/// @addtogroup renderer
/// @{

class RenderingMatrices
{
public:
	Mat3x4 m_cameraTransform;
	Mat3x4 m_viewMatrix;
	Mat4 m_projectionMatrix;
	Mat4 m_viewProjectionMatrix;
	Mat4 m_previousViewProjectionMatrix;
};

/// Render queue element that contains info on items that populate the G-buffer or the forward shading buffer etc.
class RenderableQueueElement final
{
public:
	U64 m_mergeKey;

	ShaderProgram* m_program;

	U32 m_worldTransformsOffset;
	U32 m_uniformsOffset;
	U32 m_meshLodOffset;
	U32 m_boneTransformsOffset;
	U32 m_particleEmitterOffset;
	U32 m_instanceCount;

	union
	{
		U32 m_indexCount;
		U32 m_vertexCount;
	};

	union
	{
		U32 m_firstIndex;
		U32 m_firstVertex;
	};

	F32 m_distanceFromCamera; ///< Don't set this. Visibility will.

	Vec3 m_aabbMin;
	Vec3 m_aabbMax;

	Bool m_indexed;
	PrimitiveTopology m_primitiveTopology;

	RenderableQueueElement()
	{
	}

	void computeMergeKey()
	{
		Array<U64, 5> toHash;
		toHash[0] = ptrToNumber(m_program);
		toHash[1] = m_indexed;
		toHash[2] = m_indexCount;
		toHash[3] = m_firstIndex;
		toHash[4] = U64(m_primitiveTopology);
		m_mergeKey = computeHash(toHash.getBegin(), toHash.getSizeInBytes());
	}

	Bool canMergeWith(const RenderableQueueElement& b) const
	{
		return m_mergeKey != 0 && m_mergeKey == b.m_mergeKey;
	}
};
static_assert(std::is_trivially_destructible<RenderableQueueElement>::value == true);

/// Context that contains variables for the GenericGpuComputeJobQueueElement.
class GenericGpuComputeJobQueueElementContext final : public RenderingMatrices
{
public:
	CommandBufferPtr m_commandBuffer;
	class RebarTransientMemoryPool* m_rebarStagingPool ANKI_DEBUG_CODE(= nullptr);
};

/// Callback for GenericGpuComputeJobQueueElement.
using GenericGpuComputeJobQueueElementCallback = void (*)(GenericGpuComputeJobQueueElementContext& ctx, const void* userData);

/// It has enough info to execute generic compute on the GPU.
class GenericGpuComputeJobQueueElement final
{
public:
	GenericGpuComputeJobQueueElementCallback m_callback;
	const void* m_userData;

	GenericGpuComputeJobQueueElement()
	{
	}
};
static_assert(std::is_trivially_destructible<GenericGpuComputeJobQueueElement>::value == true);

/// Point light render queue element.
class PointLightQueueElement final
{
public:
	U64 m_uuid;
	Vec3 m_worldPosition;
	F32 m_radius;
	Vec3 m_diffuseColor;
	Array<RenderQueue*, 6> m_shadowRenderQueues;

	Array<Vec2, 6> m_shadowAtlasTileOffsets; ///< Renderer internal.
	F32 m_shadowAtlasTileSize; ///< Renderer internal.
	U8 m_shadowLayer; ///< Renderer internal.

	U32 m_index;

	PointLightQueueElement()
	{
	}

	Bool hasShadow() const
	{
		return m_shadowRenderQueues[0] != nullptr;
	}
};
static_assert(std::is_trivially_destructible<PointLightQueueElement>::value == true);

/// Spot light render queue element.
class SpotLightQueueElement final
{
public:
	U64 m_uuid;
	Mat4 m_worldTransform;
	Mat4 m_textureMatrix;
	F32 m_distance;
	F32 m_outerAngle;
	F32 m_innerAngle;
	Vec3 m_diffuseColor;
	Array<Vec3, 4> m_edgePoints;
	RenderQueue* m_shadowRenderQueue;

	U8 m_shadowLayer; ///< Renderer internal.

	U32 m_index;

	SpotLightQueueElement()
	{
	}

	Bool hasShadow() const
	{
		return m_shadowRenderQueue != nullptr;
	}
};
static_assert(std::is_trivially_destructible<SpotLightQueueElement>::value == true);

/// Directional light render queue element.
class DirectionalLightQueueElement final
{
public:
	Array<Mat4, kMaxShadowCascades> m_textureMatrices;
	Array<Mat4, kMaxShadowCascades> m_viewProjectionMatrices;
	Array<RenderQueue*, kMaxShadowCascades> m_shadowRenderQueues;
	U64 m_uuid; ///< Zero means that there is no dir light
	Vec3 m_diffuseColor;
	Vec3 m_direction;
	Array<F32, kMaxShadowCascades> m_shadowCascadesDistances;
	U8 m_shadowCascadeCount; ///< Zero means that it doesn't cast any shadows.
	U8 m_shadowLayer; ///< Renderer internal.

	DirectionalLightQueueElement()
	{
	}

	[[nodiscard]] Bool isEnabled() const
	{
		return m_uuid != 0;
	}

	[[nodiscard]] Bool hasShadow() const
	{
		return isEnabled() && m_shadowCascadeCount > 0;
	}
};
static_assert(std::is_trivially_destructible<DirectionalLightQueueElement>::value == true);

/// Reflection probe render queue element.
class ReflectionProbeQueueElement final
{
public:
	Vec3 m_worldPosition;
	Vec3 m_aabbMin;
	Vec3 m_aabbMax;
	U32 m_textureBindlessIndex;

	U32 m_index;

	ReflectionProbeQueueElement()
	{
	}
};
static_assert(std::is_trivially_destructible<ReflectionProbeQueueElement>::value == true);

/// Contains info for a reflection probe that the renderer will have to refresh.
class ReflectionProbeQueueElementForRefresh final
{
public:
	Array<RenderQueue*, 6> m_renderQueues;
	Vec3 m_worldPosition;
	Texture* m_reflectionTexture;
};

// Probe for global illumination.
class GlobalIlluminationProbeQueueElement final
{
public:
	Vec3 m_aabbMin;
	Vec3 m_aabbMax;
	UVec3 m_cellCounts;
	U32 m_totalCellCount;
	Vec3 m_cellSizes; ///< The cells might not be cubes so have different sizes per dimension.
	F32 m_fadeDistance;
	U32 m_volumeTextureBindlessIndex;

	U32 m_index;

	GlobalIlluminationProbeQueueElement()
	{
	}

	Bool operator<(const GlobalIlluminationProbeQueueElement& b) const
	{
		if(m_cellSizes.x() != b.m_cellSizes.x())
		{
			return m_cellSizes.x() < b.m_cellSizes.x();
		}
		else
		{
			return m_totalCellCount < b.m_totalCellCount;
		}
	}
};
static_assert(std::is_trivially_destructible<GlobalIlluminationProbeQueueElement>::value == true);

/// Contains info for a GI probe that the renderer will have to refresh.
class GlobalIlluminationProbeQueueElementForRefresh final
{
public:
	Array<RenderQueue*, 6> m_renderQueues;
	Texture* m_volumeTexture;
	UVec3 m_cellToRefresh;
	UVec3 m_cellCounts;

	GlobalIlluminationProbeQueueElementForRefresh()
	{
	}
};

/// Lens flare render queue element.
class LensFlareQueueElement final
{
public:
	/// Totaly unsafe but we can't have a smart ptr in here since there will be no deletion.
	TextureView* m_textureView;
	Vec3 m_worldPosition;
	Vec2 m_firstFlareSize;
	Vec4 m_colorMultiplier;

	LensFlareQueueElement()
	{
	}
};
static_assert(std::is_trivially_destructible<LensFlareQueueElement>::value == true);

/// Decal render queue element.
class DecalQueueElement final
{
public:
	U32 m_diffuseBindlessTextureIndex;
	U32 m_roughnessMetalnessBindlessTextureIndex;
	F32 m_diffuseBlendFactor;
	F32 m_roughnessMetalnessBlendFactor;
	Mat4 m_textureMatrix;
	Vec3 m_obbCenter;
	Vec3 m_obbExtend;
	Mat3 m_obbRotation;

	U32 m_index;

	DecalQueueElement()
	{
	}
};
static_assert(std::is_trivially_destructible<DecalQueueElement>::value == true);

/// Draw callback for drawing.
using UiQueueElementDrawCallback = void (*)(CanvasPtr& canvas, void* userData);

/// UI element render queue element.
class UiQueueElement final
{
public:
	void* m_userData;
	UiQueueElementDrawCallback m_drawCallback;

	UiQueueElement()
	{
	}
};
static_assert(std::is_trivially_destructible<UiQueueElement>::value == true);

/// Fog density queue element.
class FogDensityQueueElement final
{
public:
	union
	{
		Vec3 m_aabbMin;
		Vec3 m_sphereCenter;
	};

	union
	{
		Vec3 m_aabbMax;
		F32 m_sphereRadius;
	};

	F32 m_density;
	U32 m_index;
	Bool m_isBox;

	FogDensityQueueElement()
	{
	}
};
static_assert(std::is_trivially_destructible<FogDensityQueueElement>::value == true);

/// A callback to fill a coverage buffer.
using FillCoverageBufferCallback = void (*)(void* userData, F32* depthValues, U32 width, U32 height);

/// Ray tracing queue element.
class RayTracingInstanceQueueElement final
{
public:
	AccelerationStructure* m_bottomLevelAccelerationStructure;
	U32 m_shaderGroupHandleIndex;

	U32 m_worldTransformsOffset;
	U32 m_uniformsOffset;
	U32 m_geometryOffset;

	U32 m_indexBufferOffset;

	Mat3x4 m_transform;
};
static_assert(std::is_trivially_destructible<RayTracingInstanceQueueElement>::value == true);

/// Skybox info.
class SkyboxQueueElement final
{
public:
	TextureView* m_skyboxTexture;
	Vec3 m_solidColor;

	class
	{
	public:
		F32 m_minDensity;
		F32 m_maxDensity;
		F32 m_heightOfMinDensity; ///< The height (meters) where fog density is max.
		F32 m_heightOfMaxDensity; ///< The height (meters) where fog density is the min value.
		F32 m_scatteringCoeff;
		F32 m_absorptionCoeff;
		Vec3 m_diffuseColor;
	} m_fog;
};
static_assert(std::is_trivially_destructible<SkyboxQueueElement>::value == true);

/// The render queue. This is what the renderer is fed to render.
class RenderQueue : public RenderingMatrices
{
public:
	WeakArray<RenderableQueueElement> m_renderables; ///< Deferred shading or shadow renderables.
	WeakArray<RenderableQueueElement> m_forwardShadingRenderables;
	WeakArray<PointLightQueueElement> m_pointLights; ///< Those who cast shadows are first.
	WeakArray<SpotLightQueueElement> m_spotLights; ///< Those who cast shadows are first.
	DirectionalLightQueueElement m_directionalLight;
	WeakArray<ReflectionProbeQueueElement> m_reflectionProbes;
	WeakArray<GlobalIlluminationProbeQueueElement> m_giProbes;
	WeakArray<LensFlareQueueElement> m_lensFlares;
	WeakArray<DecalQueueElement> m_decals;
	WeakArray<FogDensityQueueElement> m_fogDensityVolumes;
	WeakArray<UiQueueElement> m_uis;
	WeakArray<GenericGpuComputeJobQueueElement> m_genericGpuComputeJobs;
	WeakArray<RayTracingInstanceQueueElement> m_rayTracingInstances;

	/// Contains the ray tracing elements. The rest of the members are unused. It's separate to avoid multithreading bugs.
	RenderQueue* m_rayTracingQueue = nullptr;

	SkyboxQueueElement m_skybox;

	/// Applies only if the RenderQueue holds shadow casters. It's the max timesamp of all shadow casters
	Timestamp m_shadowRenderablesLastUpdateTimestamp = 0;

	F32 m_cameraNear;
	F32 m_cameraFar;
	F32 m_cameraFovX;
	F32 m_cameraFovY;

	FillCoverageBufferCallback m_fillCoverageBufferCallback = nullptr;
	void* m_fillCoverageBufferCallbackUserData = nullptr;

	ReflectionProbeQueueElementForRefresh* m_reflectionProbeForRefresh = nullptr;
	GlobalIlluminationProbeQueueElementForRefresh* m_giProbeForRefresh = nullptr;

	RenderQueue()
	{
		zeroMemory(m_directionalLight);
		zeroMemory(m_skybox);
	}

	U32 countAllRenderables() const;
};

static_assert(std::is_trivially_destructible<RenderQueue>::value == true);
/// @}

} // end namespace anki
