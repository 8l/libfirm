/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief   Statistics for Firm.
 * @author  Michael Beck
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dags.h"
#include "firmstat_t.h"
#include "hashptr.h"
#include "ircons.h"
#include "irdump.h"
#include "irhooks.h"
#include "irouts.h"
#include "irtools.h"
#include "lc_opts.h"
#include "lc_opts_enum.h"
#include "pattern.h"
#include "stat_dmp.h"
#include "util.h"
#include "xmalloc.h"

/*
 * need this to be static:
 * Special pseudo Opcodes that we need to count some interesting cases
 */

static unsigned stat_options;

/* ---------------------------------------------------------------------------------- */

/** Marks the begin of a statistic (hook) function. */
#define STAT_ENTER    ++status->recursive

/** Marks the end of a statistic (hook) functions. */
#define STAT_LEAVE    --status->recursive

/** Allows to enter a statistic function only when we are not already in a hook. */
#define STAT_ENTER_SINGLE    do { if (status->recursive > 0) return; ++status->recursive; } while (0)

/**
 * global status
 */
static stat_info_t *status = NULL;

/**
 * Compare two elements of the opcode hash.
 */
static int opcode_cmp(const void *elt, const void *key)
{
	const node_entry_t *e1 = (const node_entry_t*)elt;
	const node_entry_t *e2 = (const node_entry_t*)key;

	return e1->op_id != e2->op_id;
}

/**
 * Compare two elements of the graph hash.
 */
static int graph_cmp(const void *elt, const void *key)
{
	const graph_entry_t *e1 = (const graph_entry_t*)elt;
	const graph_entry_t *e2 = (const graph_entry_t*)key;

	return e1->irg != e2->irg;
}

/**
 * Compare two elements of the optimization hash.
 */
static int opt_cmp(const void *elt, const void *key)
{
	const opt_entry_t *e1 = (const opt_entry_t*)elt;
	const opt_entry_t *e2 = (const opt_entry_t*)key;

	return e1->op_id != e2->op_id;
}

/**
 * Compare two elements of the block hash.
 */
static int block_cmp(const void *elt, const void *key)
{
	const block_entry_t *e1 = (const block_entry_t*)elt;
	const block_entry_t *e2 = (const block_entry_t*)key;

	/* it's enough to compare the block number */
	return e1->block_nr != e2->block_nr;
}

/**
 * Compare two elements of the be_block hash.
 */
static int be_block_cmp(const void *elt, const void *key)
{
	const be_block_entry_t *e1 = (const be_block_entry_t*)elt;
	const be_block_entry_t *e2 = (const be_block_entry_t*)key;

	return e1->block_nr != e2->block_nr;
}

/**
 * Compare two elements of reg pressure hash.
 */
static int reg_pressure_cmp(const void *elt, const void *key)
{
	const reg_pressure_entry_t *e1 = (const reg_pressure_entry_t*)elt;
	const reg_pressure_entry_t *e2 = (const reg_pressure_entry_t*)key;

	return e1->class_name != e2->class_name;
}

/**
 * Compare two elements of the ir_op hash.
 */
static int opcode_cmp_2(const void *elt, const void *key)
{
	const ir_op *e1 = (const ir_op*)elt;
	const ir_op *e2 = (const ir_op*)key;

	return e1->code != e2->code;
}

/**
 * Compare two elements of the address_mark set.
 */
static int address_mark_cmp(const void *elt, const void *key, size_t size)
{
	(void)size;
	const address_mark_entry_t *e1 = (const address_mark_entry_t*)elt;
	const address_mark_entry_t *e2 = (const address_mark_entry_t*)key;

	/* compare only the nodes, the rest is used as data container */
	return e1->node != e2->node;
}

/**
 * Clear all counter in a node_entry_t.
 */
static void opcode_clear_entry(node_entry_t *elem)
{
	cnt_clr(&elem->cnt_alive);
	cnt_clr(&elem->new_node);
	cnt_clr(&elem->into_Id);
	cnt_clr(&elem->normalized);
}

/**
 * Returns the associates node_entry_t for an ir_op (and allocates
 * one if not yet available).
 *
 * @param op_id  the IR operation
 * @param hmap   a hash map containing ir_op* -> node_entry_t*
 */
static node_entry_t *opcode_get_entry(op_id_t const op_id, hmap_node_entry_t *const hmap)
{
	node_entry_t key;
	key.op_id = op_id;

	node_entry_t *elem = (node_entry_t*)pset_find(hmap, &key, hash_ptr(op_id));
	if (elem)
		return elem;

	elem = OALLOCZ(&status->cnts, node_entry_t);

	/* clear counter */
	opcode_clear_entry(elem);

	elem->op_id = op_id;

	return (node_entry_t*)pset_insert(hmap, elem, hash_ptr(op_id));
}

/**
 * Returns the associates ir_op for an opcode
 *
 * @param code  the IR opcode
 * @param hmap  the hash map containing opcode -> ir_op*
 */
static ir_op *opcode_find_entry(ir_opcode code, hmap_ir_op *hmap)
{
	ir_op key;
	key.code = code;
	return (ir_op*)pset_find(hmap, &key, code);
}

/**
 * Clears all counter in a graph_entry_t.
 *
 * @param elem  the graph entry
 * @param all   if non-zero, clears all counters, else leave accumulated ones
 */
static void graph_clear_entry(graph_entry_t *elem, int all)
{
	/* clear accumulated / non-accumulated counter */
	for (int i = all ? 0 : _gcnt_non_acc; i < _gcnt_last; ++i) {
		cnt_clr(&elem->cnt[i]);
	}

	if (elem->block_hash) {
		del_pset(elem->block_hash);
		elem->block_hash = NULL;
	}

	obstack_free(&elem->recalc_cnts, NULL);
	obstack_init(&elem->recalc_cnts);
}

/**
 * Returns the associated graph_entry_t for an IR graph.
 *
 * @param irg   the IR graph, NULL for the global counter
 * @param hmap  the hash map containing ir_graph* -> graph_entry_t*
 */
static graph_entry_t *graph_get_entry(ir_graph *irg, hmap_graph_entry_t *hmap)
{
	graph_entry_t key;
	key.irg = irg;

	graph_entry_t *elem = (graph_entry_t*)pset_find(hmap, &key, hash_ptr(irg));
	if (elem) {
		/* create hash map backend block information */
		if (!elem->be_block_hash)
			elem->be_block_hash = new_pset(be_block_cmp, 5);

		return elem;
	}

	/* allocate a new one */
	elem = OALLOCZ(&status->cnts, graph_entry_t);
	obstack_init(&elem->recalc_cnts);

	/* clear counter */
	graph_clear_entry(elem, 1);

	/* new hash table for opcodes here  */
	elem->opcode_hash   = new_pset(opcode_cmp, 5);
	elem->address_mark  = new_set(address_mark_cmp, 5);
	elem->irg           = irg;

	/* these hash tables are created on demand */
	elem->block_hash = NULL;

	for (size_t i = 0; i != ARRAY_SIZE(elem->opt_hash); ++i)
		elem->opt_hash[i] = new_pset(opt_cmp, 4);

	return (graph_entry_t*)pset_insert(hmap, elem, hash_ptr(irg));
}

