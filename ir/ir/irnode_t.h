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
 * @brief   Representation of an intermediate operation -- private header.
 * @author  Martin Trapp, Christian Schaefer, Goetz Lindenmaier, Michael Beck
 * @version $Id$
 */
#ifndef FIRM_IR_IRNODE_T_H
#define FIRM_IR_IRNODE_T_H

#include "irtypes.h"
#include "irnode.h"
#include "irop_t.h"
#include "irgraph_t.h"
#include "irflag_t.h"
#include "array.h"
#include "iredges_t.h"

/**
 * Returns the array with the ins.  The content of the array may not be
 * changed.
 * Note that this function returns the whole in array including the
 * block predecessor. So, it is NOT symmetric with set_irn_in().
 */
ir_node **get_irn_in(const ir_node *node);

/**
 * The amount of additional space for custom data to be allocated upon creating a new node.
 */
extern unsigned firm_add_node_size;

/**
 * Sets the get_type_attr operation for an ir_op_ops.
 *
 * @param code   the opcode for the default operation
 * @param ops    the operations initialized
 *
 * @return
 *    The operations.
 */
ir_op_ops *firm_set_default_get_type_attr(unsigned code, ir_op_ops *ops);

/**
 * Sets the get_entity_attr operation for an ir_op_ops.
 *
 * @param code   the opcode for the default operation
 * @param ops    the operations initialized
 *
 * @return
 *    The operations.
 */
ir_op_ops *firm_set_default_get_entity_attr(unsigned code, ir_op_ops *ops);

/**
 * Returns an array with the predecessors of the Block. Depending on
 * the implementation of the graph data structure this can be a copy of
 * the internal representation of predecessors as well as the internal
 * array itself. Therefore writing to this array might obstruct the IR.
 */
ir_node **get_Block_cfgpred_arr(ir_node *node);

/*-------------------------------------------------------------------*/
/*  These function are most used in libfirm.  Give them as static    */
/*  functions so they can be inlined.                                */
/*-------------------------------------------------------------------*/

/**
 * Checks whether a pointer points to a ir node.
 * Intern version for libFirm.
 */
static inline int is_ir_node_(const void *thing)
{
	return (get_kind(thing) == k_ir_node);
}

/**
 * Gets the op of a node.
 * Intern version for libFirm.
 */
static inline ir_op *get_irn_op_(const ir_node *node)
{
	assert(node);
	return node->op;
}

static inline void set_irn_op_(ir_node *node, ir_op *op)
{
	assert(node);
	node->op = op;
}

/** Copies all attributes stored in the old node  to the new node.
    Assumes both have the same opcode and sufficient size. */
static inline void copy_node_attr_(ir_graph *irg, const ir_node *old_node,
                                   ir_node *new_node)
{
	ir_op *op = get_irn_op_(old_node);

	/* must always exist */
	op->ops.copy_attr(irg, old_node, new_node);
}

/**
 * Gets the opcode of a node.
 * Intern version for libFirm.
 */
static inline unsigned get_irn_opcode_(const ir_node *node)
{
	assert(k_ir_node == get_kind(node));
	assert(node->op);
	return node->op->code;
}

/**
 * Returns the number of predecessors without the block predecessor.
 * Intern version for libFirm.
 */
static inline int get_irn_arity_(const ir_node *node)
{
	return (int)(ARR_LEN(node->in) - 1);
}

/**
 * Intern version for libFirm.
 */
static inline ir_node *get_irn_n_(const ir_node *node, int n)
{
	ir_node *nn;

	assert(-1 <= n && n < get_irn_arity_(node));

	nn = node->in[n + 1];
	if (nn->op != op_Id) return nn;

	return (node->in[n + 1] = skip_Id(nn));
}

/**
 * returns a hash value for a node
 */
static inline unsigned hash_irn(const ir_node *node)
{
	return (unsigned) get_irn_idx(node);
}

static inline int get_irn_deps_(const ir_node *node)
{
	return node->deps ? (int)ARR_LEN(node->deps) : 0;
}

static inline ir_node *get_irn_dep_(const ir_node *node, int pos)
{
	assert(node->deps && "dependency array node yet allocated. use add_irn_dep()");
	assert(pos >= 0 && pos < (int)ARR_LEN(node->deps) && "dependency index out of range");
	return node->deps[pos];
}

