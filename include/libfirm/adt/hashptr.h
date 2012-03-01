/*
 * Copyright (C) 1995-2011 University of Karlsruhe.  All right reserved.
 *
 * This file is part of libFirm.
 *
 * This file may be distributed and/or modified under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation and appearing in the file LICENSE.GPL included in the
 * packaging of this file.
 *
 * Licensees holding valid libFirm Professional Edition licenses may use
 * this file in accordance with the libFirm Commercial License.
 * Agreement provided with the Software.
 *
 * This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 * WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE.
 */

/**
 * @file
 * @brief       Hash functions
 * @author      Michael Beck, Sebastian Hack
 */
#ifndef FIRM_ADT_HASHPTR_H
#define FIRM_ADT_HASHPTR_H

#include <stdlib.h>
#include "../begin.h"
#include <stddef.h>

/**
 * @ingroup algorithms
 * @defgroup hashptr Hash Functions
 * @{
 */

/** @cond DISABLED */

#define _FIRM_FNV_OFFSET_BASIS 2166136261U
#define _FIRM_FNV_FNV_PRIME 16777619U

/* Computing x * _FIRM_FNV_FNV_PRIME */
#define _FIRM_FNV_TIMES_PRIME(x) ((x) * _FIRM_FNV_FNV_PRIME)

/** @endcond */

/**
 * Returns a hash value for a block of data.
 */
static inline unsigned hash_data(const unsigned char *data, size_t bytes)
{
	size_t   i;
	unsigned hash = _FIRM_FNV_OFFSET_BASIS;

	for(i = 0; i < bytes; ++i) {
		hash = _FIRM_FNV_TIMES_PRIME(hash);
		hash ^= data[i];
	}

	return hash;
}

/**
 * Returns a hash value for a string.
 * @param str The string (can be const).
 * @return A hash value for the string.
 */
static inline unsigned hash_str(const char *str)
{
	unsigned i;
	unsigned hash = _FIRM_FNV_OFFSET_BASIS;

	for(i = 0; str[i] != '\0'; ++i) {
		hash = _FIRM_FNV_TIMES_PRIME(hash);
		hash ^= str[i];
	}

	return hash;
}

/**
 * Returns a hash value for a pointer.
 * Pointer addresses are mostly aligned to 4 or 8 bytes. So we remove the
 * lowest 3 bits.
 */
static inline unsigned hash_ptr(const void *ptr)
{
	return ((unsigned)(((char *) (ptr) - (char *)0) >> 3));
}

/**
 * Combines 2 hash values.
 * @param x One hash value.
 * @param y Another hash value.
 * @return  A hash value computed from both.
 */
static inline unsigned hash_combine(unsigned x, unsigned y)
{
	unsigned hash = _FIRM_FNV_TIMES_PRIME(_FIRM_FNV_OFFSET_BASIS);
	hash ^= x;
	hash  = _FIRM_FNV_TIMES_PRIME(hash);
	hash ^= y;
	return hash;
}

/** @} */

#include "../end.h"

#endif
