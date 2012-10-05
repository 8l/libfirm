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
 * @brief       Peephole optimisation framework keeps track of which registers contain which values
 * @author      Matthias Braun
 */
#include "config.h"

#include "array_t.h"
#include "bepeephole.h"

#include "iredges_t.h"
#include "irgwalk.h"
#include "irprintf.h"
#include "ircons.h"
#include "irgmod.h"
#include "heights.h"
#include "error.h"

#include "beirg.h"
#include "belive_t.h"
#include "bearch.h"
#include "beintlive_t.h"
#include "benode.h"
#include "besched.h"
#include "bemodule.h"

DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

static const arch_env_t *arch_env;
static be_lv_t          *lv;
static ir_node          *current_node;
ir_node                **register_values;

static void clear_reg_value(ir_node *node)
{
	const arch_register_t *reg;
	unsigned               reg_idx;

	if (!mode_is_data(get_irn_mode(node)))
		return;

	reg = arch_get_irn_register(node);
	if (reg == NULL) {
		panic("No register assigned at %+F", node);
	}
	if (reg->type & arch_register_type_virtual)
		return;
	reg_idx = reg->global_index;

	DB((dbg, LEVEL_1, "Clear Register %s\n", reg->name));
	register_values[reg_idx] = NULL;
}

static void set_reg_value(ir_node *node)
{
	const arch_register_t *reg;
	unsigned               reg_idx;

	if (!mode_is_data(get_irn_mode(node)))
		return;

	reg = arch_get_irn_register(node);
	if (reg == NULL) {
		panic("No register assigned at %+F", node);
	}
	if (reg->type & arch_register_type_virtual)
		return;
	reg_idx = reg->global_index;

	DB((dbg, LEVEL_1, "Set Register %s: %+F\n", reg->name, node));
	register_values[reg_idx] = node;
}

static void clear_defs(ir_node *node)
{
	/* clear values defined */
	if (get_irn_mode(node) == mode_T) {
		foreach_out_edge(node, edge) {
			ir_node *proj = get_edge_src_irn(edge);
			clear_reg_value(proj);
		}
	} else {
		clear_reg_value(node);
	}
}

static void set_uses(ir_node *node)
{
	int i, arity;

	/* set values used */
	arity = get_irn_arity(node);
	for (i = 0; i < arity; ++i) {
		ir_node *in = get_irn_n(node, i);
		set_reg_value(in);
	}
}

void be_peephole_new_node(ir_node * nw)
{
	be_liveness_introduce(lv, nw);
}

/**
 * must be called from peephole optimisations before a node will be killed
 * and its users will be redirected to new_node.
 * so bepeephole can update its internal state.
 *
 * Note: killing a node and rewiring is only allowed if new_node produces
 * the same registers as old_node.
 */
static void be_peephole_before_exchange(const ir_node *old_node,
                                        ir_node *new_node)
{
	const arch_register_t *reg;
	unsigned               reg_idx;
	bool                   old_is_current = false;

	DB((dbg, LEVEL_1, "About to exchange and kill %+F with %+F\n", old_node, new_node));

	assert(sched_is_scheduled(skip_Proj_const(old_node)));
	assert(sched_is_scheduled(skip_Proj(new_node)));

	if (current_node == old_node) {
		old_is_current = true;

		/* next node to be processed will be killed. Its scheduling predecessor
		 * must be processed next. */
		current_node = sched_next(current_node);
		assert (!is_Bad(current_node));

		/* we can't handle liveness updates correctly when exchange current node
		 * with something behind it */
		assert(value_dominates(skip_Proj(new_node), skip_Proj_const(old_node)));
	}

	if (!mode_is_data(get_irn_mode(old_node)))
		return;

	reg = arch_get_irn_register(old_node);
	if (reg == NULL) {
		panic("No register assigned at %+F", old_node);
	}
	assert(reg == arch_get_irn_register(new_node) &&
	      "KILLING a node and replacing by different register is not allowed");

	reg_idx = reg->global_index;
	if (register_values[reg_idx] == old_node || old_is_current) {
		register_values[reg_idx] = new_node;
	}

	be_liveness_remove(lv, old_node);
}

void be_peephole_exchange(ir_node *old, ir_node *nw)
{
	be_peephole_before_exchange(old, nw);
	sched_remove(old);
	exchange(old, nw);
	be_peephole_new_node(nw);
}