/* forward declaration outside iredges_t.h to avoid circular include problems */
void edges_notify_edge_kind(ir_node *src, int pos, ir_node *tgt, ir_node *old_tgt, ir_edge_kind_t kind, ir_graph *irg);

static inline void set_irn_dep_(ir_node *node, int pos, ir_node *dep)
{
	ir_node *old;

	assert(node->deps && "dependency array node yet allocated. use add_irn_dep()");
	assert(pos >= 0 && pos < (int)ARR_LEN(node->deps) && "dependency index out of range");
	old = node->deps[pos];
	node->deps[pos] = dep;
	edges_notify_edge_kind(node, pos, dep, old, EDGE_KIND_DEP, get_irn_irg(node));
}


static inline int get_irn_ins_or_deps_(const ir_node *irn)
{
	return get_irn_deps_(irn) + get_irn_arity_(irn);
}

static inline ir_node *get_irn_in_or_dep_(const ir_node *irn, int pos)
{
	int n_in = get_irn_arity(irn);
	return pos < n_in ? get_irn_n(irn, pos) : get_irn_dep(irn, pos - n_in);
}

/**
 * Gets the mode of a node.
 * Intern version for libFirm.
 */
static inline ir_mode *get_irn_mode_(const ir_node *node)
{
	assert(node);
	return node->mode;
}

/**
 * Sets the mode of a node.
 * Intern version of libFirm.
 */
static inline void set_irn_mode_(ir_node *node, ir_mode *mode)
{
	assert(node);
	node->mode = mode;
}

static inline int ir_has_irg_ref(const ir_node *node)
{
	return is_Block(node) || is_Bad(node) || is_Anchor(node);
}

static inline ir_graph *get_irn_irg_(const ir_node *node)
{
	/*
	 * Do not use get_nodes_block() here, because this
	 * will check the pinned state.
	 * However even a 'wrong' block is always in the proper irg.
	 */
	if (! is_Block(node))
		node = get_irn_n(node, -1);
	assert(ir_has_irg_ref(node));
	return node->attr.irg.irg;
}

static inline ir_node *get_nodes_block_(const ir_node *node)
{
	assert(!is_Block(node));
	return get_irn_n(node, -1);
}

/**
 * Gets the visited counter of a node.
 * Intern version for libFirm.
 */
static inline ir_visited_t get_irn_visited_(const ir_node *node)
{
	assert(node);
	return node->visited;
}

/**
 * Sets the visited counter of a node.
 * Intern version for libFirm.
 */
static inline void set_irn_visited_(ir_node *node, ir_visited_t visited)
{
	assert(node);
	node->visited = visited;
}

/**
 * Mark a node as visited in a graph.
 * Intern version for libFirm.
 */
static inline void mark_irn_visited_(ir_node *node)
{
	node->visited = get_irn_irg(node)->visited;
}

/**
 * Returns non-zero if a node of was visited.
 * Intern version for libFirm.
 */
static inline int irn_visited_(const ir_node *node)
{
	ir_graph *irg = get_irn_irg(node);
	return node->visited >= irg->visited;
}

static inline int irn_visited_else_mark_(ir_node *node)
{
	if (irn_visited_(node))
		return 1;
	mark_irn_visited_(node);
	return 0;
}

/**
 * Sets the link of a node.
 * Intern version of libFirm.
 */
static inline void set_irn_link_(ir_node *node, void *link)
{
	assert(node);
	node->link = link;
}

/**
 * Returns the link of a node.
 * Intern version of libFirm.
 */
static inline void *get_irn_link_(const ir_node *node)
{
	assert(node && is_ir_node_(node));
	return node->link;
}

/**
 * Returns whether the node _always_ must be pinned.
 * I.e., the node is not floating after global cse.
 *
 * Intern version of libFirm.
 */
static inline op_pin_state get_irn_pinned_(const ir_node *node)
{
	op_pin_state state;
	assert(node && is_ir_node_(node));
	/* Check opcode */
	state = get_op_pinned_(get_irn_op_(node));

	if (state >= op_pin_state_exc_pinned)
		return (op_pin_state)node->attr.except.pin_state;

	return state;
}

static inline op_pin_state is_irn_pinned_in_irg_(const ir_node *node)
{
	if (get_irg_pinned(get_irn_irg(node)) == op_pin_state_floats)
		return get_irn_pinned(node);
	return op_pin_state_pinned;
}

/* include generated code */
#include "gen_irnode.h"

