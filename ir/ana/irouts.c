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
 * @brief    Compute and access out edges (also called def-use edges).
 * @author   Goetz Lindenmaier, Michael Beck
 * @date     1.2002
 */
#include "config.h"

#include <string.h>

#include "xmalloc.h"
#include "irouts.h"
#include "irnode_t.h"
#include "irgraph_t.h"
#include "irprog_t.h"
#include "irgwalk.h"
#include "util.h"
#include "irprintf.h"
#include "error.h"
#include "ircons.h"

int get_irn_outs_computed(const ir_node *node)
{
	return node->out != NULL;
}

int get_irn_n_outs(const ir_node *node)
{
	assert(node->kind == k_ir_node);
	assert(node->out_valid);
	/* we misuse the first for the size info of the out array */
	return node->out[0].pos;
}

ir_node *get_irn_out(const ir_node *def, int pos)
{
	assert(pos >= 0 && pos < get_irn_n_outs(def));
	assert(def->out_valid);
	return def->out[pos+1].use;
}

ir_node *get_irn_out_ex(const ir_node *def, int pos, int *in_pos)
{
	assert(pos >= 0 && pos < get_irn_n_outs(def));
	assert(def->out_valid);
	*in_pos = def->out[pos+1].pos;
	return def->out[pos+1].use;
}

void set_irn_out(ir_node *def, int pos, ir_node *use, int in_pos)
{
	assert(use);
	assert(pos >= 0 && pos < get_irn_n_outs(def));
	assert(def->out_valid);
	def->out[pos+1].use = use;
	def->out[pos+1].pos = in_pos;
}

int get_Block_n_cfg_outs(const ir_node *bl)
{
	assert(is_Block(bl));
	assert(bl->out_valid);
	int n_cfg_outs = 0;
	for (int i = 1; i <= bl->out[0].pos; ++i) {
		const ir_node *succ = bl->out[i].use;
		if (get_irn_mode(succ) == mode_X && !is_End(succ) && !is_Bad(succ))
			n_cfg_outs += succ->out[0].pos;
	}
	return n_cfg_outs;
}

int get_Block_n_cfg_outs_ka(const ir_node *bl)
{
	assert(is_Block(bl));
	assert(bl->out_valid);
	int n_cfg_outs = 0;
	for (int i = 1; i <= bl->out[0].pos; ++i) {
		const ir_node *succ = bl->out[i].use;
		if (get_irn_mode(succ) == mode_X) {
			if (is_Bad(succ))
				continue;
			if (is_End(succ)) {
				/* ignore End if we are in the Endblock */
				if (get_nodes_block(succ) == bl)
					continue;
				else /* count Keep-alive as one */
					n_cfg_outs += 1;
			} else
				n_cfg_outs += succ->out[0].pos;
		}
	}
	return n_cfg_outs;
}

ir_node *get_Block_cfg_out(const ir_node *bl, int pos)
{
	assert(is_Block(bl));
	assert(bl->out_valid);
	for (int i = 1; i <= bl->out[0].pos; ++i) {
		const ir_node *succ = bl->out[i].use;
		if (get_irn_mode(succ) == mode_X && !is_End(succ) && !is_Bad(succ)) {
			int n_outs = succ->out[0].pos;
			if (pos < n_outs)
				return succ->out[pos + 1].use;
			else
				pos -= n_outs;
		}
	}
	return NULL;
}

ir_node *get_Block_cfg_out_ka(const ir_node *bl, int pos)
{
	assert(is_Block(bl));
	assert(bl->out_valid);
	for (int i = 1; i <= bl->out[0].pos; ++i) {
		const ir_node *succ = bl->out[i].use;
		if (get_irn_mode(succ) == mode_X) {
			if (is_Bad(succ))
				continue;
			if (is_End(succ)) {
				ir_node *end_bl = get_nodes_block(succ);
				if (end_bl == bl) {
					/* ignore End if we are in the Endblock */
					continue;
				}
				if (pos == 0) {
					/* handle keep-alive here: return the Endblock instead of the End node */
					return end_bl;
				} else
					--pos;
			} else {
				int n_outs = succ->out[0].pos;
				if (pos < n_outs)
					return succ->out[pos + 1].use;
				else
					pos -= n_outs;
			}
		}
	}
	return NULL;
}

