/*
 * Copyright (C) 2011 University of Karlsruhe.  All right reserved.
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
 * @brief   Lower (stack-) Alloc nodes to allocate an aligned number of bytes
 * @author  Matthias Braun
 */
#include "config.h"

#include "lower_alloc.h"
#include "irgwalk.h"
#include "irnode_t.h"
#include "ircons.h"
#include "error.h"
#include "irgmod.h"
#include "irnodeset.h"

static bool         lower_constant_sizes;
static unsigned     stack_alignment;
static long         addr_delta;
static ir_nodeset_t transformed;

/**
 * Adjust the size of a node representing a stack alloc to a certain
 * stack_alignment.
 *
 * @param size       the node containing the non-aligned size
 * @param block      the block where new nodes are allocated on
 * @return a node representing the aligned size
 */
static ir_node *adjust_alloc_size(dbg_info *dbgi, ir_node *size, ir_node *block)
{
	ir_mode   *mode;
	ir_tarval *tv;
	ir_node   *mask;
	ir_graph  *irg;

	if (stack_alignment <= 1)
		return size;
	if (is_Const(size) && !lower_constant_sizes)
		return size;

	mode = get_irn_mode(size);
	tv   = new_tarval_from_long(stack_alignment-1, mode);
	irg  = get_Block_irg(block);
	mask = new_r_Const(irg, tv);
	size = new_rd_Add(dbgi, block, size, mask, mode);

	tv   = new_tarval_from_long(-(long)stack_alignment, mode);
	mask = new_r_Const(irg, tv);
	size = new_rd_And(dbgi, block, size, mask, mode);
	return size;
}

static void transform_Proj_Alloc(ir_node *node)
{
	ir_graph *irg;
	dbg_info *dbgi;
	ir_node *block;
	ir_node *delta;
	ir_node *add;
	ir_node *dummy;
	ir_node *alloc;
	ir_node *new_proj;

	/* we might need a result adjustment */
	if (addr_delta == 0)
		return;
	if (get_Proj_proj(node) != pn_Alloc_res)
		return;
	if (ir_nodeset_contains(&transformed, node))
		return;

	alloc = get_Proj_pred(node);
	dbgi  = get_irn_dbg_info(alloc);
	irg   = get_irn_irg(node);
	block = get_nodes_block(node);
	delta = new_r_Const_long(irg, mode_P, addr_delta);
	dummy = new_r_Dummy(irg, mode_P);
	add   = new_rd_Add(dbgi, block, dummy, delta, mode_P);

	exchange(node, add);
	new_proj = new_r_Proj(alloc, mode_P, pn_Alloc_res);
	set_Add_left(add, new_proj);
	ir_nodeset_insert(&transformed, new_proj);
}

/**
 * lower Alloca nodes to allocate "bytes" instead of a certain type
 */
static void lower_alloca_free(ir_node *node, void *data)
{
	ir_type  *type;
	unsigned  size;
	ir_graph *irg;
	ir_node  *count;
	ir_mode  *mode;
	ir_node  *szconst;
	ir_node  *block;
	ir_node  *mem;
	ir_type  *new_type;
	ir_node  *mul;
	ir_node  *new_size;
	dbg_info *dbgi;
	ir_node  *new_node;
	ir_where_alloc where;
	(void) data;
	if (is_Alloc(node)) {
		type = get_Alloc_type(node);
	} else if (is_Free(node)) {
		type = get_Free_type(node);
	} else if (is_Proj(node)) {
		ir_node *proj_pred = get_Proj_pred(node);
		if (is_Alloc(proj_pred)) {
			transform_Proj_Alloc(node);
		}
		return;
	} else {
		return;
	}
	if (ir_nodeset_contains(&transformed, node))
		return;

	ir_nodeset_insert(&transformed, node);
	size = get_type_size_bytes(type);
	if (is_unknown_type(type))
		size = 1;
	if (size == 1 && stack_alignment <= 1)
		return;

	if (is_Alloc(node)) {
		count     = get_Alloc_count(node);
		mem       = get_Alloc_mem(node);
		where     = get_Alloc_where(node);
	} else {
		count = get_Free_count(node);
		mem   = get_Free_mem(node);
		where = get_Free_where(node);
	}
	mode      = get_irn_mode(count);
	block     = get_nodes_block(node);
	irg       = get_irn_irg(node);
	szconst   = new_r_Const_long(irg, mode, (long)size);
	mul       = new_r_Mul(block, count, szconst, mode);
	dbgi      = get_irn_dbg_info(node);
	new_size  = adjust_alloc_size(dbgi, mul, block);
	new_type  = get_unknown_type();
	if (is_Alloc(node)) {
		new_node = new_rd_Alloc(dbgi, block, mem, new_size, new_type, where);
	} else {
		ir_node *ptr = get_Free_ptr(node);
		new_node
			= new_rd_Free(dbgi, block, mem, ptr, new_size, new_type, where);
	}
	ir_nodeset_insert(&transformed, new_node);

	if (new_node != node)
		exchange(node, new_node);
}

void lower_alloc(ir_graph *irg, unsigned new_stack_alignment, bool lower_consts,
                 long new_addr_delta)
{
	if (!is_po2(stack_alignment))
		panic("lower_alloc only supports stack alignments that are a power of 2");
	addr_delta           = new_addr_delta;
	stack_alignment      = new_stack_alignment;
	lower_constant_sizes = lower_consts;
	ir_nodeset_init(&transformed);
	irg_walk_graph(irg, lower_alloca_free, NULL, NULL);
	ir_nodeset_destroy(&transformed);
}