static inline int is_unop_(const ir_node *node)
{
	assert(node && is_ir_node_(node));
	return (node->op->opar == oparity_unary);
}

static inline int is_binop_(const ir_node *node)
{
	assert(node && is_ir_node_(node));
	return (node->op->opar == oparity_binary);
}

static inline int is_strictConv_(const ir_node *node)
{
	return is_Conv_(node) && get_Conv_strict(node);
}

static inline int is_SymConst_addr_ent_(const ir_node *node)
{
	return is_SymConst(node) && get_SymConst_kind(node) == symconst_addr_ent;
}

static inline int get_Block_n_cfgpreds_(const ir_node *node)
{
	assert(is_Block_(node));
	return get_irn_arity_(node);
}

static inline ir_node *get_Block_cfgpred_(const ir_node *node, int pos)
{
	assert(is_Block_(node));
	return get_irn_n_(node, pos);
}

/* Get the predecessor block.
 *
 *  Returns the block corresponding to the predecessor pos.
 *
 *  There are several ambiguities we resolve with this function:
 *  - The direct predecessor can be a Proj, which is not pinned.
 *    We walk from the predecessor to the next pinned node
 *    (skip_Proj) and return the block that node is in.
 *  - If we encounter the Bad node, this function does not return
 *    the Start block, but the Bad node.
 */
static inline ir_node *get_Block_cfgpred_block_(const ir_node *node, int pos)
{
	ir_node *res = get_Block_cfgpred(node, pos);
	if (is_Bad(res)) {
		/* must return a Bad with mode_BB! */
		ir_graph *irg = get_irn_irg(node);
		return new_r_Bad(irg, mode_BB);
	} else {
		return get_nodes_block(skip_Proj(res));
	}
}

static inline ir_visited_t get_Block_block_visited_(const ir_node *node)
{
	assert(is_Block(node));
	return node->attr.block.block_visited;
}

static inline void set_Block_block_visited_(ir_node *node, ir_visited_t visit)
{
	assert(is_Block(node));
	node->attr.block.block_visited = visit;
}

static inline void mark_Block_block_visited_(ir_node *node)
{
	ir_graph *irg = get_Block_irg(node);
	node->attr.block.block_visited = get_irg_block_visited(irg);
}

static inline int Block_block_visited_(const ir_node *node)
{
	ir_graph *irg = get_Block_irg(node);
	return node->attr.block.block_visited >= get_irg_block_visited(irg);
}

static inline ir_graph *get_Block_irg_(const ir_node *block)
{
	assert(is_Block(block));
	return block->attr.irg.irg;
}

static inline ir_tarval *get_Const_tarval_(const ir_node *node)
{
	assert(get_irn_op_(node) == op_Const);
	return node->attr.con.tarval;
}

static inline int is_Const_null_(const ir_node *node)
{
	return tarval_is_null(get_Const_tarval_(node));
}

static inline int is_Const_one_(const ir_node *node)
{
	return tarval_is_one(get_Const_tarval_(node));
}

static inline int is_Const_all_one_(const ir_node *node)
{
	return tarval_is_all_one(get_Const_tarval_(node));
}

static inline int is_irn_forking_(const ir_node *node)
{
	return is_op_forking(get_irn_op_(node));
}

static inline ir_type *get_irn_type_attr_(ir_node *node)
{
	return get_irn_op_(node)->ops.get_type_attr(node);
}

static inline ir_entity *get_irn_entity_attr_(ir_node *node)
{
	return get_irn_op_(node)->ops.get_entity_attr(node);
}

static inline int is_irn_constlike_(const ir_node *node)
{
	return is_op_constlike(get_irn_op_(node));
}

static inline int is_irn_always_opt_(const ir_node *node)
{
	return is_op_always_opt(get_irn_op_(node));
}

static inline int is_irn_keep_(const ir_node *node)
{
	return is_op_keep(get_irn_op_(node));
}

static inline int is_irn_start_block_placed_(const ir_node *node)
{
	return is_op_start_block_placed(get_irn_op_(node));
}

static inline int is_irn_machine_op_(const ir_node *node)
{
	return is_op_machine(get_irn_op_(node));
}

static inline int is_irn_machine_operand_(const ir_node *node)
{
	return is_op_machine_operand(get_irn_op_(node));
}

static inline int is_irn_machine_user_(const ir_node *node, unsigned n)
{
	return is_op_machine_user(get_irn_op_(node), n);
}