static void irg_out_walk_2(ir_node *node, irg_walk_func *pre,
                           irg_walk_func *post, void *env)
{
	assert(get_irn_visited(node) < get_irg_visited(current_ir_graph));

	set_irn_visited(node, get_irg_visited(current_ir_graph));

	if (pre) pre(node, env);

	int n = get_irn_n_outs(node);
	for (int i = 0; i < n; ++i) {
		ir_node *succ = get_irn_out(node, i);
		if (get_irn_visited(succ) < get_irg_visited(current_ir_graph))
			irg_out_walk_2(succ, pre, post, env);
	}

	if (post) post(node, env);
}

void irg_out_walk(ir_node *node, irg_walk_func *pre, irg_walk_func *post,
                  void *env)
{
	assert(node);
	ir_graph *irg = get_irn_irg(node);
	if (irg_has_properties(irg, IR_GRAPH_PROPERTY_CONSISTENT_OUTS)) {
		inc_irg_visited(irg);
		irg_out_walk_2(node, pre, post, env);
	}
}

static void irg_out_block_walk2(ir_node *bl, irg_walk_func *pre,
                                irg_walk_func *post, void *env)
{
	if (Block_block_visited(bl))
		return;

	mark_Block_block_visited(bl);

	if (pre)
		pre(bl, env);

	int n = get_Block_n_cfg_outs(bl);
	for (int i = 0; i < n; ++i) {
		/* find the corresponding predecessor block. */
		ir_node *pred = get_Block_cfg_out(bl, i);
		/* recursion */
		irg_out_block_walk2(pred, pre, post, env);
	}

	if (post)
		post(bl, env);
}

void irg_out_block_walk(ir_node *node, irg_walk_func *pre, irg_walk_func *post,
                        void *env)
{
	ir_graph *irg = get_irn_irg(node);
	assert(is_Block(node) || (get_irn_mode(node) == mode_X));

	ir_graph *rem = current_ir_graph;
	current_ir_graph = irg;

	inc_irg_block_visited(irg);

	if (get_irn_mode(node) == mode_X) {
		int n = get_irn_n_outs(node);
		for (int i = 0; i < n; ++i) {
			ir_node *succ = get_irn_out(node, i);
			irg_out_block_walk2(succ, pre, post, env);
		}
	} else {
		irg_out_block_walk2(node, pre, post, env);
	}

	current_ir_graph = rem;
}

/*--------------------------------------------------------------------*/
/** Building and Removing the out datastructure                      **/
/**                                                                  **/
/** The outs of a graph are allocated in a single, large array.      **/
/** This allows to allocate and deallocate the memory for the outs   **/
/** on demand.  The large array is separated into many small ones    **/
/** for each node.  Only a single field to reference the out array   **/
/** is stored in each node and a field referencing the large out     **/
/** array in irgraph.  The 0 field of each out array contains the    **/
/** size of this array.  This saves memory in the irnodes themselves.**/
/** The construction does two passes over the graph.  The first pass **/
/** counts the overall number of outs and the outs of each node.  It **/
/** stores the outs of each node in the out reference of the node.   **/
/** Then the large array is allocated.  The second iteration chops   **/
/** the large array into smaller parts, sets the out edges and       **/
/** recounts the out edges.                                          **/
/** Removes Tuple nodes!                                             **/
/*--------------------------------------------------------------------*/


/** Returns the amount of out edges for not yet visited successors. */
static int _count_outs(ir_node *n)
{
	mark_irn_visited(n);
	n->out = (ir_def_use_edge*) INT_TO_PTR(1);     /* Space for array size. */

	int start     = is_Block(n) ? 0 : -1;
	int irn_arity = get_irn_arity(n);
	int res       = irn_arity - start + 1;  /* --1 or --0; 1 for array size. */

	for (int i = start; i < irn_arity; ++i) {
		/* Optimize Tuples.  They annoy if walking the cfg. */
		ir_node *pred         = get_irn_n(n, i);
		ir_node *skipped_pred = skip_Tuple(pred);

		if (skipped_pred != pred) {
			set_irn_n(n, i, skipped_pred);
		}

		/* count Def-Use edges for predecessors */
		if (!irn_visited(skipped_pred))
			res += _count_outs(skipped_pred);

		/*count my Def-Use edges */
		skipped_pred->out = (ir_def_use_edge*) INT_TO_PTR(PTR_TO_INT(skipped_pred->out) + 1);
	}
	return res;
}


/** Returns the amount of out edges for not yet visited successors.
 *  This version handles some special nodes like irg_frame, irg_args etc.
 */
static int count_outs(ir_graph *irg)
{
	inc_irg_visited(irg);
	int res = _count_outs(get_irg_end(irg));

	/* Now handle anchored nodes. We need the out count of those
	   even if they are not visible. */
	for (int i = anchor_last; i >= anchor_first; --i) {
		ir_node *n = get_irg_anchor(irg, i);
		if (!irn_visited_else_mark(n)) {
			n->out = (ir_def_use_edge*) INT_TO_PTR(1);
			++res;
		}
	}
	return res;
}

