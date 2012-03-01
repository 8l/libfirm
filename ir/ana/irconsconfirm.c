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
 * @brief    Construction of Confirm nodes
 * @author   Michael Beck
 * @date     6.2005
 */
#include "config.h"

#include "irconsconfirm.h"

#include "irgraph_t.h"
#include "irnode_t.h"
#include "ircons_t.h"
#include "irgmod.h"
#include "iropt_dbg.h"
#include "iredges_t.h"
#include "irgwalk.h"
#include "irprintf.h"
#include "irgopt.h"
#include "irpass.h"
#include "irtools.h"
#include "array_t.h"
#include "debug.h"
#include "error.h"
#include "irflag.h"
#include "opt_manage.h"

/**
 * Walker environment.
 */
typedef struct env_t {
	unsigned num_confirms;  /**< Number of inserted Confirm nodes. */
	unsigned num_consts;    /**< Number of constants placed. */
	unsigned num_eq;        /**< Number of equalities placed. */
	unsigned num_non_null;  /**< Number of non-null Confirms. */
} env_t;

/** The debug handle. */
DEBUG_ONLY(static firm_dbg_module_t *dbg;)

/**
 * Return the effective use block of a node and its predecessor on
 * position pos.
 *
 * @param node  the node
 * @param pos   the position of the used input
 *
 * This handles correctly Phi nodes.
 */
static ir_node *get_effective_use_block(ir_node *node, int pos)
{
	if (is_Phi(node)) {
		/* the effective use of a Phi argument is in its predecessor block */
		node = get_nodes_block(node);
		return get_Block_cfgpred_block(node, pos);
	}
	return get_nodes_block(node);
}

static ir_node *get_case_value(ir_node *switchn, long pn)
{
	ir_graph              *irg       = get_irn_irg(switchn);
	const ir_switch_table *table     = get_Switch_table(switchn);
	size_t                 n_entries = ir_switch_table_get_n_entries(table);
	ir_tarval             *val       = NULL;
	size_t                 e;
	for (e = 0; e < n_entries; ++e) {
		const ir_switch_table_entry *entry
			= ir_switch_table_get_entry_const(table, e);
		if (entry->pn != pn)
			continue;
		/* multiple matching entries gets too complicated for a single
		 * Confirm */
		if (val != NULL)
			return NULL;
		/* case ranges are too complicated too */
		if (entry->min != entry->max)
			return NULL;
		val = entry->min;
	}
	assert(val != NULL);
	return new_r_Const(irg, val);
}

/**
 * Handle a CASE-branch.
 *
 * Branch labels are a simple case. We can replace the value
 * by a Const with the branch label.
 */
static void handle_case(ir_node *block, ir_node *switchn, long pn, env_t *env)
{
	ir_node         *c = NULL;
	ir_node         *selector = get_Switch_selector(switchn);
	const ir_edge_t *edge;
	const ir_edge_t *next;

	/* we can't do usefull things with the default label */
	if (pn == pn_Switch_default)
		return;

	foreach_out_edge_safe(selector, edge, next) {
		ir_node *succ = get_edge_src_irn(edge);
		int     pos   = get_edge_src_pos(edge);
		ir_node *blk  = get_effective_use_block(succ, pos);

		if (block_dominates(block, blk)) {
			/*
			 * Ok, we found a user of irn that is placed
			 * in a block dominated by the branch block.
			 * We can replace the input with the Constant
			 * branch label.
			 */
			if (c == NULL)
				c = get_case_value(switchn, pn);

			set_irn_n(succ, pos, c);
			DBG_OPT_CONFIRM_C(selector, c);
			DB((dbg, LEVEL_2, "Replacing input %d of node %+F with %+F\n", pos, succ, c));

			env->num_consts += 1;
		}
	}
}

/**
 * Handle a mode_b input of Cond nodes.
 *
 * @param block     the block which is entered by the branch
 * @param selector  the mode_b node expressing the branch condition
 * @param pnc       the true/false condition branch
 * @param env       statistical environment
 */