static graph_entry_t *get_graph_entry(ir_node *const node, hmap_graph_entry_t *const hmap)
{
	ir_graph *const irg = get_irn_irg(node);
	return graph_get_entry(irg, hmap);
}

static node_entry_t *get_node_entry(ir_node *const node, op_id_t const op_id)
{
	ir_graph      *const irg   = node ? get_irn_irg(node) : NULL;
	graph_entry_t *const graph = graph_get_entry(irg, status->irg_hash);
	return opcode_get_entry(op_id, graph->opcode_hash);
}

/**
 * Clear all counter in an opt_entry_t.
 */
static void opt_clear_entry(opt_entry_t *elem)
{
	cnt_clr(&elem->count);
}

/**
 * Returns the associated opt_entry_t for an IR operation.
 *
 * @param op_id  the IR operation
 * @param hmap   the hash map containing ir_op* -> opt_entry_t*
 */
static opt_entry_t *opt_get_entry(op_id_t const op_id, hmap_opt_entry_t *const hmap)
{
	opt_entry_t key;
	key.op_id = op_id;

	opt_entry_t *elem = (opt_entry_t*)pset_find(hmap, &key, hash_ptr(op_id));
	if (elem)
		return elem;

	elem = OALLOCZ(&status->cnts, opt_entry_t);

	/* clear new counter */
	opt_clear_entry(elem);

	elem->op_id = op_id;

	return (opt_entry_t*)pset_insert(hmap, elem, hash_ptr(op_id));
}

/**
 * clears all counter in a block_entry_t
 */
static void block_clear_entry(block_entry_t *elem)
{
	for (int i = 0; i < _bcnt_last; ++i)
		cnt_clr(&elem->cnt[i]);
}

/**
 * Returns the associated block_entry_t for an block.
 *
 * @param block_nr  an IR  block number
 * @param hmap      a hash map containing long -> block_entry_t
 */
static block_entry_t *block_get_entry(struct obstack *obst, long block_nr, hmap_block_entry_t *hmap)
{
	block_entry_t key;
	key.block_nr = block_nr;

	block_entry_t *elem = (block_entry_t*)pset_find(hmap, &key, block_nr);
	if (elem)
		return elem;

	elem = OALLOCZ(obst, block_entry_t);

	/* clear new counter */
	block_clear_entry(elem);

	elem->block_nr = block_nr;

	return (block_entry_t*)pset_insert(hmap, elem, block_nr);
}

/**
 * Clear all sets in be_block_entry_t.
 */
static void be_block_clear_entry(be_block_entry_t *elem)
{
	if (elem->reg_pressure)
		del_pset(elem->reg_pressure);

	elem->reg_pressure = new_pset(reg_pressure_cmp, 5);
}

/**
 * Returns the associated be_block_entry_t for an block.
 *
 * @param block  a block
 * @param hmap   a hash map containing long -> be_block_entry_t
 */
static be_block_entry_t *be_block_get_entry(ir_node *const block, hmap_be_block_entry_t *const hmap)
{
	long const block_nr = get_irn_node_nr(block);
	be_block_entry_t key;
	key.block_nr = block_nr;

	be_block_entry_t *elem = (be_block_entry_t*)pset_find(hmap, &key, block_nr);
	if (elem)
		return elem;

	elem = OALLOCZ(&status->be_data, be_block_entry_t);

	/* clear new counter */
	be_block_clear_entry(elem);

	elem->block_nr = block_nr;

	return (be_block_entry_t*)pset_insert(hmap, elem, block_nr);
}

/**
 * Clear optimizations counter,
 */
static void clear_optimization_counter(void)
{
	for (int i = 0; i < FS_OPT_MAX; ++i)
		cnt_clr(&status->num_opts[i]);
}

/**
 * Returns the ir_op for an IR-node,
 * handles special cases and return pseudo op codes.
 *
 * @param none  an IR node
 */
static op_id_t stat_get_irn_op(ir_node *node)
{
	ir_op *const op    = get_irn_op(node);
	op_id_t      op_id = get_op_name(op);
	switch (op->code) {
	case iro_Phi:
		if (get_irn_arity(node) == 0) {
			/* special case, a Phi0 node, count on extra counter */
			op_id = status->op_Phi0 ? status->op_Phi0 : op_id;
		} else if (get_irn_mode(node) == mode_M) {
			/* special case, a Memory Phi node, count on extra counter */
			op_id = status->op_PhiM ? status->op_PhiM : op_id;
		}
		break;
	case iro_Proj:
		if (get_irn_mode(node) == mode_M) {
			/* special case, a Memory Proj node, count on extra counter */
			op_id = status->op_ProjM ? status->op_ProjM : op_id;
		}
		break;
	case iro_Mul:
		if (is_Const(get_Mul_left(node)) || is_Const(get_Mul_right(node))) {
			/* special case, a Multiply by a const, count on extra counter */
			op_id = status->op_MulC ? status->op_MulC : op_id;
		}
		break;
	case iro_Div:
		if (is_Const(get_Div_right(node))) {
			/* special case, a division by a const, count on extra counter */
			op_id = status->op_DivC ? status->op_DivC : op_id;
		}
		break;
	case iro_Mod:
		if (is_Const(get_Mod_right(node))) {
			/* special case, a module by a const, count on extra counter */
			op_id = status->op_ModC ? status->op_ModC : op_id;
		}
		break;
	case iro_Sel:
		if (is_Sel(get_Sel_ptr(node))) {
			/* special case, a Sel of a Sel, count on extra counter */
			op_id = status->op_SelSel ? status->op_SelSel : op_id;
			if (is_Sel(get_Sel_ptr(get_Sel_ptr(node)))) {
				/* special case, a Sel of a Sel of a Sel, count on extra counter */
				op_id = status->op_SelSelSel ? status->op_SelSelSel : op_id;
			}
		}
		break;
	default:
		break;
	}

	return op_id;
}

/**
 * update the block counter
 */
static void undate_block_info(ir_node *node, graph_entry_t *graph)
{
	/* check for block */
	if (is_Block(node)) {
		block_entry_t *const b_entry = block_get_entry(&graph->recalc_cnts, get_irn_node_nr(node), graph->block_hash);
		/* mark start end block to allow to filter them out */
		if (node == get_irg_start_block(graph->irg))
			b_entry->is_start = 1;
		else if (node == get_irg_end_block(graph->irg))
			b_entry->is_end = 1;

		/* count all incoming edges */
		foreach_irn_in(node, i, pred) {
			ir_node *other_block = get_nodes_block(pred);
			block_entry_t *b_entry_other = block_get_entry(&graph->recalc_cnts, get_irn_node_nr(other_block), graph->block_hash);

			cnt_inc(&b_entry->cnt[bcnt_in_edges]);  /* an edge coming from another block */
			cnt_inc(&b_entry_other->cnt[bcnt_out_edges]);
		}
		return;
	}

	ir_node       *const block   = get_nodes_block(node);
	block_entry_t *const b_entry = block_get_entry(&graph->recalc_cnts, get_irn_node_nr(block), graph->block_hash);

	if (is_Phi(node) && get_irn_mode(node) != mode_M) {
		/* count data Phi per block */
		cnt_inc(&b_entry->cnt[bcnt_phi_data]);
	}

	/* we have a new node in our block */
	cnt_inc(&b_entry->cnt[bcnt_nodes]);

	/* don't count keep-alive edges */
	if (is_End(node))
		return;

	foreach_irn_in(node, i, pred) {
		ir_node *const other_block = get_nodes_block(pred);
		if (other_block == block) {
			cnt_inc(&b_entry->cnt[bcnt_edges]); /* a in block edge */
		} else {
			block_entry_t *b_entry_other = block_get_entry(&graph->recalc_cnts, get_irn_node_nr(other_block), graph->block_hash);

			cnt_inc(&b_entry->cnt[bcnt_in_edges]);  /* an edge coming from another block */
			cnt_inc(&b_entry_other->cnt[bcnt_out_edges]);
		}
	}
}