/**
 * block-walker: run peephole optimization on the given block.
 */
static void process_block(ir_node *block, void *data)
{
	(void) data;

	/* construct initial register assignment */
	memset(register_values, 0, sizeof(ir_node*) * arch_env->n_registers);

	assert(lv->sets_valid && "live sets must be computed");
	DB((dbg, LEVEL_1, "\nProcessing block %+F (from end)\n", block));
	be_lv_foreach(lv, block, be_lv_state_end, node) {
		set_reg_value(node);
	}
	DB((dbg, LEVEL_1, "\nstart processing\n"));

	/* walk the block from last insn to the first */
	current_node = sched_last(block);
	for ( ; !sched_is_begin(current_node);
			current_node = sched_prev(current_node)) {
		ir_op             *op;
		peephole_opt_func  peephole_node;

		assert(!is_Bad(current_node));
		if (is_Phi(current_node))
			break;

		clear_defs(current_node);
		set_uses(current_node);

		op            = get_irn_op(current_node);
		peephole_node = (peephole_opt_func)op->ops.generic;
		if (peephole_node == NULL)
			continue;

		DB((dbg, LEVEL_2, "optimize %+F\n", current_node));
		peephole_node(current_node);
		assert(!is_Bad(current_node));
	}
}

/**
 * Check whether the node has only one user.  Explicitly ignore the anchor.
 */
bool be_has_only_one_user(ir_node *node)
{
	int n = get_irn_n_edges(node);
	int n_users;

	if (n <= 1)
		return 1;

	n_users = 0;
	foreach_out_edge(node, edge) {
		ir_node *src = get_edge_src_irn(edge);
		/* ignore anchor and keep-alive edges */
		if (is_Anchor(src) || is_End(src))
			continue;
		n_users++;
	}

	return n_users == 1;
}

static inline bool overlapping_regs(const arch_register_t *reg0,
	const arch_register_req_t *req0, const arch_register_t *reg1,
	const arch_register_req_t *req1)
{
	if (reg0 == NULL || reg1 == NULL)
		return false;
	return reg0->global_index < (unsigned)reg1->global_index + req1->width
		&& reg1->global_index < (unsigned)reg0->global_index + req0->width;
}

bool be_can_move_down(ir_heights_t *heights, const ir_node *node,
                      const ir_node *before)
{
	assert(get_nodes_block(node) == get_nodes_block(before));
	assert(sched_get_time_step(node) < sched_get_time_step(before));

	int      node_arity = get_irn_arity(node);
	ir_node *schedpoint = sched_next(node);

	while (schedpoint != before) {
		/* schedpoint must not use our computed values */
		if (heights_reachable_in_block(heights, schedpoint, node))
			return false;

		/* schedpoint must not overwrite registers of our inputs */
		unsigned n_outs = arch_get_irn_n_outs(schedpoint);
		for (int i = 0; i < node_arity; ++i) {
			ir_node                   *in  = get_irn_n(node, i);
			const arch_register_t     *reg = arch_get_irn_register(in);
			if (reg == NULL)
				continue;
			const arch_register_req_t *in_req
				= arch_get_irn_register_req_in(node, i);
			for (unsigned o = 0; o < n_outs; ++o) {
				const arch_register_t *outreg
					= arch_get_irn_register_out(schedpoint, o);
				const arch_register_req_t *outreq
					= arch_get_irn_register_req_out(schedpoint, o);
				if (overlapping_regs(reg, in_req, outreg, outreq))
					return false;
			}
		}

		schedpoint = sched_next(schedpoint);
	}
	return true;
}

