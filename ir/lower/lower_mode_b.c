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
 * @brief       lower mode_b operations to something the backend can handle
 * @author      Matthias Braun, Christoph Mallon
 */
#include "config.h"

#include "lower_mode_b.h"

#include <stdlib.h>
#include <stdbool.h>

#include "irnode_t.h"
#include "ircons_t.h"
#include "irflag.h"
#include "irgwalk.h"
#include "irtools.h"
#include "iredges.h"
#include "iropt_t.h"
#include "irgmod.h"
#include "tv.h"
#include "error.h"
#include "lowering.h"
#include "pdeq.h"
#include "irpass_t.h"
#include "util.h"
#include "array.h"
#include "irgopt.h"

typedef struct needs_lowering_t {
	ir_node *node;
	int      input;
} needs_lowering_t;

static ir_mode          *lowered_mode;
static needs_lowering_t *needs_lowering;

static ir_node *create_not(dbg_info *dbgi, ir_node *node)
{
	ir_node   *block  = get_nodes_block(node);
	ir_mode   *mode   = lowered_mode;
	ir_tarval *tv_one = get_mode_one(mode);
	ir_graph  *irg    = get_irn_irg(node);
	ir_node   *one    = new_rd_Const(dbgi, irg, tv_one);

	return new_rd_Eor(dbgi, block, node, one, mode);
}

static ir_node *convert_to_modeb(ir_node *node)
{
	ir_node   *block   = get_nodes_block(node);
	ir_graph  *irg     = get_irn_irg(node);
	ir_mode   *mode    = lowered_mode;
	ir_tarval *tv_zero = get_mode_null(mode);
	ir_node   *zero    = new_r_Const(irg, tv_zero);
	ir_node   *cmp     = new_r_Cmp(block, node, zero, ir_relation_less_greater);
	return cmp;
}

/**
 * implementation of create_set_func which produces a cond with control
 * flow
 */
static ir_node *create_cond_set(ir_node *cond_value, ir_mode *dest_mode)
{
	ir_node  *lower_block = part_block_edges(cond_value);
	ir_node  *upper_block = get_nodes_block(cond_value);
	ir_graph *irg         = get_irn_irg(cond_value);
	ir_node  *cond        = new_r_Cond(upper_block, cond_value);
	ir_node  *proj_true   = new_r_Proj(cond, mode_X, pn_Cond_true);
	ir_node  *proj_false  = new_r_Proj(cond, mode_X, pn_Cond_false);
	ir_node  *in_true[1]  = { proj_true };
	ir_node  *in_false[1] = { proj_false };
	ir_node  *true_block  = new_r_Block(irg, ARRAY_SIZE(in_true), in_true);
	ir_node  *false_block = new_r_Block(irg, ARRAY_SIZE(in_false),in_false);
	ir_node  *true_jmp    = new_r_Jmp(true_block);
	ir_node  *false_jmp   = new_r_Jmp(false_block);
	ir_node  *lower_in[2] = { true_jmp, false_jmp };
	ir_node  *one         = new_r_Const(irg, get_mode_one(dest_mode));
	ir_node  *zero        = new_r_Const(irg, get_mode_null(dest_mode));
	ir_node  *phi_in[2]   = { one, zero };
	ir_node  *phi;

	set_irn_in(lower_block, ARRAY_SIZE(lower_in), lower_in);
	phi = new_r_Phi(lower_block, ARRAY_SIZE(phi_in), phi_in, dest_mode);

	return phi;
}