/**
 * Find the base address of a Sel node.
 *
 * @param sel  the node
 *
 * @return the base address.
 */
static ir_node *find_base_adr(ir_node *sel)
{
	ir_node *ptr = sel;
	do {
		ptr = get_Sel_ptr(ptr);
	} while (is_Sel(ptr));
	return ptr;
}

/**
 * Calculates how many arguments of the call are const, updates
 * param distribution.
 */
static void analyse_params_of_Call(graph_entry_t *graph, ir_node *call)
{
	int num_const_args = 0;
	int num_local_adr  = 0;
	int n              = get_Call_n_params(call);
	for (int i = 0; i < n; ++i) {
		ir_node *param = get_Call_param(call, i);
		if (is_irn_constlike(param)) {
			++num_const_args;
		} else if (is_Sel(param)) {
			ir_node *const base = find_base_adr(param);
			if (base == get_irg_frame(graph->irg))
				++num_local_adr;
		}
	}

	if (num_const_args > 0)
		cnt_inc(&graph->cnt[gcnt_call_with_cnst_arg]);
	if (num_const_args == n)
		cnt_inc(&graph->cnt[gcnt_call_with_all_cnst_arg]);
	if (num_local_adr > 0)
		cnt_inc(&graph->cnt[gcnt_call_with_local_adr]);

	stat_inc_int_distrib_tbl(status->dist_param_cnt, n);
}

/**
 * Update info on calls.
 *
 * @param call   The call
 * @param graph  The graph entry containing the call
 */
static void stat_update_call(ir_node *call, graph_entry_t *graph)
{
	/* If the block is bad, the whole subgraph will collapse later
	 * so do not count this call.
	 * This happens in dead code. */
	ir_node *block = get_nodes_block(call);
	if (is_Bad(block))
		return;

	cnt_inc(&graph->cnt[gcnt_all_calls]);

	/* found a call, this function is not a leaf */
	graph->is_leaf = 0;

	ir_graph       *callee = NULL;
	ir_node  *const ptr    = get_Call_ptr(call);
	if (is_Address(ptr)) {
		/* ok, we seem to know the entity */
		ir_entity *const ent = get_Address_entity(ptr);
		callee = get_entity_irg(ent);

		/* it is recursive, if it calls at least once */
		if (callee == graph->irg)
			graph->is_recursive = 1;
		if (callee == NULL)
			cnt_inc(&graph->cnt[gcnt_external_calls]);
	} else {
		/* indirect call, could not predict */
		cnt_inc(&graph->cnt[gcnt_indirect_calls]);

		/* NOT a leaf call */
		graph->is_leaf_call = LCS_NON_LEAF_CALL;
	}

	/* check, if it's a chain-call: Then, the call-block
	 * must dominate the end block. */
	{
		int const depth = get_Block_dom_depth(block);
		ir_node  *curr  = get_irg_end_block(graph->irg);
		for (; curr != block && get_Block_dom_depth(curr) > depth;) {
			curr = get_Block_idom(curr);
			if (!curr || !is_Block(curr))
				break;
		}

		if (curr != block)
			graph->is_chain_call = 0;
	}

	/* check, if the callee is a leaf */
	if (callee) {
		graph_entry_t *called = graph_get_entry(callee, status->irg_hash);
		if (called->is_analyzed) {
			if (!called->is_leaf)
				graph->is_leaf_call = LCS_NON_LEAF_CALL;
		}
	}

	analyse_params_of_Call(graph, call);
}

/**
 * Update info on calls for graphs on the wait queue.
 */
static void stat_update_call_2(ir_node *call, graph_entry_t *graph)
{
	/* If the block is bad, the whole subgraph will collapse later
	 * so do not count this call.
	 * This happens in dead code. */
	ir_node *const block = get_nodes_block(call);
	if (is_Bad(block))
		return;

	ir_graph       *callee = NULL;
	ir_node  *const ptr    = get_Call_ptr(call);
	if (is_Address(ptr)) {
		/* ok, we seem to know the entity */
		ir_entity *const ent = get_Address_entity(ptr);
		callee = get_entity_irg(ent);
	}

	/* check, if the callee is a leaf */
	if (callee) {
		graph_entry_t *called = graph_get_entry(callee, status->irg_hash);

		assert(called->is_analyzed);

		if (!called->is_leaf)
			graph->is_leaf_call = LCS_NON_LEAF_CALL;
	} else
		graph->is_leaf_call = LCS_NON_LEAF_CALL;
}

/**
 * Update info on Load/Store address statistics.
 */
static void stat_update_address(ir_node *node, graph_entry_t *graph)
{
	unsigned const opc = get_irn_opcode(node);
	switch (opc) {
	case iro_Address:
		/* a global address */
		cnt_inc(&graph->cnt[gcnt_global_adr]);
		break;

	case iro_Sel: {
		ir_node  *const base = find_base_adr(node);
		ir_graph *const irg  = graph->irg;
		if (base == get_irg_frame(irg)) {
			/* a local Variable. */
			cnt_inc(&graph->cnt[gcnt_local_adr]);
		} else {
			/* Pointer access */
			if (is_Proj(base) && skip_Proj(get_Proj_pred(base)) == get_irg_start(irg)) {
				/* pointer access through parameter, check for THIS */
				ir_entity *const ent = get_irg_entity(irg);
				if (ent) {
					ir_type *const ent_tp = get_entity_type(ent);
					if (get_method_calling_convention(ent_tp) & cc_this_call) {
						if (get_Proj_num(base) == 0) {
							/* THIS pointer */
							cnt_inc(&graph->cnt[gcnt_this_adr]);
							goto end_parameter;
						}
					}
				}
				/* other parameter */
				cnt_inc(&graph->cnt[gcnt_param_adr]);
end_parameter: ;
			} else {
				/* unknown Pointer access */
				cnt_inc(&graph->cnt[gcnt_other_adr]);
			}
		}
	}

	default:
		break;
	}
}

/**
 * Walker for reachable nodes count.
 */
