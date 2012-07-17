/*
 * Copyright (C) 1995-2008 University of Karlsruhe.  All right reserved.
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
 * @brief   A nodemap. This variant is a thin wrapper around an ARR_F which
 *          uses node-indices for access. It is preferable over ir_nodehashmap
 *          if the info is dense (i.e. something is mapped for most nodes in
 *          the graph)
 * @author  Matthias Braun
 */
#ifndef FIRM_IRNODEMAP_H
#define FIRM_IRNODEMAP_H

#include "firm_types.h"
#include "irgraph_t.h"
#include "array.h"

typedef struct ir_nodemap ir_nodemap;

/**
 * Allocate and initialize a new nodemap object
 *
 * @param irg           The graph the nodemap will run on.
 * @return              A new nodemap object.
 */
static inline void ir_nodemap_init(ir_nodemap *nodemap, const ir_graph *irg)
{
	unsigned max_idx = get_irg_last_idx(irg) + 32;
	nodemap->data = NEW_ARR_F(void*, max_idx);
	memset(nodemap->data, 0, max_idx * sizeof(nodemap->data[0]));
}

/**
 * frees all internal memory used by the nodemap but does not free the
 * nodemap struct itself.
 */
static inline void ir_nodemap_destroy(ir_nodemap *nodemap)
{
	DEL_ARR_F(nodemap->data);
	nodemap->data = NULL;
}

/**
 * Insert a mapping from @p node to @p data.
 */
static inline void ir_nodemap_insert(ir_nodemap *nodemap, const ir_node *node,
                                     void *data)
{
	unsigned idx = get_irn_idx(node);
	size_t   len = ARR_LEN(nodemap->data);
	if (idx >= len) {
		ARR_RESIZE(void*, nodemap->data, idx+1);
		memset(nodemap->data + len, 0, (idx-len) * sizeof(nodemap->data[0]));
	}
	nodemap->data[idx] = data;
}

/**
 * Insert a mapping from @p node to @p data (fast version).
 *
 * @attention You must only use this version if you can be sure that the nodemap
 * already has enough space to store the mapping. This is the case if @p node
 * already existed at nodemap_init() time or ir_nodemap_insert() has been used
 * for this node)
 */
static inline void ir_nodemap_insert_fast(ir_nodemap *nodemap,
                                          const ir_node *node, void *data)
{
	unsigned idx = get_irn_idx(node);
	nodemap->data[idx] = data;
}

/**
 * Removed mapping for @p node.
 *
 * This is really a shorthand form for ir_nodemap_insert(nodemap, node, NULL);
 */
static inline void ir_nodemap_remove(ir_nodemap *nodemap, const ir_node *node)
{
	ir_nodemap_insert(nodemap, node, NULL);
}

/**
 * Get mapping for @p node. Returns NULL if nothing is mapped.
 */
static inline void *ir_nodemap_get(const ir_nodemap *nodemap,
                                   const ir_node *node)
{
	unsigned idx = get_irn_idx(node);
	if (idx >= ARR_LEN(nodemap->data))
		return NULL;
	return nodemap->data[idx];
}

#define ir_nodemap_get(type, nodemap, node) ((type*)ir_nodemap_get(nodemap, node))

/**
 * Get mapping for @p node (fast version). Returns NULL if nothing is mapped.
 *
 * @attention You must only use this function if you can be sure that the
 * nodemap has enough space to potentially contain the mapping. This is the
 * case if @p node already existed at nodemap_init() time or ir_nodemap_insert()
 * has been used for this node)
 */
static inline void *ir_nodemap_get_fast(const ir_nodemap *nodemap,
                                        const ir_node *node)
{
	unsigned idx = get_irn_idx(node);
	return nodemap->data[idx];
}

#endif