static inline int is_irn_cse_neutral_(const ir_node *node)
{
	return is_op_cse_neutral(get_irn_op_(node));
}

static inline cond_jmp_predicate get_Cond_jmp_pred_(const ir_node *node)
{
	assert(get_irn_op_(node) == op_Cond);
	return node->attr.cond.jmp_pred;
}

static inline void set_Cond_jmp_pred_(ir_node *node, cond_jmp_predicate pred)
{
	assert(get_irn_op_(node) == op_Cond);
	node->attr.cond.jmp_pred = pred;
}

static inline void *get_irn_generic_attr_(ir_node *node)
{
	return &node->attr;
}

static inline const void *get_irn_generic_attr_const_(const ir_node *node)
{
	return &node->attr;
}

static inline unsigned get_irn_idx_(const ir_node *node)
{
	return node->node_idx;
}

static inline dbg_info *get_irn_dbg_info_(const ir_node *n)
{
	return n->dbi;
}

static inline void set_irn_dbg_info_(ir_node *n, dbg_info *db)
{
	n->dbi = db;
}

/**
 * Sets the Phi list of a block.
 */
static inline void set_Block_phis_(ir_node *block, ir_node *phi)
{
	assert(is_Block_(block));
	assert(phi == NULL || is_Phi_(phi));
	block->attr.block.phis = phi;
}

/**
 * Returns the link of a node.
 * Intern version of libFirm.
 */
static inline ir_node *get_Block_phis_(const ir_node *block)
{
	assert(is_Block_(block));
	return block->attr.block.phis;
}

static inline void set_Phi_next_(ir_node *phi, ir_node *next)
{
	assert(is_Phi_(phi));
	phi->attr.phi.next = next;
}

static inline ir_node *get_Phi_next_(const ir_node *phi)
{
	assert(is_Phi_(phi));
	return phi->attr.phi.next;
}

/** Add a Phi node to the list of Block Phi's. */
static inline void add_Block_phi_(ir_node *block, ir_node *phi)
{
	assert(is_Block_(block));
	set_Phi_next_(phi, get_Block_phis_(block));
	set_Block_phis_(block, phi);
}

/** Get the Block mark (single bit). */
static inline unsigned get_Block_mark_(const ir_node *block)
{
	assert(is_Block_(block));
	return block->attr.block.marked;
}

/** Set the Block mark (single bit). */
static inline void set_Block_mark_(ir_node *block, unsigned mark)
{
	assert(is_Block_(block));
	block->attr.block.marked = mark;
}

/** Returns non-zero if a node is a routine parameter. */
static inline int is_arg_Proj_(const ir_node *node)
{
	if (! is_Proj(node))
		return 0;
	node = get_Proj_pred(node);
	if (! is_Proj(node))
		return 0;
	return pn_Start_T_args == get_Proj_proj(node) && is_Start(get_Proj_pred(node));
}

static inline size_t ir_switch_table_get_n_entries_(const ir_switch_table *table)
{
	return table->n_entries;
}

static inline ir_switch_table_entry *ir_switch_table_get_entry(
		ir_switch_table *table, size_t entry)
{
	assert(entry < table->n_entries);
	return &table->entries[entry];
}

static inline const ir_switch_table_entry *ir_switch_table_get_entry_const(
		const ir_switch_table *table, size_t entry)
{
	assert(entry < table->n_entries);
	return &table->entries[entry];
}

/** initialize ir_node module */
void init_irnode(void);