static void update_node_stat(ir_node *node, void *env)
{
	graph_entry_t *const graph = (graph_entry_t*)env;
	op_id_t        const op    = stat_get_irn_op(node);
	node_entry_t  *const entry = opcode_get_entry(op, graph->opcode_hash);

	cnt_inc(&entry->cnt_alive);
	int const arity = get_irn_arity(node);
	cnt_add_i(&graph->cnt[gcnt_edges], arity);

	/* count block edges */
	undate_block_info(node, graph);

	/* handle statistics for special node types */

	switch (get_irn_opcode(node)) {
	case iro_Call:
		/* check for properties that depends on calls like recursion/leaf/indirect call */
		stat_update_call(node, graph);
		break;
	case iro_Load:
		/* check address properties */
		stat_update_address(get_Load_ptr(node), graph);
		break;
	case iro_Store:
		/* check address properties */
		stat_update_address(get_Store_ptr(node), graph);
		break;
	case iro_Phi:
		/* check for non-strict Phi nodes */
		foreach_irn_in_r(node, i, pred) {
			if (is_Unknown(pred)) {
				/* found an Unknown predecessor, graph is not strict */
				graph->is_strict = 0;
				break;
			}
		}
	default:
		break;
	}

	/* we want to count the constant IN nodes, not the CSE'ed constant's itself */
	if (status->stat_options & FIRMSTAT_COUNT_CONSTS) {
		foreach_irn_in_r(node, i, pred) {
			if (is_Const(pred)) {
				/* check properties of constants */
				stat_update_const(status, pred, graph);
			}
		}
	}
}

/**
 * Walker for reachable nodes count for graphs on the wait_q.
 */
static void update_node_stat_2(ir_node *node, void *env)
{
	graph_entry_t *const graph = (graph_entry_t*)env;

	/* check for properties that depends on calls like recursion/leaf/indirect call */
	if (is_Call(node))
		stat_update_call_2(node, graph);
}

/**
 * Get the current address mark.
 */
static unsigned get_adr_mark(graph_entry_t *const graph, ir_node const *const node)
{
	address_mark_entry_t  const val   = { node, 0 };
	address_mark_entry_t *const value = set_find(address_mark_entry_t, graph->address_mark, &val, sizeof(val), hash_ptr(node));
	return value ? value->mark : 0;
}

/**
 * Set the current address mark.
 */
static void set_adr_mark(graph_entry_t *graph, ir_node *node, unsigned val)
{
	address_mark_entry_t const value = { node, val };
	(void)set_insert(address_mark_entry_t, graph->address_mark, &value, sizeof(value), hash_ptr(node));
}

#undef DUMP_ADR_MODE

#ifdef DUMP_ADR_MODE
/**
 * a vcg attribute hook: Color a node with a different color if
 * it's identified as a part of an address expression or at least referenced
 * by an address expression.
 */
static int stat_adr_mark_hook(FILE *const F, ir_node const *const node, ir_node const *const local)
{
	ir_node const *const n     = local ? local : node;
	graph_entry_t *const graph = get_graph_entry(n, status->irg_hash);
	unsigned       const mark  = get_adr_mark(graph, n);

	if (mark & MARK_ADDRESS_CALC)
		fprintf(F, "color: purple");
	else if ((mark & (MARK_REF_ADR | MARK_REF_NON_ADR)) == MARK_REF_ADR)
		fprintf(F, "color: pink");
	else if ((mark & (MARK_REF_ADR | MARK_REF_NON_ADR)) == (MARK_REF_ADR|MARK_REF_NON_ADR))
		fprintf(F, "color: lightblue");
	else
		return 0;

	/* I know the color! */
	return 1;
}
#endif

/**
 * Return the "operational" mode of a Firm node.
 */
static ir_mode *get_irn_op_mode(ir_node *node)
{
	switch (get_irn_opcode(node)) {
	case iro_Load:  return get_Load_mode(node);
	case iro_Store: return get_irn_mode(get_Store_value(node));
	case iro_Div:   return get_irn_mode(get_Div_left(node));
	case iro_Mod:   return get_irn_mode(get_Mod_left(node));
	case iro_Cmp:   /* Cmp is no address calculation, or is it? */
	default:        return get_irn_mode(node);
	}
}

/**
 * Post-walker that marks every node that is an address calculation.
 *
 * Users of a node must be visited first. We ensure this by
 * calling it in the post of an outs walk. This should work even in cycles,
 * while the normal pre-walk will not.
 */
static void mark_address_calc(ir_node *node, void *env)
{
	graph_entry_t *const graph = (graph_entry_t*)env;

	ir_mode *const mode = get_irn_op_mode(node);
	if (!mode_is_data(mode))
		return;

	unsigned mark_preds = MARK_REF_NON_ADR;
	if (mode_is_reference(mode)) {
		/* a reference is calculated here, we are sure */
		set_adr_mark(graph, node, MARK_ADDRESS_CALC);

		mark_preds = MARK_REF_ADR;
	} else {
		unsigned mark = get_adr_mark(graph, node);

		if ((mark & (MARK_REF_ADR | MARK_REF_NON_ADR)) == MARK_REF_ADR) {
			/* this node has no reference mode, but is only
			 * referenced by address calculations */
			mark_preds = MARK_REF_ADR;
		}
	}

	/* mark all predecessors */
	foreach_irn_in(node, i, pred) {
		if (mode_is_data(get_irn_op_mode(pred)))
			set_adr_mark(graph, pred, get_adr_mark(graph, pred) | mark_preds);
	}
}

/**
 * Post-walker that marks every node that is an address calculation.
 *
 * Users of a node must be visited first. We ensure this by
 * calling it in the post of an outs walk. This should work even in cycles,
 * while the normal pre-walk will not.
 */
static void count_adr_ops(ir_node *node, void *env)
{
	graph_entry_t *const graph = (graph_entry_t*)env;

	unsigned const mark = get_adr_mark(graph, node);
	if (mark & MARK_ADDRESS_CALC)
		cnt_inc(&graph->cnt[gcnt_pure_adr_ops]);
	else if ((mark & (MARK_REF_ADR | MARK_REF_NON_ADR)) == MARK_REF_ADR)
		cnt_inc(&graph->cnt[gcnt_pure_adr_ops]);
	else if ((mark & (MARK_REF_ADR | MARK_REF_NON_ADR)) == (MARK_REF_ADR|MARK_REF_NON_ADR))
		cnt_inc(&graph->cnt[gcnt_all_adr_ops]);
}

/**
 * Called for every graph when the graph is either deleted or stat_dump_snapshot()
 * is called, must recalculate all statistic info.
 *
 * @param global    The global entry
 * @param graph     The current entry
 */