static void handle_modeb(ir_node *block, ir_node *selector, pn_Cond pnc, env_t *env)
{
	ir_node *cond, *old, *other_blk = NULL, *con = NULL;
	ir_node *c_b = NULL, *c_o = NULL;
	const ir_edge_t *edge, *next;

	for (edge = get_irn_out_edge_first(selector); edge; edge = next) {
		ir_node *user     = get_edge_src_irn(edge);
		int     pos       = get_edge_src_pos(edge);
		ir_node *user_blk = get_effective_use_block(user, pos);

		next = get_irn_out_edge_next(selector, edge);
		if (block_dominates(block, user_blk)) {
			/*
			 * Ok, we found a usage of selector in a block
			 * dominated by the branch block.
			 * We can replace the input with true/false.
			 */
			if (con == NULL) {
				ir_graph *irg = get_irn_irg(block);
				con = new_r_Const(irg, pnc == pn_Cond_true ? tarval_b_true : tarval_b_false);
			}
			old = get_irn_n(user, pos);
			set_irn_n(user, pos, con);
			DBG_OPT_CONFIRM_C(old, con);

			DB((dbg, LEVEL_2, "Replacing input %d of node %+F with %+F\n", pos, user, con));

			env->num_consts += 1;
		} else {
			int i, n;

			/* get the other block */
			if (other_blk == NULL) {
				/* we have already tested, that block has only ONE Cond predecessor */
				cond = get_Proj_pred(get_Block_cfgpred(block, 0));
				foreach_out_edge(cond, edge) {
					ir_node *proj = get_edge_src_irn(edge);
					if (get_Proj_proj(proj) == (long)pnc)
						continue;
					edge = get_irn_out_edge_first(proj);
					other_blk = get_edge_src_irn(edge);
					break;
				}
				assert(other_blk);

				/*
				 * Note the special case here: if block is a then, there might be no else
				 * block. In that case the other_block is the user_blk itself and pred_block
				 * is the cond_block ...
				 *
				 * Best would be to introduce a block here, removing this critical edge.
				 * For some reasons I cannot repair dominance here, so I have to remove
				 * ALL critical edges...
				 * FIXME: This should not be needed if we could repair dominance ...
				 */
			}

			n = get_Block_n_cfgpreds(user_blk);

			/*
			 * We have found a user in a non-dominated block:
			 * check, if all its block predecessors are dominated.
			 * If yes, place a Phi.
			 */
			for (i = n - 1; i >= 0; --i) {
				ir_node *pred_blk = get_Block_cfgpred_block(user_blk, i);

				if (!block_dominates(block, pred_blk) &&
				    !block_dominates(other_blk, pred_blk)) {
					/* can't do anything */
					break;
				}
			}
			if (i < 0) {
				ir_node *phi, **in;

				NEW_ARR_A(ir_node *, in, n);
				/* ok, ALL predecessors are either dominated by block OR other block */
				if (c_b == NULL) {
					ir_graph *irg    = get_irn_irg(block);
					ir_node *c_true  = new_r_Const(irg, tarval_b_true);
					ir_node *c_false = new_r_Const(irg, tarval_b_false);
					env->num_consts += 2;
					if (pnc == pn_Cond_true) {
						c_b = c_true;
						c_o = c_false;
					} else {
						c_b = c_false;
						c_o = c_true;
					}
				}
				for (i = n - 1; i >= 0; --i) {
					ir_node *pred_blk = get_Block_cfgpred_block(user_blk, i);

					if (block_dominates(block, pred_blk))
						in[i] = c_b;
					else
						in[i] = c_o;
				}
				phi = new_r_Phi(user_blk, n, in, mode_b);
				set_irn_n(user, pos, phi);
				env->num_eq += 1;
			}
		}
	}
}