/**
 * Enter memory for the outs to a node.
 *
 * @param use    current node
 * @param free   current free address in the chunk allocated for the outs
 *
 * @return The next free address
 */
static ir_def_use_edge *_set_out_edges(ir_node *use, ir_def_use_edge *free)
{
	mark_irn_visited(use);

	/* Allocate my array */
	size_t n_outs = PTR_TO_INT(use->out);
	use->out = free;
#ifdef DEBUG_libfirm
	use->out_valid = 1;
#endif /* defined DEBUG_libfirm */
	free += n_outs;
	/* We count the successors again, the space will be sufficient.
	   We use this counter to remember the position for the next back
	   edge. */
	use->out[0].pos = 0;

	int start     = is_Block(use) ? 0 : -1;
	int irn_arity = get_irn_arity(use);

	for (int i = start; i < irn_arity; ++i) {
		ir_node *def = get_irn_n(use, i);

		/* Recursion */
		if (!irn_visited(def))
			free = _set_out_edges(def, free);

		/* Remember this Def-Use edge */
		int pos = def->out[0].pos + 1;
		def->out[pos].use = use;
		def->out[pos].pos = i;

		/* increase the number of Def-Use edges so far */
		def->out[0].pos = pos;
	}
	return free;
}

/**
 * Enter memory for the outs to a node. Handles special nodes
 *
 * @param irg    the graph
 * @param free   current free address in the chunk allocated for the outs
 *
 * @return The next free address
 */
static ir_def_use_edge *set_out_edges(ir_graph *irg, ir_def_use_edge *free)
{
	inc_irg_visited(irg);
	free = _set_out_edges(get_irg_end(irg), free);

	/* handle anchored nodes */
	for (int i = anchor_last; i >= anchor_first; --i) {
		ir_node *n = get_irg_anchor(irg, i);
		if (!irn_visited_else_mark(n)) {
			size_t n_outs = PTR_TO_INT(n->out);
			n->out = free;
#ifdef DEBUG_libfirm
			n->out_valid = 1;
#endif
			free += n_outs;
		}
	}

	return free;
}

void compute_irg_outs(ir_graph *irg)
{
	int             n_out_edges = 0;
	ir_def_use_edge *end = NULL;         /* Only for debugging */

	/* Update graph state */
	assert(get_irg_phase_state(irg) != phase_building);

	free_irg_outs(irg);

	/* This first iteration counts the overall number of out edges and the
	   number of out edges for each node. */
	n_out_edges = count_outs(irg);

	/* allocate memory for all out edges. */
	irg->outs = XMALLOCNZ(ir_def_use_edge, n_out_edges);
#ifdef DEBUG_libfirm
	irg->n_outs = n_out_edges;
#endif

	/* The second iteration splits the irg->outs array into smaller arrays
	   for each node and writes the back edges into this array. */
	end = set_out_edges(irg, irg->outs);

	/* Check how much memory we have used */
	assert (end == (irg->outs + n_out_edges));

	add_irg_properties(irg, IR_GRAPH_PROPERTY_CONSISTENT_OUTS);
}

void assure_irg_outs(ir_graph *irg)
{
	if (! irg_has_properties(irg, IR_GRAPH_PROPERTY_CONSISTENT_OUTS))
		compute_irg_outs(irg);
}

void compute_irp_outs(void)
{
	size_t n = get_irp_n_irgs();
	for (size_t i = 0; i < n; ++i)
		compute_irg_outs(get_irp_irg(i));
}

void free_irp_outs(void)
{
	size_t n = get_irp_n_irgs();
	for (size_t i = 0; i < n; ++i)
		free_irg_outs(get_irp_irg(i));
}

#ifdef DEBUG_libfirm
/** Clear the outs of a node */
static void reset_outs(ir_node *node, void *unused)
{
	(void) unused;
	node->out       = NULL;
	node->out_valid = 0;
}
#endif

void free_irg_outs(ir_graph *irg)
{
	if (irg->outs != NULL) {
#ifdef DEBUG_libfirm
		memset(irg->outs, 0, irg->n_outs);
		irg->n_outs = 0;
#endif
		free(irg->outs);
		irg->outs = NULL;
	}

#ifdef DEBUG_libfirm
	/* when debugging, *always* reset all nodes' outs!  irg->outs might
	   have been lying to us */
	irg_walk_graph (irg, reset_outs, NULL, NULL);
#endif
}