static void update_graph_stat(graph_entry_t *global, graph_entry_t *graph)
{
	/* clear first the alive counter in the graph */
	foreach_pset(graph->opcode_hash, node_entry_t, entry) {
		cnt_clr(&entry->cnt_alive);
	}

	/* set pessimistic values */
	graph->is_leaf       = 1;
	graph->is_leaf_call  = LCS_UNKNOWN;
	graph->is_recursive  = 0;
	graph->is_chain_call = 1;
	graph->is_strict     = 1;

	/* create new block counter */
	graph->block_hash = new_pset(block_cmp, 5);

	/* we need dominator info */
	if (graph->irg != get_const_code_irg()) {
		assure_irg_properties(graph->irg, IR_GRAPH_PROPERTY_CONSISTENT_DOMINANCE);
	}

	/* count the nodes in the graph */
	irg_walk_graph(graph->irg, update_node_stat, NULL, graph);

	/* recursive functions are never chain calls, leafs don't have calls */
	if (graph->is_recursive || graph->is_leaf)
		graph->is_chain_call = 0;

	/* assume we walk every graph only ONCE, we could sum here the global count */
	foreach_pset(graph->opcode_hash, node_entry_t, entry) {
		node_entry_t *g_entry = opcode_get_entry(entry->op_id, global->opcode_hash);

		/* update the node counter */
		cnt_add(&g_entry->cnt_alive, &entry->cnt_alive);
	}

	/* count the number of address calculation */
	if (graph->irg != get_const_code_irg()) {
		assure_irg_outs(graph->irg);

		/* Must be done an the outs graph */
		irg_out_walk(get_irg_start(graph->irg), NULL, mark_address_calc, graph);

#ifdef DUMP_ADR_MODE
		/* register the vcg hook and dump the graph for test */
		set_dump_node_vcgattr_hook(stat_adr_mark_hook);
		dump_ir_graph(graph->irg, "-adr");
		set_dump_node_vcgattr_hook(NULL);
#endif

		irg_walk_graph(graph->irg, NULL, count_adr_ops, graph);
	}

	/* count the DAG's */
	if (status->stat_options & FIRMSTAT_COUNT_DAG)
		count_dags_in_graph(global, graph);

	/* calculate the patterns of this graph */
	stat_calc_pattern_history(graph->irg);

	/* leaf function did not call others */
	if (graph->is_leaf) {
		graph->is_leaf_call = LCS_NON_LEAF_CALL;
	} else if (graph->is_leaf_call == LCS_UNKNOWN) {
		/* we still don't know if this graph calls leaf-functions, so enqueue */
		pdeq_putl(status->wait_q, graph);
	}

	/* we have analyzed this graph */
	graph->is_analyzed = 1;

	/* accumulate all counter's */
	for (int i = 0; i < _gcnt_last; ++i)
		cnt_add(&global->cnt[i], &graph->cnt[i]);
}

/**
 * Called for every graph that was on the wait_q in stat_dump_snapshot()
 * must finish all statistic info calculations.
 *
 * @param graph  The current entry
 */
static void update_graph_stat_2(graph_entry_t *graph)
{
	if (graph->is_deleted) {
		/* deleted, ignore */
		return;
	}

	if (graph->irg) {
		/* count the nodes in the graph */
		irg_walk_graph(graph->irg, update_node_stat_2, NULL, graph);

		if (graph->is_leaf_call == LCS_UNKNOWN)
			graph->is_leaf_call = LCS_LEAF_CALL;
	}
}

/**
 * Register a dumper.
 */
static void stat_register_dumper(const dumper_t *dumper)
{
	dumper_t *p = XMALLOC(dumper_t);
	*p = *dumper;

	p->next        = status->dumper;
	p->status      = status;
	status->dumper = p;

	/* FIXME: memory leak */
}

/**
 * Dumps the statistics of an IR graph.
 */
static void stat_dump_graph(graph_entry_t *entry)
{
	for (dumper_t *dumper = status->dumper; dumper; dumper = dumper->next) {
		if (dumper->dump_graph)
			dumper->dump_graph(dumper, entry);
	}
}

/**
 * Dumps a constant table.
 */
static void stat_dump_consts(const constant_info_t *tbl)
{
	for (dumper_t *dumper = status->dumper; dumper; dumper = dumper->next) {
		if (dumper->dump_const_tbl)
			dumper->dump_const_tbl(dumper, tbl);
	}
}

/**
 * Dumps the parameter distribution
 */
static void stat_dump_param_tbl(const distrib_tbl_t *tbl, graph_entry_t *global)
{
	for (dumper_t *dumper = status->dumper; dumper; dumper = dumper->next) {
		if (dumper->dump_param_tbl)
			dumper->dump_param_tbl(dumper, tbl, global);
	}
}

/**
 * Dumps the optimization counter
 */
static void stat_dump_opt_cnt(const counter_t *tbl, unsigned len)
{
	for (dumper_t *dumper = status->dumper; dumper; dumper = dumper->next) {
		if (dumper->dump_opt_cnt)
			dumper->dump_opt_cnt(dumper, tbl, len);
	}
}

/**
 * Initialize the dumper.
 */
static void stat_dump_init(const char *name)
{
	for (dumper_t *dumper = status->dumper; dumper; dumper = dumper->next) {
		if (dumper->init)
			dumper->init(dumper, name);
	}
}

/**
 * Finish the dumper.
 */
static void stat_dump_finish(void)
{
	for (dumper_t *dumper = status->dumper; dumper; dumper = dumper->next) {
		if (dumper->finish)
			dumper->finish(dumper);
	}
}

/* ---------------------------------------------------------------------- */

/*
 * Helper: get an ir_op from an opcode.
 */
ir_op *stat_get_op_from_opcode(unsigned code)
{
	return opcode_find_entry((ir_opcode)code, status->ir_op_hash);
}

/**
 * Hook: A new IR op is registered.
 *
 * @param ctx  the hook context
 * @param op   the new IR opcode that was created.
 */
static void stat_new_ir_op(void *ctx, ir_op *op)
{
	(void)ctx;
	if (!status->stat_options)
		return;

	STAT_ENTER;
	{
		graph_entry_t *graph = graph_get_entry(NULL, status->irg_hash);

		/* execute for side effect :-) */
		opcode_get_entry(get_op_name(op), graph->opcode_hash);

		pset_insert(status->ir_op_hash, op, op->code);
	}
	STAT_LEAVE;
}

/**
 * Hook: An IR op is freed.
 *
 * @param ctx  the hook context
 * @param op   the IR opcode that is freed
 */
static void stat_free_ir_op(void *ctx, ir_op *op)
{
	(void)ctx;
	(void)op;
	if (!status->stat_options)
		return;

	STAT_ENTER;
	{
	}
	STAT_LEAVE;
}

/**
 * Hook: A new node is created.
 *
 * @param ctx   the hook context
 * @param node  the new IR node that was created
 */
static void stat_new_node(void *ctx, ir_node *node)
{
	(void)ctx;
	if (!status->stat_options)
		return;

	/* do NOT count during dead node elimination */
	if (status->in_dead_node_elim)
		return;

	STAT_ENTER;
	{
		op_id_t const op_id = stat_get_irn_op(node);
		/* increase global value */
		cnt_inc(&get_node_entry(NULL, op_id)->new_node);
		/* increase local value */
		cnt_inc(&get_node_entry(node, op_id)->new_node);
	}
	STAT_LEAVE;
}

/**
 * Hook: A node is changed into a Id node
 *
 * @param ctx   the hook context
 * @param node  the IR node that will be turned into an ID
 */