/**
 * Handle an IF-branch.
 *
 * @param block   the block which is entered by the branch
 * @param cmp     the Cmp node expressing the branch condition
 * @param rel     the Compare relation for taking this branch
 * @param env     statistical environment
 */
static void handle_if(ir_node *block, ir_node *cmp, ir_relation rel, env_t *env)
{
	ir_node *left  = get_Cmp_left(cmp);
	ir_node *right = get_Cmp_right(cmp);
	ir_node *cond_block;
	ir_op *op;
	const ir_edge_t *edge, *next;

	/* Beware of Bads */
	if (is_Bad(left) || is_Bad(right))
		return;

	op = get_irn_op(left);

	/* Do not create Confirm nodes for Cmp(Const, Const) constructs.
	   These are removed anyway */
	if (op == op_Const && is_Const(right))
		return;

	/* try to place the constant on the right side for a Confirm */
	if (op == op_Const || op == op_SymConst) {
		ir_node *t = left;

		left  = right;
		right = t;

		rel = get_inversed_relation(rel);
	}

	/*
	 * First case: both values are identical.
	 * replace the left one by the right (potentially const) one.
	 */
	if (rel == ir_relation_equal) {
		cond_block = get_Block_cfgpred_block(block, 0);
		for (edge = get_irn_out_edge_first(left); edge; edge = next) {
			ir_node *user = get_edge_src_irn(edge);
			int     pos   = get_edge_src_pos(edge);
			ir_node *blk  = get_effective_use_block(user, pos);

			next = get_irn_out_edge_next(left, edge);
			if (block_dominates(block, blk)) {
				/*
				 * Ok, we found a usage of left in a block
				 * dominated by the branch block.
				 * We can replace the input with right.
				 */
				set_irn_n(user, pos, right);
				DBG_OPT_CONFIRM(left, right);

				DB((dbg, LEVEL_2, "Replacing input %d of node %+F with %+F\n", pos, user, right));

				env->num_eq += 1;
			} else if (block_dominates(blk, cond_block)
					&& is_Const(right)
					&& get_irn_pinned(user) == op_pin_state_floats) {
				/*
				 * left == Const and we found a movable user of left in a
				 * dominator of the Cond block
				 */
				const ir_edge_t *user_edge;
				const ir_edge_t *user_next;
				foreach_out_edge_safe(user, user_edge, user_next) {
					ir_node *usr_of_usr = get_edge_src_irn(user_edge);
					int      npos       = get_edge_src_pos(user_edge);
					ir_node *user_blk   = get_effective_use_block(usr_of_usr, npos);

					if (block_dominates(block, user_blk)) {
						/*
						 * The user of the user is dominated by our true/false
						 * block. So, create a copy of user WITH the constant
						 * replacing its pos'th input.
						 *
						 * This is always good for unop's and might be good
						 * for binops.
						 *
						 * If user has other user in the false/true block, code
						 * placement will move it down.
						 * If there are users in cond block or upper, we create
						 * "redundant ops", because one will have a const op,
						 * the other no const ...
						 */
						ir_node *new_op = exact_copy(user);
						set_nodes_block(new_op, block);
						set_irn_n(new_op, pos, right);
						set_irn_n(usr_of_usr, npos, new_op);
						env->num_eq += 1;
					}
				}
			}
		}
	} else { /* not ir_relation_equal cases */
		ir_node *c = NULL;

		foreach_out_edge_safe(left, edge, next) {
			ir_node *succ = get_edge_src_irn(edge);
			int     pos   = get_edge_src_pos(edge);
			ir_node *blk  = get_effective_use_block(succ, pos);

			if (block_dominates(block, blk)) {
				/*
				 * Ok, we found a usage of left in a block
				 * dominated by the branch block.
				 * We can replace the input with a Confirm(left, pnc, right).
				 */
				if (! c)
					c = new_r_Confirm(block, left, right, rel);

				pos = get_edge_src_pos(edge);
				set_irn_n(succ, pos, c);
				DB((dbg, LEVEL_2, "Replacing input %d of node %+F with %+F\n", pos, succ, c));

				env->num_confirms += 1;
			}
		}

		if (! is_Const(right)) {
			/* also construct inverse Confirms */
			ir_node *rc = NULL;

			rel = get_inversed_relation(rel);
			foreach_out_edge_safe(right, edge, next) {
				ir_node *succ = get_edge_src_irn(edge);
				int     pos;
				ir_node *blk;

				if (succ == c)
					continue;

				pos  = get_edge_src_pos(edge);
				blk  = get_effective_use_block(succ, pos);

				if (block_dominates(block, blk)) {
					/*
					 * Ok, we found a usage of right in a block
					 * dominated by the branch block.
					 * We can replace the input with a Confirm(right, rel^-1, left).
					 */
					if (! rc)
						rc = new_r_Confirm(block, right, left, rel);

					pos = get_edge_src_pos(edge);
					set_irn_n(succ, pos, rc);
					DB((dbg, LEVEL_2, "Replacing input %d of node %+F with %+F\n", pos, succ, rc));

					env->num_confirms += 1;
				}
			}
		}
	}
}