static ir_node *lower_node(ir_node *node)
{
	dbg_info *dbgi  = get_irn_dbg_info(node);
	ir_node  *block = get_nodes_block(node);
	ir_mode  *mode  = lowered_mode;
	ir_node  *res   = (ir_node*)get_irn_link(node);
	ir_graph *irg;

	if (res != NULL)
		return res;

	node = skip_Tuple(node);

	assert(get_irn_mode(node) == mode_b);

	irg = get_irn_irg(node);
	switch (get_irn_opcode(node)) {
	case iro_Phi: {
		int       i, arity;
		ir_node **in;
		ir_node  *dummy;
		ir_node  *new_phi;

		arity = get_irn_arity(node);
		in    = ALLOCAN(ir_node*, arity);
		dummy = new_r_Dummy(irg, mode);
		for (i = 0; i < arity; ++i) {
			in[i] = dummy;
		}
		new_phi = new_r_Phi(block, arity, in, mode);
		/* FIXME This does not correctly break cycles: The Phi might not be the
		 * first in the recursion, so the caller(s) are some yet un-lowered
		 * nodes and this Phi might have them (indirectly) as operands, so they
		 * would be replaced twice. */
		set_irn_link(node, new_phi);

		for (i = 0; i < arity; ++i) {
			ir_node *in         = get_irn_n(node, i);
			ir_node *lowered_in = lower_node(in);

			set_irn_n(new_phi, i, lowered_in);
		}

		return new_phi;
	}

	case iro_And: {
		ir_node *lowered_left  = lower_node(get_And_left(node));
		ir_node *lowered_right = lower_node(get_And_right(node));
		res = new_rd_And(dbgi, block, lowered_left, lowered_right, mode);
		break;
	}
	case iro_Or: {
		ir_node *lowered_left  = lower_node(get_Or_left(node));
		ir_node *lowered_right = lower_node(get_Or_right(node));
		res = new_rd_Or(dbgi, block, lowered_left, lowered_right, mode);
		break;
	}
	case iro_Eor: {
		ir_node *lowered_left  = lower_node(get_Eor_left(node));
		ir_node *lowered_right = lower_node(get_Eor_right(node));
		res = new_rd_Eor(dbgi, block, lowered_left, lowered_right, mode);
		break;
	}

	case iro_Not: {
		ir_node *op     = get_Not_op(node);
		ir_node *low_op = lower_node(op);

		res = create_not(dbgi, low_op);
		break;
	}

	case iro_Mux: {
		ir_node *cond        = get_Mux_sel(node);
		ir_node *low_cond    = lower_node(cond);
		ir_node *v_true      = get_Mux_true(node);
		ir_node *low_v_true  = lower_node(v_true);
		ir_node *v_false     = get_Mux_false(node);
		ir_node *low_v_false = lower_node(v_false);

		ir_node *and0     = new_rd_And(dbgi, block, low_cond, low_v_true, mode);
		ir_node *not_cond = create_not(dbgi, low_cond);
		ir_node *and1     = new_rd_And(dbgi, block, not_cond, low_v_false, mode);
		res = new_rd_Or(dbgi, block, and0, and1, mode);
		break;
	}

	case iro_Cmp:
		res = create_cond_set(node, mode);
		break;

	case iro_Const: {
		ir_tarval *tv = get_Const_tarval(node);
		if (tv == get_tarval_b_true()) {
			ir_tarval *tv_one = get_mode_one(mode);
			res               = new_rd_Const(dbgi, irg, tv_one);
		} else if (tv == get_tarval_b_false()) {
			ir_tarval *tv_zero = get_mode_null(mode);
			res                = new_rd_Const(dbgi, irg, tv_zero);
		} else {
			panic("invalid boolean const %+F", node);
		}
		break;
	}

	case iro_Unknown:
		res = new_r_Unknown(irg, mode);
		break;

	case iro_Bad:
		res = new_r_Bad(irg, mode);
		break;

	default:
		panic("Don't know how to lower mode_b node %+F", node);
	}

	set_irn_link(node, res);
	return res;
}

static bool needs_mode_b_input(const ir_node *node, int input)
{
	return (is_Cond(node) && input == n_Cond_selector)
	    || (is_Mux(node) && input == n_Mux_sel);
}

/**
 * Collects "roots" of a mode_b calculation. These are nodes which require a
 * mode_b input (Cond, Mux)
 */
static void collect_needs_lowering(ir_node *node, void *env)
{
	int arity = get_irn_arity(node);
	int i;
	(void) env;

	/* if the node produces mode_b then it is not a root (but should be
	 * something our lower_node function can handle) */
	if (get_irn_mode(node) == mode_b) {
		assert(is_And(node) || is_Or(node) || is_Eor(node) || is_Phi(node)
		       || is_Not(node) || is_Mux(node) || is_Cmp(node)
		       || is_Const(node) || is_Unknown(node) || is_Bad(node));
		return;
	}

	for (i = 0; i < arity; ++i) {
		needs_lowering_t entry;
		ir_node *in = get_irn_n(node, i);
		if (get_irn_mode(in) != mode_b)
			continue;
		if (is_Cmp(in) && needs_mode_b_input(node, i))
			continue;

		entry.node  = node;
		entry.input = i;
		ARR_APP1(needs_lowering_t, needs_lowering, entry);
	}
}

void ir_lower_mode_b(ir_graph *const irg, ir_mode *const nlowered_mode)
{
	size_t i;
	size_t n;

	lowered_mode = nlowered_mode;

	/* edges are used by part_block_edges in the ir_create_cond_set variant. */
	edges_assure(irg);
	/* part_block_edges can go wrong with tuples present */
	remove_tuples(irg);

	set_irg_state(irg, IR_GRAPH_STATE_MODEB_LOWERED);
	ir_reserve_resources(irg, IR_RESOURCE_IRN_LINK);

	needs_lowering = NEW_ARR_F(needs_lowering_t, 0);

	irg_walk_graph(irg, firm_clear_link, collect_needs_lowering, NULL);

	n = ARR_LEN(needs_lowering);
	for (i = 0; i < n; ++i) {
		const needs_lowering_t *entry   = &needs_lowering[i];
		ir_node                *node    = entry->node;
		int                     input   = entry->input;
		ir_node                *in      = get_irn_n(node, input);
		ir_node                *lowered = lower_node(in);

		if (needs_mode_b_input(node, input))
			lowered = convert_to_modeb(lowered);
		set_irn_n(node, input, lowered);
	}

	ir_free_resources(irg, IR_RESOURCE_IRN_LINK);

	DEL_ARR_F(needs_lowering);

	if (n > 0) {
		/* lowering might create new blocks, so be sure to handle this */
		clear_irg_state(irg, IR_GRAPH_STATE_CONSISTENT_DOMINANCE
		                   | IR_GRAPH_STATE_VALID_EXTENDED_BLOCKS);
		edges_deactivate(irg);
	}
}