static void stat_turn_into_id(void *ctx, ir_node *node)
{
	(void)ctx;
	if (!status->stat_options)
		return;

	STAT_ENTER;
	{
		op_id_t const op_id = stat_get_irn_op(node);
		/* increase global value */
		cnt_inc(&get_node_entry(NULL, op_id)->into_Id);
		/* increase local value */
		cnt_inc(&get_node_entry(node, op_id)->into_Id);
	}
	STAT_LEAVE;
}

/**
 * Hook: A node is normalized
 *
 * @param ctx   the hook context
 * @param node  the IR node that was normalized
 */
static void stat_normalize(void *ctx, ir_node *node)
{
	(void)ctx;
	if (!status->stat_options)
		return;

	STAT_ENTER;
	{
		op_id_t const op_id = stat_get_irn_op(node);
		/* increase global value */
		cnt_inc(&get_node_entry(NULL, op_id)->normalized);
		/* increase local value */
		cnt_inc(&get_node_entry(node, op_id)->normalized);
	}
	STAT_LEAVE;
}

/**
 * Hook: A new graph was created
 *
 * @param ctx  the hook context
 * @param irg  the new IR graph that was created
 * @param ent  the entity of this graph
 */
static void stat_new_graph(void *ctx, ir_graph *irg, ir_entity *ent)
{
	(void)ctx;
	if (!status->stat_options)
		return;

	STAT_ENTER;
	{
		/* execute for side effect :-) */
		graph_entry_t *const graph = graph_get_entry(irg, status->irg_hash);
		graph->ent           = ent;
		graph->is_deleted    = 0;
		graph->is_leaf       = 0;
		graph->is_leaf_call  = 0;
		graph->is_recursive  = 0;
		graph->is_chain_call = 0;
		graph->is_strict     = 1;
		graph->is_analyzed   = 0;
	}
	STAT_LEAVE;
}

/**
 * Hook: A graph will be deleted
 *
 * @param ctx  the hook context
 * @param irg  the IR graph that will be deleted
 *
 * Note that we still hold the information for this graph
 * in our hash maps, only a flag is set which prevents this
 * information from being changed, it's "frozen" from now.
 */
static void stat_free_graph(void *ctx, ir_graph *irg)
{
	(void)ctx;
	if (!status->stat_options)
		return;

	STAT_ENTER;
	{
		graph_entry_t *graph  = graph_get_entry(irg, status->irg_hash);
		graph_entry_t *global = graph_get_entry(NULL, status->irg_hash);

		graph->is_deleted = 1;

		if (status->stat_options & FIRMSTAT_COUNT_DELETED) {
			/* count the nodes of the graph yet, it will be destroyed later */
			update_graph_stat(global, graph);
		}
	}
	STAT_LEAVE;
}

/**
 * Hook: A walk over a graph is initiated. Do not count walks from statistic code.
 *
 * @param ctx  the hook context
 * @param irg  the IR graph that will be walked
 * @param pre  the pre walker
 * @param post the post walker
 */
static void stat_irg_walk(void *ctx, ir_graph *irg, generic_func *pre, generic_func *post)
{
	(void)ctx;
	(void)pre;
	(void)post;
	if (!status->stat_options)
		return;

	STAT_ENTER_SINGLE;
	{
		graph_entry_t *graph = graph_get_entry(irg, status->irg_hash);
		cnt_inc(&graph->cnt[gcnt_acc_walked]);
	}
	STAT_LEAVE;
}

/**
 * Hook: A walk over a graph in block-wise order is initiated. Do not count walks from statistic code.
 *
 * @param ctx  the hook context
 * @param irg  the IR graph that will be walked
 * @param pre  the pre walker
 * @param post the post walker
 */
static void stat_irg_walk_blkwise(void *ctx, ir_graph *irg, generic_func *pre, generic_func *post)
{
	/* for now, do NOT differentiate between blockwise and normal */
	stat_irg_walk(ctx, irg, pre, post);
}

/**
 * Hook: A walk over the graph's blocks is initiated. Do not count walks from statistic code.
 *
 * @param ctx  the hook context
 * @param irg  the IR graph that will be walked
 * @param node the IR node
 * @param pre  the pre walker
 * @param post the post walker
 */
static void stat_irg_block_walk(void *ctx, ir_graph *irg, ir_node *node, generic_func *pre, generic_func *post)
{
	(void)ctx;
	(void)node;
	(void)pre;
	(void)post;
	if (!status->stat_options)
		return;

	STAT_ENTER_SINGLE;
	{
		graph_entry_t *graph = graph_get_entry(irg, status->irg_hash);
		cnt_inc(&graph->cnt[gcnt_acc_walked_blocks]);
	}
	STAT_LEAVE;
}

static bool is_const(ir_node *const node)
{
	switch (get_irn_opcode(node)) {
	case iro_Address:
	case iro_Align:
	case iro_Const:
	case iro_Offset:
	case iro_Size:
		return true;
	default:
		return false;
	}
}

/**
 * Called for every node that is removed due to an optimization.
 *
 * @param n     the IR node that will be removed
 * @param hmap  the hash map containing ir_op* -> opt_entry_t*
 * @param kind  the optimization kind
 */
static void removed_due_opt(ir_node *n, hmap_opt_entry_t *hmap, hook_opt_kind kind)
{
	/* ignore CSE for Constants */
	if (kind == HOOK_OPT_CSE && is_const(n))
		return;

	/* increase global value */
	op_id_t      const op_id = stat_get_irn_op(n);
	opt_entry_t *const entry = opt_get_entry(op_id, hmap);
	cnt_inc(&entry->count);
}

/**
 * Hook: Some nodes were optimized into some others due to an optimization.
 *
 * @param ctx  the hook context
 */
static void stat_merge_nodes(void *ctx, ir_node *const *new_node_array, int new_num_entries, ir_node *const *old_node_array, int old_num_entries, hook_opt_kind opt)
{
	(void)ctx;
	if (!status->stat_options)
		return;

	STAT_ENTER;
	{
		assert(old_num_entries > 0);
		graph_entry_t *const graph = get_graph_entry(old_node_array[0], status->irg_hash);

		cnt_inc(&status->num_opts[opt]);
		if (status->reassoc_run)
			opt = HOOK_OPT_REASSOC;

		for (int i = 0; i < old_num_entries; ++i) {
			/* nodes might be in new and old, so if we found a node
			   in both sets, this one is NOT removed */
			int j;
			for (j = 0; j < new_num_entries; ++j) {
				if (old_node_array[i] == new_node_array[j])
					break;
			}
			if (j >= new_num_entries) {
				int xopt = opt;

				/* sometimes we did not detect, that it is replaced by a Const */
				if (opt == HOOK_OPT_CONFIRM && new_num_entries == 1) {
					ir_node *const irn = new_node_array[0];
					if (is_const(irn))
						xopt = HOOK_OPT_CONFIRM_C;
				}

				removed_due_opt(old_node_array[i], graph->opt_hash[xopt], (hook_opt_kind)xopt);
			}
		}
	}
	STAT_LEAVE;
}

