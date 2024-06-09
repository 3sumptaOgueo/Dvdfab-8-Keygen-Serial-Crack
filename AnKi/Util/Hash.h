// Copyright (C) 2009-2023, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Util/StdTypes.h>

namespace anki {

/// @addtogroup util_other
/// @{

/// Computes a hash of a buffer. This function implements the MurmurHash2 algorithm by Austin Appleby.
/// @param[in] buffer The buffer to hash.
/// @param bufferSize The size of the buffer.
/// @param seed A unique seed.
/// @return The hash.
[[nodiscard]] ANKI_PURE U64 computeHash(const void* buffer, PtrSize bufferSize, U64 seed = 123);

/// Computes a hash of a buffer. This function implements the MurmurHash2 algorithm by Austin Appleby.
/// @param[in] buffer The buffer to hash.
/// @param bufferSize The size of the buffer.
/// @param prevHash The hash to append to.
/// @return The new hash.
[[nodiscard]] ANKI_PURE U64 appendHash(const void* buffer, PtrSize bufferSize, U64 prevHash);

/// See computeHash.
template<typename T>
[[nodiscard]] ANKI_PURE U64 computeObjectHash(const T& obj, U64 seed = 123)
{
	return computeHash(&obj, sizeof(obj), seed);
}

/// See appendHash.
template<typename T>
[[nodiscard]] ANKI_PURE U64 appendObjectHash(const T& obj, U64 prevHash)
{
	return appendHash(&obj, sizeof(obj), prevHash);
}
/// @}

} // end namespace anki