/* this section MUST contain all inline functions */
#define is_ir_node(thing)                     is_ir_node_(thing)
#define get_irn_arity(node)                   get_irn_arity_(node)
#define get_irn_n(node, n)                    get_irn_n_(node, n)
#define get_irn_mode(node)                    get_irn_mode_(node)
#define set_irn_mode(node, mode)              set_irn_mode_(node, mode)
#define get_irn_irg(node)                     get_irn_irg_(node)
#define get_nodes_block(node)                 get_nodes_block_(node)
#define get_irn_op(node)                      get_irn_op_(node)
#define set_irn_op(node, op)                  set_irn_op_(node, op)
#define get_irn_opcode(node)                  get_irn_opcode_(node)
#define get_irn_visited(node)                 get_irn_visited_(node)
#define set_irn_visited(node, v)              set_irn_visited_(node, v)
#define mark_irn_visited(node)                mark_irn_visited_(node)
#define irn_visited(node)                     irn_visited_(node)
#define irn_visited_else_mark(node)           irn_visited_else_mark_(node)
#define set_irn_link(node, link)              set_irn_link_(node, link)
#define get_irn_link(node)                    get_irn_link_(node)
#define get_irn_pinned(node)                  get_irn_pinned_(node)
#define is_irn_pinned_in_irg(node)            is_irn_pinned_in_irg_(node)
#define is_unop(node)                         is_unop_(node)
#define is_binop(node)                        is_binop_(node)
#define is_Proj(node)                         is_Proj_(node)
#define is_Phi(node)                          is_Phi_(node)
#define is_strictConv(node)                   is_strictConv_(node)
#define is_SymConst_addr_ent(node)            is_SymConst_addr_ent_(node)
#define get_Block_n_cfgpreds(node)            get_Block_n_cfgpreds_(node)
#define get_Block_cfgpred(node, pos)          get_Block_cfgpred_(node, pos)
#define get_Block_cfgpred_block(node, pos)    get_Block_cfgpred_block_(node, pos)
#define get_Block_block_visited(node)         get_Block_block_visited_(node)
#define set_Block_block_visited(node, visit)  set_Block_block_visited_(node, visit)
#define mark_Block_block_visited(node)        mark_Block_block_visited_(node)
#define Block_block_visited(node)             Block_block_visited_(node)
#define get_Block_irg(block)                  get_Block_irg_(block)
#define get_Const_tarval(node)                get_Const_tarval_(node)
#define is_Const_null(node)                   is_Const_null_(node)
#define is_Const_one(node)                    is_Const_one_(node)
#define is_Const_all_one(node)                is_Const_all_one_(node)
#define is_irn_forking(node)                  is_irn_forking_(node)
#define copy_node_attr(irg,oldn,newn)         copy_node_attr_(irg,oldn,newn)
#define get_irn_type(node)                    get_irn_type_(node)
#define get_irn_type_attr(node)               get_irn_type_attr_(node)
#define get_irn_entity_attr(node)             get_irn_entity_attr_(node)
#define is_irn_constlike(node)                is_irn_constlike_(node)
#define is_irn_always_opt(node)               is_irn_always_opt_(node)
#define is_irn_keep(node)                     is_irn_keep_(node)
#define is_irn_start_block_placed(node)       is_irn_start_block_placed_(node)
#define is_irn_machine_op(node)               is_irn_machine_op_(node)
#define is_irn_machine_operand(node)          is_irn_machine_operand_(node)
#define is_irn_machine_user(node, n)          is_irn_machine_user_(node, n)
#define is_irn_cse_neutral(node)              is_irn_cse_neutral_(node)
#define get_Cond_jmp_pred(node)               get_Cond_jmp_pred_(node)
#define set_Cond_jmp_pred(node, pred)         set_Cond_jmp_pred_(node, pred)
#define get_irn_generic_attr(node)            get_irn_generic_attr_(node)
#define get_irn_generic_attr_const(node)      get_irn_generic_attr_const_(node)
#define get_irn_idx(node)                     get_irn_idx_(node)

#define get_irn_deps(node)                    get_irn_deps_(node)
#define set_irn_dep(node, pos, dep)           set_irn_dep_(node, pos, dep)
#define get_irn_dep(node, pos)                get_irn_dep_(node, pos)

#define get_irn_ins_or_deps(node)             get_irn_ins_or_deps_(node)
#define get_irn_in_or_dep(node, pos)          get_irn_in_or_dep_(node, pos)

#define get_irn_dbg_info(node)                get_irn_dbg_info_(node)
#define set_irn_dbg_info(node, db)            set_irn_dbg_info_(node, db)

#define set_Block_phis(block, phi)            set_Block_phis_(block, phi)
#define get_Block_phis(block)                 get_Block_phis_(block)
#define add_Block_phi(block, phi)             add_Block_phi_(block, phi)
#define get_Block_mark(block)                 get_Block_mark_(block)
#define set_Block_mark(block, mark)           set_Block_mark_(block, mark)

#define set_Phi_next(node, phi)               set_Phi_next_(node, phi)
#define get_Phi_next(node)                    get_Phi_next_(node)

#define is_arg_Proj(node)                     is_arg_Proj_(node)
#define ir_switch_table_get_n_entries(table)  ir_switch_table_get_n_entries_(table)

#endif