/**
 * Hook: Reassociation is started/stopped.
 *
 * @param ctx   the hook context
 * @param flag  if non-zero, reassociation is started else stopped
 */
static void stat_reassociate(void *ctx, int flag)
{
	(void)ctx;
	if (!status->stat_options)
		return;

	STAT_ENTER;
	{
		status->reassoc_run = flag;
	}
	STAT_LEAVE;
}

/**
 * Hook: A node was lowered into other nodes
 *
 * @param ctx  the hook context
 * @param node the IR node that will be lowered
 */
static void stat_lower(void *ctx, ir_node *node)
{
	(void)ctx;
	if (!status->stat_options)
		return;

	STAT_ENTER;
	{
		graph_entry_t *const graph = get_graph_entry(node, status->irg_hash);
		removed_due_opt(node, graph->opt_hash[HOOK_LOWERED], HOOK_LOWERED);
	}
	STAT_LEAVE;
}

/**
 * Hook: A graph was inlined.
 *
 * @param ctx  the hook context
 * @param call the IR call that will re changed into the body of
 *             the called IR graph
 * @param called_irg  the IR graph representing the called routine
 */
static void stat_inline(void *ctx, ir_node *call, ir_graph *called_irg)
{
	(void)ctx;
	if (!status->stat_options)
		return;

	STAT_ENTER;
	{
		graph_entry_t *const graph = get_graph_entry(call, status->irg_hash);
		cnt_inc(&graph->cnt[gcnt_acc_got_inlined]);
		graph_entry_t *const i_graph = graph_get_entry(called_irg, status->irg_hash);
		cnt_inc(&i_graph->cnt[gcnt_acc_was_inlined]);
	}
	STAT_LEAVE;
}

/**
 * Hook: A graph with tail-recursions was optimized.
 *
 * @param ctx  the hook context
 */
static void stat_tail_rec(void *ctx, ir_graph *irg, int n_calls)
{
	(void)ctx;
	if (!status->stat_options)
		return;

	STAT_ENTER;
	{
		graph_entry_t *const graph = graph_get_entry(irg, status->irg_hash);
		graph->num_tail_recursion += n_calls;
	}
	STAT_LEAVE;
}

/**
 * Strength reduction was performed on an iteration variable.
 *
 * @param ctx  the hook context
 */
static void stat_strength_red(void *ctx, ir_graph *irg, ir_node *strong)
{
	(void)ctx;
	if (!status->stat_options)
		return;

	STAT_ENTER;
	{
		graph_entry_t *const graph = graph_get_entry(irg, status->irg_hash);
		cnt_inc(&graph->cnt[gcnt_acc_strength_red]);

		removed_due_opt(strong, graph->opt_hash[HOOK_OPT_STRENGTH_RED], HOOK_OPT_STRENGTH_RED);
	}
	STAT_LEAVE;
}

/**
 * Hook: Start/Stop the dead node elimination.
 *
 * @param ctx  the hook context
 */
static void stat_dead_node_elim(void *ctx, ir_graph *irg, int start)
{
	(void)ctx;
	(void)irg;
	if (!status->stat_options)
		return;

	status->in_dead_node_elim = (start != 0);
}

/**
 * Hook: real function call was optimized.
 */
static void stat_func_call(void *context, ir_graph *irg, ir_node *call)
{
	(void)context;
	(void)call;
	if (!status->stat_options)
		return;

	STAT_ENTER;
	{
		graph_entry_t *const graph = graph_get_entry(irg, status->irg_hash);
		cnt_inc(&graph->cnt[gcnt_acc_real_func_call]);
	}
	STAT_LEAVE;
}

/**
 * Hook: A multiply was replaced by a series of Shifts/Adds/Subs.
 *
 * @param ctx  the hook context
 */
static void stat_arch_dep_replace_mul_with_shifts(void *ctx, ir_node *mul)
{
	(void)ctx;
	if (!status->stat_options)
		return;

	STAT_ENTER;
	{
		graph_entry_t *const graph = get_graph_entry(mul, status->irg_hash);
		removed_due_opt(mul, graph->opt_hash[HOOK_OPT_ARCH_DEP], HOOK_OPT_ARCH_DEP);
	}
	STAT_LEAVE;
}

/**
 * Hook: A division by const was replaced.
 *
 * @param ctx   the hook context
 * @param node  the division node that will be optimized
 */
static void stat_arch_dep_replace_division_by_const(void *ctx, ir_node *node)
{
	(void)ctx;
	if (!status->stat_options)
		return;

	STAT_ENTER;
	{
		graph_entry_t *const graph = get_graph_entry(node, status->irg_hash);
		removed_due_opt(node, graph->opt_hash[HOOK_OPT_ARCH_DEP], HOOK_OPT_ARCH_DEP);
	}
	STAT_LEAVE;
}

/*
 * Update the register pressure of a block.
 *
 * @param irg        the irg containing the block
 * @param block      the block for which the reg pressure should be set
 * @param pressure   the pressure
 * @param class_name the name of the register class
 */
void stat_be_block_regpressure(ir_graph *irg, ir_node *block, int pressure, const char *class_name)
{
	if (!status->stat_options)
		return;

	STAT_ENTER;
	{
		graph_entry_t        *const graph     = graph_get_entry(irg, status->irg_hash);
		be_block_entry_t     *const block_ent = be_block_get_entry(block, graph->be_block_hash);
		reg_pressure_entry_t *const rp_ent    = OALLOCZ(&status->be_data, reg_pressure_entry_t);

		rp_ent->class_name = class_name;
		rp_ent->pressure   = pressure;

		pset_insert(block_ent->reg_pressure, rp_ent, hash_ptr(class_name));
	}
	STAT_LEAVE;
}

/* Dumps a statistics snapshot. */
void stat_dump_snapshot(const char *name, const char *phase)
{
	if (!status->stat_options)
		return;

	STAT_ENTER;
	{
		graph_entry_t *const global = graph_get_entry(NULL, status->irg_hash);

		/* The constant counter is only global, so we clear it here.
		 * Note that it does NOT contain the constants in DELETED
		 * graphs due to this. */
		if (status->stat_options & FIRMSTAT_COUNT_CONSTS)
			stat_const_clear(status);

		/* build the name */
		char const *p = strrchr(name, '/');
#ifdef _WIN32
		{
			char const *const q = strrchr(name, '\\');
			/* NULL might be not the smallest pointer */
			if (q && (!p || q > p))
				p = q;
		}
#endif /* _WIN32 */
		p = p ? p + 1 : name;

		char fname[2048];
		snprintf(fname, sizeof(fname), "%.*sfirmstat-%s-%s", (int)(p - name), name, phase, p);

		stat_dump_init(fname);

		/* calculate the graph statistics */
		foreach_pset(status->irg_hash, graph_entry_t, entry) {
			/* special entry for the global count */
			if (!entry->irg)
				continue;
			if (!entry->is_deleted) {
				/* the graph is still alive, count the nodes on it */
				update_graph_stat(global, entry);
			}
		}

		/* some calculations are dependent, we pushed them on the wait_q */
		while (!pdeq_empty(status->wait_q)) {
			graph_entry_t *const entry = (graph_entry_t*)pdeq_getr(status->wait_q);
			update_graph_stat_2(entry);
		}

		/* dump per graph */
		foreach_pset(status->irg_hash, graph_entry_t, entry) {
			/* special entry for the global count */
			if (!entry->irg)
				continue;

			if (!entry->is_deleted || status->stat_options & FIRMSTAT_COUNT_DELETED) {
				stat_dump_graph(entry);
			}

			if (!entry->is_deleted) {
				/* clear the counter that are not accumulated */
				graph_clear_entry(entry, 0);
			}
		}

		/* dump global */
		stat_dump_graph(global);

		/* dump the const info */
		if (status->stat_options & FIRMSTAT_COUNT_CONSTS)
			stat_dump_consts(&status->const_info);

		/* dump the parameter distribution */
		stat_dump_param_tbl(status->dist_param_cnt, global);

		/* dump the optimization counter and clear them */
		stat_dump_opt_cnt(status->num_opts, ARRAY_SIZE(status->num_opts));
		clear_optimization_counter();

		stat_dump_finish();

		stat_finish_pattern_history(fname);

		/* clear the global counters here */
		foreach_pset(global->opcode_hash, node_entry_t, entry) {
			opcode_clear_entry(entry);
		}
		/* clear all global counter */
		graph_clear_entry(global, /*all=*/1);
	}
	STAT_LEAVE;
}