/**
 * Pre-block-walker: Called for every block to insert Confirm nodes
 */
static void insert_Confirm_in_block(ir_node *block, void *data)
{
	env_t   *env = (env_t*) data;
	ir_node *cond;
	ir_node *proj;

	/*
	 * we can only handle blocks with only ONE control flow
	 * predecessor yet.
	 */
	if (get_Block_n_cfgpreds(block) != 1)
		return;

	proj = get_Block_cfgpred(block, 0);
	if (! is_Proj(proj))
		return;

	cond = get_Proj_pred(proj);
	if (is_Switch(cond)) {
		long proj_nr = get_Proj_proj(proj);
		handle_case(block, cond, proj_nr, env);
	} else if (is_Cond(cond)) {
		ir_node *selector = get_Cond_selector(cond);
		ir_relation rel;

		handle_modeb(block, selector, (pn_Cond) get_Proj_proj(proj), env);

		if (! is_Cmp(selector))
			return;

		rel = get_Cmp_relation(selector);

		if (get_Proj_proj(proj) != pn_Cond_true) {
			/* it's the false branch */
			rel = get_negated_relation(rel);
		}
		DB((dbg, LEVEL_2, "At %+F using %+F Confirm %=\n", block, selector, rel));

		handle_if(block, selector, rel, env);
	}
}

/**
 * Checks if a node is a non-null Confirm.
 */
static int is_non_null_Confirm(const ir_node *ptr)
{
	for (;;) {
		if (! is_Confirm(ptr))
			break;
		if (get_Confirm_relation(ptr) == ir_relation_less_greater) {
			ir_node *bound = get_Confirm_bound(ptr);

			if (is_Const(bound) && is_Const_null(bound))
				return 1;
		}
		ptr = get_Confirm_value(ptr);
	}
	/*
	 * While a SymConst is not a Confirm, it is non-null
	 * anyway. This helps to reduce the number of
	 * constructed Confirms.
	 */
	if (is_SymConst_addr_ent(ptr))
		return 1;
	return 0;
}

/**
 * The given pointer will be dereferenced, add non-null Confirms.
 *
 * @param ptr    a node representing an address
 * @param block  the block of the dereferencing instruction
 * @param env    environment
 */