bool be_can_move_up(ir_heights_t *heights, const ir_node *node,
                    const ir_node *after)
{
	unsigned       n_outs      = arch_get_irn_n_outs(node);
	const ir_node *node_block  = get_nodes_block(node);
	const ir_node *after_block = get_block_const(after);
	const ir_node *schedpoint;
	if (node_block != after_block) {
		/* currently we can move up exactly 1 block */
		assert(get_Block_cfgpred_block(node_block, 0) == after_block);
		ir_node *first = sched_first(node_block);

		/* do not move nodes changing memory */
		if (is_memop(node)) {
			ir_node *meminput = get_memop_mem(node);
			if (!is_NoMem(meminput))
				return false;
		}

		/* make sure we can move to the beginning of the succ block */
		if (node != first && !be_can_move_up(heights, node, sched_prev(first)))
			return false;

		/* check if node overrides any of live-in values of other successors */
		ir_graph *irg = get_irn_irg(node);
		be_lv_t  *lv  = be_get_irg_liveness(irg);
		foreach_block_succ(after_block, edge) {
			ir_node *succ = get_edge_src_irn(edge);
			if (succ == node_block)
				continue;

			be_lv_foreach(lv, succ, be_lv_state_in, live_node) {
				const arch_register_t     *reg = arch_get_irn_register(live_node);
				const arch_register_req_t *req = arch_get_irn_register_req(live_node);
				for (unsigned o = 0; o < n_outs; ++o) {
					const arch_register_t *outreg
						= arch_get_irn_register_out(node, o);
					const arch_register_req_t *outreq
						= arch_get_irn_register_req_out(node, o);
					if (overlapping_regs(outreg, outreq, reg, req))
						return false;
				}
			}
			sched_foreach(succ, phi) {
				if (!is_Phi(phi))
					break;
				const arch_register_t     *reg = arch_get_irn_register(phi);
				const arch_register_req_t *req = arch_get_irn_register_req(phi);
				for (unsigned o = 0; o < n_outs; ++o) {
					const arch_register_t *outreg
						= arch_get_irn_register_out(node, o);
					const arch_register_req_t *outreq
						= arch_get_irn_register_req_out(node, o);
					if (overlapping_regs(outreg, outreq, reg, req))
						return false;
				}
			}
		}
		schedpoint = sched_last(after_block);
	} else {
		schedpoint = sched_prev(node);
	}

	/* move schedule upwards until we hit the "after" node */
	while (schedpoint != after) {
		/* TODO: the following heights query only works for nodes in the same
		 * block, otherwise we have to be conservative here */
		if (get_nodes_block(node) != get_nodes_block(schedpoint))
			return false;
		/* node must not depend on schedpoint */
		if (heights_reachable_in_block(heights, node, schedpoint))
			return false;

		/* node must not overwrite registers used by schedpoint */
		int arity = get_irn_arity(schedpoint);
		for (int i = 0; i < arity; ++i) {
			const arch_register_t *reg
				= arch_get_irn_register_in(schedpoint, i);
			if (reg == NULL)
				continue;
			const arch_register_req_t *in_req
				= arch_get_irn_register_req_in(schedpoint, i);
			for (unsigned o = 0; o < n_outs; ++o) {
				const arch_register_t *outreg
					= arch_get_irn_register_out(node, o);
				const arch_register_req_t *outreq
					= arch_get_irn_register_req_out(node, o);
				if (overlapping_regs(outreg, outreq, reg, in_req))
					return false;
			}
		}

		schedpoint = sched_prev(schedpoint);
	}
	return true;
}

/*
 * Tries to optimize a beIncSP node with its previous IncSP node.
 * Must be run from a be_peephole_opt() context.
 */
ir_node *be_peephole_IncSP_IncSP(ir_node *node)
{
	int      pred_offs;
	int      curr_offs;
	int      offs;
	ir_node *pred = be_get_IncSP_pred(node);

	if (!be_is_IncSP(pred))
		return node;

	if (!be_has_only_one_user(pred))
		return node;

	pred_offs = be_get_IncSP_offset(pred);
	curr_offs = be_get_IncSP_offset(node);
	offs = curr_offs + pred_offs;

	/* add node offset to pred and remove our IncSP */
	be_set_IncSP_offset(pred, offs);

	be_peephole_exchange(node, pred);
	return pred;
}

void be_peephole_opt(ir_graph *irg)
{
#if 0
	/* we sometimes find BadE nodes in float apps like optest_float.c or
	 * kahansum.c for example... */
	be_invalidate_live_sets(irg);
#endif
	be_assure_live_sets(irg);

	arch_env = be_get_irg_arch_env(irg);
	lv       = be_get_irg_liveness(irg);

	register_values = XMALLOCN(ir_node*, arch_env->n_registers);

	irg_block_walk_graph(irg, process_block, NULL, NULL);

	xfree(register_values);
}

BE_REGISTER_MODULE_CONSTRUCTOR(be_init_peephole)
void be_init_peephole(void)
{
	FIRM_DBG_REGISTER(dbg, "firm.be.peephole");
}