/** the hook entries for the Firm statistics module */
static hook_entry_t stat_hooks[hook_last];

/* initialize the statistics module. */
void firm_init_stat(void)
{
#define HOOK(h, fkt) \
	stat_hooks[h].hook._##h = fkt; register_hook(h, &stat_hooks[h])

	if (!(stat_options & FIRMSTAT_ENABLED))
		return;

	status = XMALLOCZ(stat_info_t);

	/* enable statistics */
	status->stat_options = stat_options & FIRMSTAT_ENABLED ? stat_options : 0;

	/* register all hooks */
	HOOK(hook_new_ir_op,                          stat_new_ir_op);
	HOOK(hook_free_ir_op,                         stat_free_ir_op);
	HOOK(hook_new_node,                           stat_new_node);
	HOOK(hook_turn_into_id,                       stat_turn_into_id);
	HOOK(hook_normalize,                          stat_normalize);
	HOOK(hook_new_graph,                          stat_new_graph);
	HOOK(hook_free_graph,                         stat_free_graph);
	HOOK(hook_irg_walk,                           stat_irg_walk);
	HOOK(hook_irg_walk_blkwise,                   stat_irg_walk_blkwise);
	HOOK(hook_irg_block_walk,                     stat_irg_block_walk);
	HOOK(hook_merge_nodes,                        stat_merge_nodes);
	HOOK(hook_reassociate,                        stat_reassociate);
	HOOK(hook_lower,                              stat_lower);
	HOOK(hook_inline,                             stat_inline);
	HOOK(hook_tail_rec,                           stat_tail_rec);
	HOOK(hook_strength_red,                       stat_strength_red);
	HOOK(hook_dead_node_elim,                     stat_dead_node_elim);
	HOOK(hook_func_call,                          stat_func_call);
	HOOK(hook_arch_dep_replace_mul_with_shifts,   stat_arch_dep_replace_mul_with_shifts);
	HOOK(hook_arch_dep_replace_division_by_const, stat_arch_dep_replace_division_by_const);

	obstack_init(&status->cnts);
	obstack_init(&status->be_data);

	/* create the hash-tables */
	status->irg_hash   = new_pset(graph_cmp, 8);
	status->ir_op_hash = new_pset(opcode_cmp_2, 1);

	/* create the wait queue */
	status->wait_q     = new_pdeq();

	if (stat_options & FIRMSTAT_COUNT_STRONG_OP) {
		status->op_Phi0  = "Phi0";
		status->op_PhiM  = "PhiM";
		status->op_ProjM = "ProjM";
		status->op_MulC  = "MulC";
		status->op_DivC  = "DivC";
		status->op_ModC  = "ModC";
	} else {
		status->op_Phi0  = NULL;
		status->op_PhiM  = NULL;
		status->op_ProjM = NULL;
		status->op_MulC  = NULL;
		status->op_DivC  = NULL;
		status->op_ModC  = NULL;
	}

	/* for Florian: count the Sel depth */
	if (stat_options & FIRMSTAT_COUNT_SELS) {
		status->op_SelSel    = "Sel(Sel)";
		status->op_SelSelSel = "Sel(Sel(Sel))";
	} else {
		status->op_SelSel    = NULL;
		status->op_SelSelSel = NULL;
	}

	/* register the dumper */
	stat_register_dumper(&simple_dumper);

	if (stat_options & FIRMSTAT_CSV_OUTPUT)
		stat_register_dumper(&csv_dumper);

	/* initialize the pattern hash */
	stat_init_pattern_history(stat_options & FIRMSTAT_PATTERN_ENABLED);

	/* initialize the Const options */
	if (stat_options & FIRMSTAT_COUNT_CONSTS)
		stat_init_const_cnt(status);

	/* distribution table for parameter counts */
	status->dist_param_cnt = stat_new_int_distrib_tbl();

	clear_optimization_counter();

#undef HOOK
}

/**
 * Frees all dumper structures.
 */
static void stat_term_dumper(void)
{
	for (dumper_t *dumper = status->dumper; dumper;) {
		dumper_t *const next_dumper = dumper->next;
		free(dumper);
		dumper = next_dumper;
	}
}


/* Terminates the statistics module, frees all memory. */
void stat_term(void)
{
	if (status) {
		obstack_free(&status->be_data, NULL);
		obstack_free(&status->cnts, NULL);

		stat_term_dumper();

		free(status);
		status = NULL;
	}
}

/* returns 1 if statistics were initialized, 0 otherwise */
int stat_is_active(void)
{
	return status != NULL;
}

void init_stat(void)
{
	lc_opt_entry_t *root_grp = firm_opt_get_root();
	lc_opt_entry_t *be_grp   = lc_opt_get_grp(root_grp, "be");

	static const lc_opt_enum_mask_items_t stat_items[] = {
		{ "enabled",         FIRMSTAT_ENABLED         },
		{ "pattern",         FIRMSTAT_PATTERN_ENABLED },
		{ "count_strong_op", FIRMSTAT_COUNT_STRONG_OP },
		{ "count_dag",       FIRMSTAT_COUNT_DAG       },
		{ "count_deleted",   FIRMSTAT_COUNT_DELETED   },
		{ "count_sels",      FIRMSTAT_COUNT_SELS      },
		{ "count_consts",    FIRMSTAT_COUNT_CONSTS    },
		{ "csv_output",      FIRMSTAT_CSV_OUTPUT      },
		{ NULL,              0 }
	};
	static lc_opt_enum_mask_var_t statmask = { &stat_options, stat_items };
	static const lc_opt_table_entry_t stat_optionstable[] = {
		LC_OPT_ENT_ENUM_MASK("statistics", "enable statistics", &statmask),
		LC_OPT_LAST
	};
	lc_opt_add_table(be_grp, stat_optionstable);
}