static void insert_non_null(ir_node *ptr, ir_node *block, env_t *env)
{
	const ir_edge_t *edge, *next;
	ir_node         *c = NULL;

	foreach_out_edge_safe(ptr, edge, next) {
		ir_node *succ = get_edge_src_irn(edge);
		int     pos;
		ir_node *blk;


		/* for now, we place a Confirm only in front of a Cmp */
		if (! is_Cmp(succ))
			continue;

		pos = get_edge_src_pos(edge);
		blk = get_effective_use_block(succ, pos);

		if (block_dominates(block, blk)) {
			/*
			 * Ok, we found a usage of ptr in a block
			 * dominated by the Load/Store block.
			 * We can replace the input with a Confirm(ptr, !=, NULL).
			 */
			if (c == NULL) {
				ir_mode  *mode = get_irn_mode(ptr);
				ir_graph *irg  = get_irn_irg(block);
				c = new_r_Const(irg, get_mode_null(mode));
				c = new_r_Confirm(block, ptr, c, ir_relation_less_greater);
			}

			set_irn_n(succ, pos, c);
			DB((dbg, LEVEL_2, "Replacing input %d of node %+F with %+F\n", pos, succ, c));

			env->num_non_null += 1;
			env->num_confirms += 1;
		}
	}
}

/**
 * Pre-walker: Called for every node to insert Confirm nodes
 */
static void insert_Confirm(ir_node *node, void *data)
{
	ir_node *ptr;
	env_t   *env = (env_t*) data;

	switch (get_irn_opcode(node)) {
	case iro_Block:
		insert_Confirm_in_block(node, env);
		break;
	case iro_Load:
		ptr = get_Load_ptr(node);
		if (! is_non_null_Confirm(ptr))
			insert_non_null(ptr, get_nodes_block(node), env);
		break;
	case iro_Store:
		ptr = get_Store_ptr(node);
		if (! is_non_null_Confirm(ptr))
			insert_non_null(ptr, get_nodes_block(node), env);
		break;
	default:
		break;
	}
}

/*
 * Construct Confirm nodes
 */
static ir_graph_state_t do_construct_confirms(ir_graph *irg)
{
	env_t env;
	FIRM_DBG_REGISTER(dbg, "firm.ana.confirm");

	assert(get_irg_pinned(irg) == op_pin_state_pinned &&
	       "Nodes must be placed to insert Confirms");

	env.num_confirms = 0;
	env.num_consts   = 0;
	env.num_eq       = 0;
	env.num_non_null = 0;

	if (get_opt_global_null_ptr_elimination()) {
		/* do global NULL test elimination */
		irg_walk_graph(irg, insert_Confirm, NULL, &env);
	} else {
		/* now, visit all blocks and add Confirms where possible */
		irg_block_walk_graph(irg, insert_Confirm_in_block, NULL, &env);
	}

	DB((dbg, LEVEL_1, "# Confirms inserted : %u\n", env.num_confirms));
	DB((dbg, LEVEL_1, "# Const replacements: %u\n", env.num_consts));
	DB((dbg, LEVEL_1, "# node equalities   : %u\n", env.num_eq));
	DB((dbg, LEVEL_1, "# non-null Confirms : %u\n", env.num_non_null));
	return 0;
}

static optdesc_t opt_confirms = {
	"confirms",
	IR_GRAPH_STATE_CONSISTENT_OUT_EDGES
	| IR_GRAPH_STATE_CONSISTENT_DOMINANCE
	| IR_GRAPH_STATE_NO_CRITICAL_EDGES,
	do_construct_confirms
};

void construct_confirms(ir_graph *irg)
{
	perform_irg_optimization(irg, &opt_confirms);
}

ir_graph_pass_t *construct_confirms_pass(const char *name)
{
	return def_graph_pass(name ? name : "confirm", construct_confirms);
}

static void remove_confirm(ir_node *n, void *env)
{
	ir_node *value;

	(void) env;
	if (!is_Confirm(n))
		return;

	value = get_Confirm_value(n);
	exchange(n, value);
}

void remove_confirms(ir_graph *irg)
{
	irg_walk_graph(irg, NULL, remove_confirm, NULL);
}

ir_graph_pass_t *remove_confirms_pass(const char *name)
{
	return def_graph_pass(name ? name : "rem_confirm", remove_confirms);
}
