// Copyright (C) 2009-2023, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Shaders/Include/Common.h>

ANKI_BEGIN_NAMESPACE

// RT shadows
constexpr U32 kMaxRtShadowLayers = 8u;

struct RtShadowsUniforms
{
	F32 historyRejectFactor[kMaxRtShadowLayers]; // 1.0 means reject, 0.0 not reject
};

struct RtShadowsDenoiseUniforms
{
	Mat4 invViewProjMat;

	F32 time;
	F32 padding0;
	F32 padding1;
	F32 padding2;
};

// Indirect diffuse
struct IndirectDiffuseUniforms
{
	UVec2 m_viewportSize;
	Vec2 m_viewportSizef;

	Vec4 m_projectionMat;

	RF32 m_radius; ///< In meters.
	U32 m_sampleCount;
	RF32 m_sampleCountf;
	RF32 m_ssaoBias;

	RF32 m_ssaoStrength;
	F32 m_padding0;
	F32 m_padding1;
	F32 m_padding2;
};

struct IndirectDiffuseDenoiseUniforms
{
	Mat4 m_invertedViewProjectionJitterMat;

	UVec2 m_viewportSize;
	Vec2 m_viewportSizef;

	F32 m_sampleCountDiv2;
	F32 m_padding0;
	F32 m_padding1;
	F32 m_padding2;
};

// Lens flare
struct LensFlareSprite
{
	Vec4 m_posScale; // xy: Position, zw: Scale
	RVec4 m_color;
	Vec4 m_depthPad3;
};

// Depth downscale
struct DepthDownscaleUniforms
{
	Vec2 m_srcTexSizeOverOne;
	U32 m_workgroupCount;
	U32 m_mipmapCount;

	U32 m_lastMipWidth;
	F32 m_padding0;
	F32 m_padding1;
	F32 m_padding2;
};

// Screen space reflections uniforms
struct SsrUniforms
{
	UVec2 m_depthBufferSize;
	UVec2 m_framebufferSize;

	U32 m_frameCount;
	U32 m_depthMipCount;
	U32 m_maxSteps;
	U32 m_lightBufferMipCount;

	UVec2 m_padding0;
	F32 m_roughnessCutoff;
	U32 m_firstStepPixels;

	Mat4 m_prevViewProjMatMulInvViewProjMat;
	Mat4 m_projMat;
	Mat4 m_invProjMat;
	Mat3x4 m_normalMat;
};

// Vol fog
struct VolumetricFogUniforms
{
	RVec3 m_fogDiffuse;
	RF32 m_fogScatteringCoeff;

	RF32 m_fogAbsorptionCoeff;
	RF32 m_near;
	RF32 m_far;
	F32 m_zSplitCountf;

	UVec3 m_volumeSize;
	F32 m_maxZSplitsToProcessf;
};

// Vol lighting
struct VolumetricLightingUniforms
{
	RF32 m_densityAtMinHeight;
	RF32 m_densityAtMaxHeight;
	F32 m_minHeight;
	F32 m_oneOverMaxMinusMinHeight; // 1 / (maxHeight / minHeight)

	UVec3 m_volumeSize;
	F32 m_maxZSplitsToProcessf;
};

// Pack visible clusterer objects
struct PointLightExtra
{
	Vec2 m_padding0;
	U32 m_shadowLayer;
	F32 m_shadowAtlasTileScale;

	Vec4 m_shadowAtlasTileOffsets[6u];
};

// Pack visible clusterer objects
struct SpotLightExtra
{
	Vec3 m_padding;
	U32 m_shadowLayer;

	Mat4 m_textureMatrix;
};

struct GpuVisibilityUniforms
{
	Vec4 m_clipPlanes[6u];

	UVec3 m_padding1;
	U32 m_aabbCount;

	Vec4 m_maxLodDistances;

	Vec3 m_cameraOrigin;
	F32 m_padding2;

	Mat4 m_viewProjectionMat;
};

struct HzbUniforms
{
	Mat4 m_reprojectionMatrix; ///< For the main camera.
	Mat4 m_invertedViewProjectionMatrix; ///< NDC to world for the main camera.
	Mat4 m_projectionMatrix;
	Mat4 m_shadowCascadeViewProjectionMatrices[kMaxShadowCascades];
};

ANKI_END_NAMESPACE
