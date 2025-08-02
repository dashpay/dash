/* $Id: sph_types.h 260 2011-07-21 01:02:38Z tp $ */
/**
 * Basic type definitions.
 *
 * This header file defines the generic integer types that will be used
 * for the implementation of hash functions; it also contains helper
 * functions which encode and decode multi-byte integer values, using
 * either little-endian or big-endian conventions.
 *
 * This file contains a compile-time test on the size of a byte
 * (the <code>unsigned char</code> C type). If bytes are not octets,
 * i.e. if they do not have a size of exactly 8 bits, then compilation
 * is aborted. Architectures where bytes are not octets are relatively
 * rare, even in the embedded devices market. We forbid non-octet bytes
 * because there is no clear convention on how octet streams are encoded
 * on such systems.
 *
 * ==========================(LICENSE BEGIN)============================
 *
 * Copyright (c) 2007-2010  Projet RNRT SAPHIR
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * ===========================(LICENSE END)=============================
 *
 * @file     sph_types.h
 * @author   Thomas Pornin <thomas.pornin@cryptolog.com>
 */

#ifndef SPH_TYPES_H__
#define SPH_TYPES_H__

#include <attributes.h>
#include <compat/byteswap.h>
#include <crypto/common.h>
#include <compat/endian.h>

#include <bit>
#include <cstdint>
#include <cstring>

using sph_s32 = int32_t;
using sph_u32 = uint32_t;
using sph_s64 = int64_t;
using sph_u64 = uint64_t;

#define SPH_C32(x)    sph_u32{x}
#define SPH_C64(x)    sph_u64{x}

#define SPH_T32(x)    (x)
#define SPH_T64(x)    (x)

#define SPH_ROTL32(x, n)   std::rotl(sph_u32{x}, n)
#define SPH_ROTR32(x, n)   std::rotr(sph_u32{x}, n)
#define SPH_ROTL64(x, n)   std::rotl(sph_u64{x}, n)
#define SPH_ROTR64(x, n)   std::rotr(sph_u64{x}, n)

#define SPH_INLINE ALWAYS_INLINE

/**
 * Byte-swap a 32-bit value.
 *
 * @param x   the input value
 * @return  the byte-swapped value
 */
static SPH_INLINE sph_u32
sph_bswap32(sph_u32 x)
{
    return internal_bswap_32(x);
}

/**
 * Byte-swap a 64-bit value.
 *
 * @param x   the input value
 * @return  the byte-swapped value
 */
static SPH_INLINE sph_u64
sph_bswap64(sph_u64 x)
{
    return internal_bswap_64(x);
}

/**
 * Encode a 16-bit unsigned value into memory, in big-endian convention
 * (most significant byte comes first).
 *
 * @param dst   the destination buffer
 * @param val   the value to encode
 */
static SPH_INLINE void sph_enc16be(void *dst, unsigned val)
{
    WriteLE16(static_cast<unsigned char*>(dst), htobe16_internal(val));
}

/**
 * Decode a 16-bit unsigned value from memory, in big-endian convention
 * (most significant byte comes first).
 *
 * @param src   the source address
 * @return  the decoded value
 */
static SPH_INLINE unsigned
sph_dec16be(const void *src)
{
    return be16toh_internal(ReadLE16(static_cast<const unsigned char*>(src)));
}

/**
 * Encode a 16-bit unsigned value into memory, in little-endian convention
 * (least significant byte comes first).
 *
 * @param dst   the destination buffer
 * @param val   the value to encode
 */
static SPH_INLINE void
sph_enc16le(void *dst, unsigned val)
{
    WriteLE16(static_cast<unsigned char*>(dst), val);
}

/**
 * Decode a 16-bit unsigned value from memory, in little-endian convention
 * (least significant byte comes first).
 *
 * @param src   the source address
 * @return  the decoded value
 */
static SPH_INLINE unsigned sph_dec16le(const void *src)
{
    return ReadLE16(static_cast<const unsigned char*>(src));
}

/**
 * Encode a 32-bit value into the provided buffer (big endian convention).
 *
 * @param dst   the destination buffer
 * @param val   the 32-bit value to encode
 */
static SPH_INLINE void
sph_enc32be(void *dst, sph_u32 val)
{
    WriteBE32(static_cast<unsigned char*>(dst), val);
}

/**
 * Encode a 32-bit value into the provided buffer (big endian convention).
 * The destination buffer must be properly aligned.
 *
 * @param dst   the destination buffer (32-bit aligned)
 * @param val   the value to encode
 */
static SPH_INLINE void
sph_enc32be_aligned(void *dst, sph_u32 val)
{
    WriteBE32(static_cast<unsigned char*>(dst), val);
}

/**
 * Decode a 32-bit value from the provided buffer (big endian convention).
 *
 * @param src   the source buffer
 * @return  the decoded value
 */
static SPH_INLINE sph_u32
sph_dec32be(const void *src)
{
    return ReadBE32(static_cast<const unsigned char*>(src));
}

/**
 * Decode a 32-bit value from the provided buffer (big endian convention).
 * The source buffer must be properly aligned.
 *
 * @param src   the source buffer (32-bit aligned)
 * @return  the decoded value
 */
static SPH_INLINE sph_u32
sph_dec32be_aligned(const void *src)
{
    return ReadBE32(static_cast<const unsigned char*>(src));
}

/**
 * Encode a 32-bit value into the provided buffer (little endian convention).
 *
 * @param dst   the destination buffer
 * @param val   the 32-bit value to encode
 */
static SPH_INLINE void
sph_enc32le(void *dst, sph_u32 val)
{
    WriteLE32(static_cast<unsigned char*>(dst), val);
}

/**
 * Encode a 32-bit value into the provided buffer (little endian convention).
 * The destination buffer must be properly aligned.
 *
 * @param dst   the destination buffer (32-bit aligned)
 * @param val   the value to encode
 */
static SPH_INLINE void
sph_enc32le_aligned(void *dst, sph_u32 val)
{
    WriteLE32(static_cast<unsigned char*>(dst), val);
}

/**
 * Decode a 32-bit value from the provided buffer (little endian convention).
 *
 * @param src   the source buffer
 * @return  the decoded value
 */
static SPH_INLINE sph_u32
sph_dec32le(const void *src)
{
    return ReadLE32(static_cast<const unsigned char*>(src));
}

/**
 * Decode a 32-bit value from the provided buffer (little endian convention).
 * The source buffer must be properly aligned.
 *
 * @param src   the source buffer (32-bit aligned)
 * @return  the decoded value
 */
static SPH_INLINE sph_u32
sph_dec32le_aligned(const void *src)
{
    return ReadLE32(static_cast<const unsigned char*>(src));
}

/**
 * Encode a 64-bit value into the provided buffer (big endian convention).
 *
 * @param dst   the destination buffer
 * @param val   the 64-bit value to encode
 */
static SPH_INLINE void
sph_enc64be(void *dst, sph_u64 val)
{
    WriteBE64(static_cast<unsigned char*>(dst), val);
}

/**
 * Encode a 64-bit value into the provided buffer (big endian convention).
 * The destination buffer must be properly aligned.
 *
 * @param dst   the destination buffer (64-bit aligned)
 * @param val   the value to encode
 */
static SPH_INLINE void
sph_enc64be_aligned(void *dst, sph_u64 val)
{
    WriteBE64(static_cast<unsigned char*>(dst), val);
}

/**
 * Decode a 64-bit value from the provided buffer (big endian convention).
 *
 * @param src   the source buffer
 * @return  the decoded value
 */
static SPH_INLINE sph_u64
sph_dec64be(const void *src)
{
    return ReadBE64(static_cast<const unsigned char*>(src));
}

/**
 * Decode a 64-bit value from the provided buffer (big endian convention).
 * The source buffer must be properly aligned.
 *
 * @param src   the source buffer (64-bit aligned)
 * @return  the decoded value
 */
static SPH_INLINE sph_u64
sph_dec64be_aligned(const void *src)
{
    return ReadBE64(static_cast<const unsigned char*>(src));
}

/**
 * Encode a 64-bit value into the provided buffer (little endian convention).
 *
 * @param dst   the destination buffer
 * @param val   the 64-bit value to encode
 */
static SPH_INLINE void
sph_enc64le(void *dst, sph_u64 val)
{
    WriteLE64(static_cast<unsigned char*>(dst), val);
}

/**
 * Encode a 64-bit value into the provided buffer (little endian convention).
 * The destination buffer must be properly aligned.
 *
 * @param dst   the destination buffer (64-bit aligned)
 * @param val   the value to encode
 */
static SPH_INLINE void
sph_enc64le_aligned(void *dst, sph_u64 val)
{
    WriteLE64(static_cast<unsigned char*>(dst), val);
}

/**
 * Decode a 64-bit value from the provided buffer (little endian convention).
 *
 * @param src   the source buffer
 * @return  the decoded value
 */
static SPH_INLINE sph_u64
sph_dec64le(const void *src)
{
    return ReadLE64(static_cast<const unsigned char*>(src));
}

/**
 * Decode a 64-bit value from the provided buffer (little endian convention).
 * The source buffer must be properly aligned.
 *
 * @param src   the source buffer (64-bit aligned)
 * @return  the decoded value
 */
static SPH_INLINE sph_u64
sph_dec64le_aligned(const void *src)
{
    return ReadLE64(static_cast<const unsigned char*>(src));
}

#endif
