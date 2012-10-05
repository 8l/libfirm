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
 * @brief   Lower double word operations, i.e. 64bit -> 32bit, 32bit -> 16bit etc.
 * @date    8.10.2004
 * @author  Michael Beck
 */
#include "config.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>

#include "be.h"
#include "error.h"
#include "lowering.h"
#include "irnode_t.h"
#include "irnodeset.h"
#include "irgraph_t.h"
#include "irmode_t.h"
#include "iropt_t.h"
#include "irgmod.h"
#include "tv_t.h"
#include "dbginfo_t.h"
#include "iropt_dbg.h"
#include "irflag_t.h"
#include "firmstat.h"
#include "irgwalk.h"
#include "ircons.h"
#include "irflag.h"
#include "iroptimize.h"
#include "irtools.h"
#include "debug.h"
#include "set.h"
#include "pmap.h"
#include "pdeq.h"
#include "irdump.h"
#include "array_t.h"
#include "irpass_t.h"
#include "lower_dw.h"

/** A map from (op, imode, omode) to Intrinsic functions entities. */
static set *intrinsic_fkt;

/** A map from (imode, omode) to conv function types. */
static set *conv_types;

/** A map from a method type to its lowered type. */
static pmap *lowered_type;

/** A map from a builtin type to its lower and higher type. */
static pmap *lowered_builtin_type_high;
static pmap *lowered_builtin_type_low;

/** The types for the binop and unop intrinsics. */
static ir_type *binop_tp_u, *binop_tp_s, *unop_tp_u, *unop_tp_s, *tp_s, *tp_u;

static ir_nodeset_t created_mux_nodes;

/** the debug handle */
DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

/**
 * An entry in the (op, imode, omode) -> entity map.
 */
typedef struct op_mode_entry {
	const ir_op   *op;    /**< the op */
	const ir_mode *imode; /**< the input mode */
	const ir_mode *omode; /**< the output mode */
	ir_entity     *ent;   /**< the associated entity of this (op, imode, omode) triple */
} op_mode_entry_t;

/**
 * An entry in the (imode, omode) -> tp map.
 */
typedef struct conv_tp_entry {
	const ir_mode *imode; /**< the input mode */
	const ir_mode *omode; /**< the output mode */
	ir_type       *mtd;   /**< the associated method type of this (imode, omode) pair */
} conv_tp_entry_t;

enum lower_flags {
	MUST_BE_LOWERED = 1,  /**< graph must be lowered */
	CF_CHANGED      = 2,  /**< control flow was changed */
};

/**
 * The lower environment.
 */
typedef struct lower_dw_env_t {
	lower64_entry_t **entries;     /**< entries per node */
	ir_graph      *irg;
	struct obstack obst;           /**< an obstack holding the temporary data */
	ir_tarval *tv_mode_bytes;      /**< a tarval containing the number of bytes in the lowered modes */
	pdeq      *waitq;              /**< a wait queue of all nodes that must be handled later */
	ir_node  **lowered_phis;       /**< list of lowered phis */
	ir_mode   *high_signed;        /**< doubleword signed type */
	ir_mode   *high_unsigned;      /**< doubleword unsigned type */
	ir_mode   *low_signed;         /**< word signed type */
	ir_mode   *low_unsigned;       /**< word unsigned type */
	ident     *first_id;           /**< .l for little and .h for big endian */
	ident     *next_id;            /**< .h for little and .l for big endian */
	const lwrdw_param_t *params;   /**< transformation parameter */
	unsigned flags;                /**< some flags */
	unsigned n_entries;            /**< number of entries */
} lower_dw_env_t;

static lower_dw_env_t *env;

static void lower_node(ir_node *node);

/**
 * Create a method type for a Conv emulation from imode to omode.
 */
static ir_type *get_conv_type(ir_mode *imode, ir_mode *omode)
{
	conv_tp_entry_t key, *entry;
	ir_type *mtd;

	key.imode = imode;
	key.omode = omode;
	key.mtd   = NULL;

	entry = set_insert(conv_tp_entry_t, conv_types, &key, sizeof(key), hash_ptr(imode) ^ hash_ptr(omode));
	if (! entry->mtd) {
		int n_param = 1, n_res = 1;

		if (imode == env->high_signed || imode == env->high_unsigned)
			n_param = 2;
		if (omode == env->high_signed || omode == env->high_unsigned)
			n_res = 2;

		/* create a new one */
		mtd = new_type_method(n_param, n_res);

		/* set param types and result types */
		n_param = 0;
		if (imode == env->high_signed) {
			if (env->params->little_endian) {
				set_method_param_type(mtd, n_param++, tp_u);
				set_method_param_type(mtd, n_param++, tp_s);
			} else {
				set_method_param_type(mtd, n_param++, tp_s);
				set_method_param_type(mtd, n_param++, tp_u);
			}
		} else if (imode == env->high_unsigned) {
			set_method_param_type(mtd, n_param++, tp_u);
			set_method_param_type(mtd, n_param++, tp_u);
		} else {
			ir_type *tp = get_type_for_mode(imode);
			set_method_param_type(mtd, n_param++, tp);
		}

		n_res = 0;
		if (omode == env->high_signed) {
			if (env->params->little_endian) {
				set_method_res_type(mtd, n_res++, tp_u);
				set_method_res_type(mtd, n_res++, tp_s);
			} else {
				set_method_res_type(mtd, n_res++, tp_s);
				set_method_res_type(mtd, n_res++, tp_u);
			}
		} else if (omode == env->high_unsigned) {
			set_method_res_type(mtd, n_res++, tp_u);
			set_method_res_type(mtd, n_res++, tp_u);
		} else {
			ir_type *tp = get_type_for_mode(omode);
			set_method_res_type(mtd, n_res++, tp);
		}
		entry->mtd = mtd;
	} else {
		mtd = entry->mtd;
	}
	return mtd;
}

/**
 * Add an additional control flow input to a block.
 * Patch all Phi nodes. The new Phi inputs are copied from
 * old input number nr.
 */
static void add_block_cf_input_nr(ir_node *block, int nr, ir_node *cf)
{
	int i, arity = get_irn_arity(block);
	ir_node **in;

	assert(nr < arity);

	NEW_ARR_A(ir_node *, in, arity + 1);
	for (i = 0; i < arity; ++i)
		in[i] = get_irn_n(block, i);
	in[i] = cf;

	set_irn_in(block, i + 1, in);

	foreach_out_edge(block, edge) {
		ir_node *phi = get_edge_src_irn(edge);
		if (!is_Phi(phi))
			continue;

		for (i = 0; i < arity; ++i)
			in[i] = get_irn_n(phi, i);
		in[i] = in[nr];
		set_irn_in(phi, i + 1, in);
	}
}

/**
 * Add an additional control flow input to a block.
 * Patch all Phi nodes. The new Phi inputs are copied from
 * old input from cf tmpl.
 */
static void add_block_cf_input(ir_node *block, ir_node *tmpl, ir_node *cf)
{
	int i, arity = get_irn_arity(block);
	int nr = 0;

	for (i = 0; i < arity; ++i) {
		if (get_irn_n(block, i) == tmpl) {
			nr = i;
			break;
		}
	}
	assert(i < arity);
	add_block_cf_input_nr(block, nr, cf);
}

/**
 * Return the "operational" mode of a Firm node.
 */
static ir_mode *get_irn_op_mode(ir_node *node)
{
	switch (get_irn_opcode(node)) {
	case iro_Load:
		return get_Load_mode(node);
	case iro_Store:
		return get_irn_mode(get_Store_value(node));
	case iro_Div:
		return get_irn_mode(get_Div_left(node));
	case iro_Mod:
		return get_irn_mode(get_Mod_left(node));
	case iro_Cmp:
		return get_irn_mode(get_Cmp_left(node));
	default:
		return get_irn_mode(node);
	}
}

/**
 * Walker, prepare the node links and determine which nodes need to be lowered
 * at all.
 */
static void prepare_links(ir_node *node)
{
	ir_mode         *mode = get_irn_op_mode(node);
	lower64_entry_t *link;

	if (mode == env->high_signed || mode == env->high_unsigned) {
		unsigned idx = get_irn_idx(node);
		/* ok, found a node that will be lowered */
		link = OALLOCZ(&env->obst, lower64_entry_t);

		if (idx >= env->n_entries) {
			/* enlarge: this happens only for Rotl nodes which is RARELY */
			unsigned old   = env->n_entries;
			unsigned n_idx = idx + (idx >> 3);

			ARR_RESIZE(lower64_entry_t *, env->entries, n_idx);
			memset(&env->entries[old], 0, (n_idx - old) * sizeof(env->entries[0]));
			env->n_entries = n_idx;
		}
		env->entries[idx] = link;
		env->flags |= MUST_BE_LOWERED;
	} else if (is_Conv(node)) {
		/* Conv nodes have two modes */
		ir_node *pred = get_Conv_op(node);
		mode = get_irn_mode(pred);

		if (mode == env->high_signed || mode == env->high_unsigned) {
			/* must lower this node either but don't need a link */
			env->flags |= MUST_BE_LOWERED;
		}
		return;
	} else if (is_Call(node)) {
		/* Special case:  If the result of the Call is never used, we won't
		 * find a Proj with a mode that potentially triggers MUST_BE_LOWERED
		 * to be set.  Thus, if we see a call, we check its result types and
		 * decide whether MUST_BE_LOWERED has to be set.
		 */
		ir_type *tp = get_Call_type(node);
		size_t   n_res, i;

		n_res = get_method_n_ress(tp);
		for (i = 0; i < n_res; ++i) {
			ir_type *rtp = get_method_res_type(tp, i);

			if (is_Primitive_type(rtp)) {
				ir_mode *rmode = get_type_mode(rtp);

				if (rmode == env->high_signed || rmode == env->high_unsigned) {
					env->flags |= MUST_BE_LOWERED;
				}
			}
		}
	}
}

lower64_entry_t *get_node_entry(ir_node *node)
{
	unsigned idx = get_irn_idx(node);
	assert(idx < env->n_entries);
	return env->entries[idx];
}

void ir_set_dw_lowered(ir_node *old, ir_node *new_low, ir_node *new_high)
{
	lower64_entry_t *entry = get_node_entry(old);
	entry->low_word  = new_low;
	entry->high_word = new_high;
}

ir_mode *ir_get_low_unsigned_mode(void)
{
	return env->low_unsigned;
}

/**
 * Translate a Constant: create two.
 */
static void lower_Const(ir_node *node, ir_mode *mode)
{
	ir_graph  *irg      = get_irn_irg(node);
	dbg_info  *dbg      = get_irn_dbg_info(node);
	ir_mode   *low_mode = env->low_unsigned;
	ir_tarval *tv       = get_Const_tarval(node);
	ir_tarval *tv_l     = tarval_convert_to(tv, low_mode);
	ir_node   *res_low  = new_rd_Const(dbg, irg, tv_l);
	ir_tarval *tv_shrs  = tarval_shrs_unsigned(tv, get_mode_size_bits(low_mode));
	ir_tarval *tv_h     = tarval_convert_to(tv_shrs, mode);
	ir_node   *res_high = new_rd_Const(dbg, irg, tv_h);

	ir_set_dw_lowered(node, res_low, res_high);
}

/**
 * Translate a Load: create two.
 */
static void lower_Load(ir_node *node, ir_mode *mode)
{
	ir_mode    *low_mode = env->low_unsigned;
	ir_graph   *irg = get_irn_irg(node);
	ir_node    *adr = get_Load_ptr(node);
	ir_node    *mem = get_Load_mem(node);
	ir_node    *low;
	ir_node    *high;
	ir_node    *proj_m;
	dbg_info   *dbg;
	ir_node    *block = get_nodes_block(node);
	ir_cons_flags volatility = get_Load_volatility(node) == volatility_is_volatile
	                         ? cons_volatile : cons_none;

	if (env->params->little_endian) {
		low  = adr;
		high = new_r_Add(block, adr, new_r_Const(irg, env->tv_mode_bytes), get_irn_mode(adr));
	} else {
		low  = new_r_Add(block, adr, new_r_Const(irg, env->tv_mode_bytes), get_irn_mode(adr));
		high = adr;
	}

	/* create two loads */
	dbg    = get_irn_dbg_info(node);
	low    = new_rd_Load(dbg, block, mem,  low,  low_mode, volatility);
	proj_m = new_r_Proj(low, mode_M, pn_Load_M);
	high   = new_rd_Load(dbg, block, proj_m, high, mode, volatility);

	foreach_out_edge_safe(node, edge) {
		ir_node *proj = get_edge_src_irn(edge);
		if (!is_Proj(proj))
			continue;

		switch (get_Proj_proj(proj)) {
		case pn_Load_M:         /* Memory result. */
			/* put it to the second one */
			set_Proj_pred(proj, high);
			break;
		case pn_Load_X_except:  /* Execution result if exception occurred. */
			/* put it to the first one */
			set_Proj_pred(proj, low);
			break;
		case pn_Load_res: {       /* Result of load operation. */
			ir_node *res_low  = new_r_Proj(low,  low_mode, pn_Load_res);
			ir_node *res_high = new_r_Proj(high, mode,     pn_Load_res);
			ir_set_dw_lowered(proj, res_low, res_high);
			break;
		}
		default:
			assert(0 && "unexpected Proj number");
		}
		/* mark this proj: we have handled it already, otherwise we might fall
		 * into out new nodes. */
		mark_irn_visited(proj);
	}
}

/**
 * Translate a Store: create two.
 */
static void lower_Store(ir_node *node, ir_mode *mode)
{
	ir_graph              *irg;
	ir_node               *block, *adr, *mem;
	ir_node               *low, *high, *proj_m;
	dbg_info              *dbg;
	ir_node               *value = get_Store_value(node);
	const lower64_entry_t *entry = get_node_entry(value);
	ir_cons_flags volatility = get_Store_volatility(node) == volatility_is_volatile
	                           ? cons_volatile : cons_none;
	(void) mode;

	assert(entry);

	if (! entry->low_word) {
		/* not ready yet, wait */
		pdeq_putr(env->waitq, node);
		return;
	}

	irg = get_irn_irg(node);
	adr = get_Store_ptr(node);
	mem = get_Store_mem(node);
	block = get_nodes_block(node);

	if (env->params->little_endian) {
		low  = adr;
		high = new_r_Add(block, adr, new_r_Const(irg, env->tv_mode_bytes), get_irn_mode(adr));
	} else {
		low  = new_r_Add(block, adr, new_r_Const(irg, env->tv_mode_bytes), get_irn_mode(adr));
		high = adr;
	}

	/* create two Stores */
	dbg    = get_irn_dbg_info(node);
	low    = new_rd_Store(dbg, block, mem, low,  entry->low_word, volatility);
	proj_m = new_r_Proj(low, mode_M, pn_Store_M);
	high   = new_rd_Store(dbg, block, proj_m, high, entry->high_word, volatility);

	foreach_out_edge_safe(node, edge) {
		ir_node *proj = get_edge_src_irn(edge);
		if (!is_Proj(proj))
			continue;

		switch (get_Proj_proj(proj)) {
		case pn_Store_M:         /* Memory result. */
			/* put it to the second one */
			set_Proj_pred(proj, high);
			break;
		case pn_Store_X_except:  /* Execution result if exception occurred. */
			/* put it to the first one */
			set_Proj_pred(proj, low);
			break;
		default:
			assert(0 && "unexpected Proj number");
		}
		/* mark this proj: we have handled it already, otherwise we might fall into
		 * out new nodes. */
		mark_irn_visited(proj);
	}
}

/**
 * Return a node containing the address of the intrinsic emulation function.
 *
 * @param method  the method type of the emulation function
 * @param op      the emulated ir_op
 * @param imode   the input mode of the emulated opcode
 * @param omode   the output mode of the emulated opcode
 * @param env     the lower environment
 */
static ir_node *get_intrinsic_address(ir_type *method, ir_op *op,
                                      ir_mode *imode, ir_mode *omode)
{
	symconst_symbol sym;
	ir_entity *ent;
	op_mode_entry_t key, *entry;

	key.op    = op;
	key.imode = imode;
	key.omode = omode;
	key.ent   = NULL;

	entry = set_insert(op_mode_entry_t, intrinsic_fkt, &key, sizeof(key),
				hash_ptr(op) ^ hash_ptr(imode) ^ (hash_ptr(omode) << 8));
	if (! entry->ent) {
		/* create a new one */
		ent = env->params->create_intrinsic(method, op, imode, omode, env->params->ctx);

		assert(ent && "Intrinsic creator must return an entity");
		entry->ent = ent;
	} else {
		ent = entry->ent;
	}
	sym.entity_p = ent;
	return new_r_SymConst(env->irg, mode_P_code, sym, symconst_addr_ent);
}

/**
 * Translate a Div.
 *
 * Create an intrinsic Call.
 */
static void lower_Div(ir_node *node, ir_mode *mode)
{
	ir_node  *left   = get_Div_left(node);
	ir_node  *right  = get_Div_right(node);
	ir_node  *block  = get_nodes_block(node);
	dbg_info *dbgi   = get_irn_dbg_info(node);
	ir_type  *mtp    = mode_is_signed(mode) ? binop_tp_s : binop_tp_u;
	ir_mode  *opmode = get_irn_op_mode(node);
	ir_node  *addr   = get_intrinsic_address(mtp, get_irn_op(node), opmode, opmode);
	ir_node  *in[4];
	ir_node  *call;
	ir_node  *resproj;

	if (env->params->little_endian) {
		in[0] = get_lowered_low(left);
		in[1] = get_lowered_high(left);
		in[2] = get_lowered_low(right);
		in[3] = get_lowered_high(right);
	} else {
		in[0] = get_lowered_high(left);
		in[1] = get_lowered_low(left);
		in[2] = get_lowered_high(right);
		in[3] = get_lowered_low(right);
	}
	call    = new_rd_Call(dbgi, block, get_Div_mem(node), addr, 4, in, mtp);
	resproj = new_r_Proj(call, mode_T, pn_Call_T_result);
	set_irn_pinned(call, get_irn_pinned(node));

	foreach_out_edge_safe(node, edge) {
		ir_node *proj = get_edge_src_irn(edge);
		if (!is_Proj(proj))
			continue;

		switch (get_Proj_proj(proj)) {
		case pn_Div_M:         /* Memory result. */
			/* reroute to the call */
			set_Proj_pred(proj, call);
			set_Proj_proj(proj, pn_Call_M);
			break;
		case pn_Div_X_regular:
			set_Proj_pred(proj, call);
			set_Proj_proj(proj, pn_Call_X_regular);
			break;
		case pn_Div_X_except:
			set_Proj_pred(proj, call);
			set_Proj_proj(proj, pn_Call_X_except);
			break;
		case pn_Div_res:
			if (env->params->little_endian) {
				ir_node *res_low  = new_r_Proj(resproj, env->low_unsigned, 0);
				ir_node *res_high = new_r_Proj(resproj, mode,              1);
				ir_set_dw_lowered(proj, res_low, res_high);
			} else {
				ir_node *res_low  = new_r_Proj(resproj, env->low_unsigned, 1);
				ir_node *res_high = new_r_Proj(resproj, mode,              0);
				ir_set_dw_lowered(proj, res_low, res_high);
			}
			break;
		default:
			assert(0 && "unexpected Proj number");
		}
		/* mark this proj: we have handled it already, otherwise we might fall into
		 * out new nodes. */
		mark_irn_visited(proj);
	}
}

/**
 * Translate a Mod.
 *
 * Create an intrinsic Call.
 */
static void lower_Mod(ir_node *node, ir_mode *mode)
{
	ir_node  *left   = get_Mod_left(node);
	ir_node  *right  = get_Mod_right(node);
	dbg_info *dbgi   = get_irn_dbg_info(node);
	ir_node  *block  = get_nodes_block(node);
	ir_type  *mtp    = mode_is_signed(mode) ? binop_tp_s : binop_tp_u;
	ir_mode  *opmode = get_irn_op_mode(node);
	ir_node  *addr   = get_intrinsic_address(mtp, get_irn_op(node), opmode, opmode);
	ir_node  *in[4];
	ir_node  *call;
	ir_node  *resproj;

	if (env->params->little_endian) {
		in[0] = get_lowered_low(left);
		in[1] = get_lowered_high(left);
		in[2] = get_lowered_low(right);
		in[3] = get_lowered_high(right);
	} else {
		in[0] = get_lowered_high(left);
		in[1] = get_lowered_low(left);
		in[2] = get_lowered_high(right);
		in[3] = get_lowered_low(right);
	}
	call    = new_rd_Call(dbgi, block, get_Mod_mem(node), addr, 4, in, mtp);
	resproj = new_r_Proj(call, mode_T, pn_Call_T_result);
	set_irn_pinned(call, get_irn_pinned(node));

	foreach_out_edge_safe(node, edge) {
		ir_node *proj = get_edge_src_irn(edge);
		if (!is_Proj(proj))
			continue;

		switch (get_Proj_proj(proj)) {
		case pn_Mod_M:         /* Memory result. */
			/* reroute to the call */
			set_Proj_pred(proj, call);
			set_Proj_proj(proj, pn_Call_M);
			break;
		case pn_Div_X_regular:
			set_Proj_pred(proj, call);
			set_Proj_proj(proj, pn_Call_X_regular);
			break;
		case pn_Mod_X_except:
			set_Proj_pred(proj, call);
			set_Proj_proj(proj, pn_Call_X_except);
			break;
		case pn_Mod_res:
			if (env->params->little_endian) {
				ir_node *res_low  = new_r_Proj(resproj, env->low_unsigned, 0);
				ir_node *res_high = new_r_Proj(resproj, mode,              1);
				ir_set_dw_lowered(proj, res_low, res_high);
			} else {
				ir_node *res_low  = new_r_Proj(resproj, env->low_unsigned, 1);
				ir_node *res_high = new_r_Proj(resproj, mode,              0);
				ir_set_dw_lowered(proj, res_low, res_high);
			}
			break;
		default:
			assert(0 && "unexpected Proj number");
		}
		/* mark this proj: we have handled it already, otherwise we might fall
		 * into out new nodes. */
		mark_irn_visited(proj);
	}
}

/**
 * Translate a binop.
 *
 * Create an intrinsic Call.
 */
static void lower_binop(ir_node *node, ir_mode *mode)
{
	ir_node  *left  = get_binop_left(node);
	ir_node  *right = get_binop_right(node);
	dbg_info *dbgi  = get_irn_dbg_info(node);
	ir_node  *block = get_nodes_block(node);
	ir_graph *irg   = get_irn_irg(block);
	ir_type  *mtp   = mode_is_signed(mode) ? binop_tp_s : binop_tp_u;
	ir_node  *addr  = get_intrinsic_address(mtp, get_irn_op(node), mode, mode);
	ir_node  *in[4];
	ir_node  *call;
	ir_node  *resproj;

	if (env->params->little_endian) {
		in[0] = get_lowered_low(left);
		in[1] = get_lowered_high(left);
		in[2] = get_lowered_low(right);
		in[3] = get_lowered_high(right);
	} else {
		in[0] = get_lowered_high(left);
		in[1] = get_lowered_low(left);
		in[2] = get_lowered_high(right);
		in[3] = get_lowered_low(right);
	}
	call    = new_rd_Call(dbgi, block, get_irg_no_mem(irg), addr, 4, in, mtp);
	resproj = new_r_Proj(call, mode_T, pn_Call_T_result);
	set_irn_pinned(call, get_irn_pinned(node));

	if (env->params->little_endian) {
		ir_node *res_low  = new_r_Proj(resproj, env->low_unsigned, 0);
		ir_node *res_high = new_r_Proj(resproj, mode,              1);
		ir_set_dw_lowered(node, res_low, res_high);
	} else {
		ir_node *res_low  = new_r_Proj(resproj, env->low_unsigned, 1);
		ir_node *res_high = new_r_Proj(resproj, mode,              0);
		ir_set_dw_lowered(node, res_low, res_high);
	}
}

static ir_node *create_conv(ir_node *block, ir_node *node, ir_mode *dest_mode)
{
	if (get_irn_mode(node) == dest_mode)
		return node;
	return new_r_Conv(block, node, dest_mode);
}

/**
 * Moves node and all predecessors of node from from_bl to to_bl.
 * Does not move predecessors of Phi nodes (or block nodes).
 */
static void move(ir_node *node, ir_node *from_bl, ir_node *to_bl)
{
	int i, arity;

	/* move this node */
	set_nodes_block(node, to_bl);

	/* move its Projs */
	if (get_irn_mode(node) == mode_T) {
		foreach_out_edge(node, edge) {
			ir_node *proj = get_edge_src_irn(edge);
			if (!is_Proj(proj))
				continue;
			move(proj, from_bl, to_bl);
		}
	}

	/* We must not move predecessors of Phi nodes, even if they are in
	 * from_bl. (because these are values from an earlier loop iteration
	 * which are not predecessors of node here)
	 */
	if (is_Phi(node))
		return;

	/* recursion ... */
	arity = get_irn_arity(node);
	for (i = 0; i < arity; i++) {
		ir_node *pred      = get_irn_n(node, i);
		ir_mode *pred_mode = get_irn_mode(pred);
		if (get_nodes_block(pred) == from_bl)
			move(pred, from_bl, to_bl);
		if (pred_mode == env->high_signed || pred_mode == env->high_unsigned) {
			ir_node *pred_low  = get_lowered_low(pred);
			ir_node *pred_high = get_lowered_high(pred);
			if (get_nodes_block(pred_low) == from_bl)
				move(pred_low, from_bl, to_bl);
			if (pred_high != NULL && get_nodes_block(pred_high) == from_bl)
				move(pred_high, from_bl, to_bl);
		}
	}
}

/**
 * We need a custom version of part_block_edges because during transformation
 * not all data-dependencies are explicit yet if a lowered nodes users are not
 * lowered yet.
 * We can fix this by modifying move to look for such implicit dependencies.
 * Additionally we have to keep the proj_2_block map updated
 */
static ir_node *part_block_dw(ir_node *node)
{
	ir_graph *irg        = get_irn_irg(node);
	ir_node  *old_block  = get_nodes_block(node);
	int       n_cfgpreds = get_Block_n_cfgpreds(old_block);
	ir_node **cfgpreds   = get_Block_cfgpred_arr(old_block);
	ir_node  *new_block  = new_r_Block(irg, n_cfgpreds, cfgpreds);

	/* old_block has no predecessors anymore for now */
	set_irn_in(old_block, 0, NULL);

	/* move node and its predecessors to new_block */
	move(node, old_block, new_block);

	/* move Phi nodes to new_block */
	foreach_out_edge_safe(old_block, edge) {
		ir_node *phi = get_edge_src_irn(edge);
		if (!is_Phi(phi))
			continue;
		set_nodes_block(phi, new_block);
	}
	return old_block;
}

typedef ir_node* (*new_rd_shr_func)(dbg_info *dbgi, ir_node *block,
                                    ir_node *left, ir_node *right,
                                    ir_mode *mode);

static void lower_shr_helper(ir_node *node, ir_mode *mode,
                             new_rd_shr_func new_rd_shrs)
{
	ir_node  *right         = get_binop_right(node);
	ir_node  *left          = get_binop_left(node);
	ir_mode  *shr_mode      = get_irn_mode(node);
	unsigned  modulo_shift  = get_mode_modulo_shift(shr_mode);
	ir_mode  *low_unsigned  = env->low_unsigned;
	unsigned  modulo_shift2 = get_mode_modulo_shift(mode);
	ir_graph *irg           = get_irn_irg(node);
	ir_node  *left_low      = get_lowered_low(left);
	ir_node  *left_high     = get_lowered_high(left);
	dbg_info *dbgi          = get_irn_dbg_info(node);
	ir_node  *lower_block;
	ir_node  *block;
	ir_node  *cnst;
	ir_node  *andn;
	ir_node  *cmp;
	ir_node  *cond;
	ir_node  *proj_true;
	ir_node  *proj_false;
	ir_node  *phi_low;
	ir_node  *phi_high;
	ir_node  *lower_in[2];
	ir_node  *phi_low_in[2];
	ir_node  *phi_high_in[2];

	/* this version is optimized for modulo shift architectures
	 * (and can't handle anything else) */
	if (modulo_shift != get_mode_size_bits(shr_mode)
			|| modulo_shift2<<1 != modulo_shift) {
		panic("Shr lowering only implemented for modulo shift shr operations");
	}
	if (!is_po2(modulo_shift) || !is_po2(modulo_shift2)) {
		panic("Shr lowering only implemented for power-of-2 modes");
	}
	/* without 2-complement the -x instead of (bit_width-x) trick won't work */
	if (get_mode_arithmetic(shr_mode) != irma_twos_complement) {
		panic("Shr lowering only implemented for two-complement modes");
	}

	block = get_nodes_block(node);

	/* if the right operand is a 64bit value, we're only interested in the
	 * lower word */
	if (get_irn_mode(right) == env->high_unsigned) {
		right = get_lowered_low(right);
	} else {
		/* shift should never have signed mode on the right */
		assert(get_irn_mode(right) != env->high_signed);
		right = create_conv(block, right, low_unsigned);
	}

	lower_block = part_block_dw(node);
	env->flags |= CF_CHANGED;
	block = get_nodes_block(node);

	/* add a Cmp to test if highest bit is set <=> whether we shift more
	 * than half the word width */
	cnst       = new_r_Const_long(irg, low_unsigned, modulo_shift2);
	andn       = new_r_And(block, right, cnst, low_unsigned);
	cnst       = new_r_Const(irg, get_mode_null(low_unsigned));
	cmp        = new_rd_Cmp(dbgi, block, andn, cnst, ir_relation_equal);
	cond       = new_rd_Cond(dbgi, block, cmp);
	proj_true  = new_r_Proj(cond, mode_X, pn_Cond_true);
	proj_false = new_r_Proj(cond, mode_X, pn_Cond_false);

	/* the true block => shift_width < 1word */
	{
		/* In theory the low value (for 64bit shifts) is:
		 *    Or(High << (32-x)), Low >> x)
		 * In practice High << 32-x will fail when x is zero (since we have
		 * modulo shift and 32 will be 0). So instead we use:
		 *    Or(High<<1<<~x, Low >> x)
		 */
		ir_node *in[1]        = { proj_true };
		ir_node *block_true   = new_r_Block(irg, ARRAY_SIZE(in), in);
		ir_node *res_high     = new_rd_shrs(dbgi, block_true, left_high,
		                                    right, mode);
		ir_node *shift_low    = new_rd_Shr(dbgi, block_true, left_low, right,
		                                   low_unsigned);
		ir_node *not_shiftval = new_rd_Not(dbgi, block_true, right,
		                                   low_unsigned);
		ir_node *conv         = create_conv(block_true, left_high,
		                                    low_unsigned);
		ir_node *one          = new_r_Const(irg, get_mode_one(low_unsigned));
		ir_node *carry0       = new_rd_Shl(dbgi, block_true, conv, one,
		                                   low_unsigned);
		ir_node *carry1       = new_rd_Shl(dbgi, block_true, carry0,
		                                   not_shiftval, low_unsigned);
		ir_node *res_low      = new_rd_Or(dbgi, block_true, shift_low, carry1,
		                                  low_unsigned);
		lower_in[0]           = new_r_Jmp(block_true);
		phi_low_in[0]         = res_low;
		phi_high_in[0]        = res_high;
	}

	/* false block => shift_width > 1word */
	{
		ir_node *in[1]       = { proj_false };
		ir_node *block_false = new_r_Block(irg, ARRAY_SIZE(in), in);
		ir_node *conv        = create_conv(block_false, left_high, low_unsigned);
		ir_node *res_low     = new_rd_shrs(dbgi, block_false, conv, right,
		                                   low_unsigned);
		int      cnsti       = modulo_shift2-1;
		ir_node *cnst2       = new_r_Const_long(irg, low_unsigned, cnsti);
		ir_node *res_high;
		if (new_rd_shrs == new_rd_Shrs) {
			res_high = new_rd_shrs(dbgi, block_false, left_high, cnst2, mode);
		} else {
			res_high = new_r_Const(irg, get_mode_null(mode));
		}
		lower_in[1]          = new_r_Jmp(block_false);
		phi_low_in[1]        = res_low;
		phi_high_in[1]       = res_high;
	}

	/* patch lower block */
	set_irn_in(lower_block, ARRAY_SIZE(lower_in), lower_in);
	phi_low  = new_r_Phi(lower_block, ARRAY_SIZE(phi_low_in), phi_low_in,
	                     low_unsigned);
	phi_high = new_r_Phi(lower_block, ARRAY_SIZE(phi_high_in), phi_high_in,
	                     mode);
	ir_set_dw_lowered(node, phi_low, phi_high);
}

static void lower_Shr(ir_node *node, ir_mode *mode)
{
	lower_shr_helper(node, mode, new_rd_Shr);
}

static void lower_Shrs(ir_node *node, ir_mode *mode)
{
	lower_shr_helper(node, mode, new_rd_Shrs);
}

static void lower_Shl(ir_node *node, ir_mode *mode)
{
	ir_node  *right         = get_binop_right(node);
	ir_node  *left          = get_binop_left(node);
	ir_mode  *shr_mode      = get_irn_mode(node);
	unsigned  modulo_shift  = get_mode_modulo_shift(shr_mode);
	ir_mode  *low_unsigned  = env->low_unsigned;
	unsigned  modulo_shift2 = get_mode_modulo_shift(mode);
	ir_graph *irg           = get_irn_irg(node);
	ir_node  *left_low      = get_lowered_low(left);
	ir_node  *left_high     = get_lowered_high(left);
	dbg_info *dbgi          = get_irn_dbg_info(node);
	ir_node  *lower_block   = get_nodes_block(node);
	ir_node  *block;
	ir_node  *cnst;
	ir_node  *andn;
	ir_node  *cmp;
	ir_node  *cond;
	ir_node  *proj_true;
	ir_node  *proj_false;
	ir_node  *phi_low;
	ir_node  *phi_high;
	ir_node  *lower_in[2];
	ir_node  *phi_low_in[2];
	ir_node  *phi_high_in[2];

	/* this version is optimized for modulo shift architectures
	 * (and can't handle anything else) */
	if (modulo_shift != get_mode_size_bits(shr_mode)
			|| modulo_shift2<<1 != modulo_shift) {
		panic("Shl lowering only implemented for modulo shift shl operations");
	}
	if (!is_po2(modulo_shift) || !is_po2(modulo_shift2)) {
		panic("Shl lowering only implemented for power-of-2 modes");
	}
	/* without 2-complement the -x instead of (bit_width-x) trick won't work */
	if (get_mode_arithmetic(shr_mode) != irma_twos_complement) {
		panic("Shl lowering only implemented for two-complement modes");
	}

	/* if the right operand is a 64bit value, we're only interested in the
	 * lower word */
	if (get_irn_mode(right) == env->high_unsigned) {
		right = get_lowered_low(right);
	} else {
		/* shift should never have signed mode on the right */
		assert(get_irn_mode(right) != env->high_signed);
		right = create_conv(lower_block, right, low_unsigned);
	}

	part_block_dw(node);
	env->flags |= CF_CHANGED;
	block = get_nodes_block(node);

	/* add a Cmp to test if highest bit is set <=> whether we shift more
	 * than half the word width */
	cnst       = new_r_Const_long(irg, low_unsigned, modulo_shift2);
	andn       = new_r_And(block, right, cnst, low_unsigned);
	cnst       = new_r_Const(irg, get_mode_null(low_unsigned));
	cmp        = new_rd_Cmp(dbgi, block, andn, cnst, ir_relation_equal);
	cond       = new_rd_Cond(dbgi, block, cmp);
	proj_true  = new_r_Proj(cond, mode_X, pn_Cond_true);
	proj_false = new_r_Proj(cond, mode_X, pn_Cond_false);

	/* the true block => shift_width < 1word */
	{
		ir_node *in[1]        = { proj_true };
		ir_node *block_true   = new_r_Block(irg, ARRAY_SIZE(in), in);

		ir_node *res_low      = new_rd_Shl(dbgi, block_true, left_low,
		                                   right, low_unsigned);
		ir_node *shift_high   = new_rd_Shl(dbgi, block_true, left_high, right,
		                                   mode);
		ir_node *not_shiftval = new_rd_Not(dbgi, block_true, right,
		                                   low_unsigned);
		ir_node *conv         = create_conv(block_true, left_low, mode);
		ir_node *one          = new_r_Const(irg, get_mode_one(low_unsigned));
		ir_node *carry0       = new_rd_Shr(dbgi, block_true, conv, one, mode);
		ir_node *carry1       = new_rd_Shr(dbgi, block_true, carry0,
		                                   not_shiftval, mode);
		ir_node *res_high     = new_rd_Or(dbgi, block_true, shift_high, carry1,
		                                  mode);
		lower_in[0]           = new_r_Jmp(block_true);
		phi_low_in[0]         = res_low;
		phi_high_in[0]        = res_high;
	}

	/* false block => shift_width > 1word */
	{
		ir_node *in[1]       = { proj_false };
		ir_node *block_false = new_r_Block(irg, ARRAY_SIZE(in), in);
		ir_node *res_low     = new_r_Const(irg, get_mode_null(low_unsigned));
		ir_node *conv        = create_conv(block_false, left_low, mode);
		ir_node *res_high    = new_rd_Shl(dbgi, block_false, conv, right, mode);
		lower_in[1]          = new_r_Jmp(block_false);
		phi_low_in[1]        = res_low;
		phi_high_in[1]       = res_high;
	}

	/* patch lower block */
	set_irn_in(lower_block, ARRAY_SIZE(lower_in), lower_in);
	phi_low  = new_r_Phi(lower_block, ARRAY_SIZE(phi_low_in), phi_low_in,
	                     low_unsigned);
	phi_high = new_r_Phi(lower_block, ARRAY_SIZE(phi_high_in), phi_high_in,
	                     mode);
	ir_set_dw_lowered(node, phi_low, phi_high);
}

/**
 * Rebuild Rotl nodes into Or(Shl, Shr) and prepare all nodes.
 */
static void prepare_links_and_handle_rotl(ir_node *node, void *data)
{
	(void) data;
	if (is_Rotl(node)) {
		ir_mode  *mode = get_irn_op_mode(node);
		ir_node  *right;
		ir_node  *left, *shl, *shr, *ornode, *block, *sub, *c;
		ir_mode  *omode, *rmode;
		ir_graph *irg;
		dbg_info *dbg;
		optimization_state_t state;

		if (mode != env->high_signed && mode != env->high_unsigned) {
			prepare_links(node);
			return;
		}

		/* replace the Rotl(x,y) by an Or(Shl(x,y), Shr(x,64-y)) */
		right = get_Rotl_right(node);
		irg   = get_irn_irg(node);
		dbg   = get_irn_dbg_info(node);
		omode = get_irn_mode(node);
		left  = get_Rotl_left(node);
		block = get_nodes_block(node);
		shl   = new_rd_Shl(dbg, block, left, right, omode);
		rmode = get_irn_mode(right);
		c     = new_r_Const_long(irg, rmode, get_mode_size_bits(omode));
		sub   = new_rd_Sub(dbg, block, c, right, rmode);
		shr   = new_rd_Shr(dbg, block, left, sub, omode);

		/* switch optimization off here, or we will get the Rotl back */
		save_optimization_state(&state);
		set_opt_algebraic_simplification(0);
		ornode = new_rd_Or(dbg, block, shl, shr, omode);
		restore_optimization_state(&state);

		exchange(node, ornode);

		/* do lowering on the new nodes */
		prepare_links(shl);
		prepare_links(c);
		prepare_links(sub);
		prepare_links(shr);
		prepare_links(ornode);
		return;
	}

	prepare_links(node);
}

/**
 * Translate an Unop.
 *
 * Create an intrinsic Call.
 */
static void lower_unop(ir_node *node, ir_mode *mode)
{
	ir_node  *op       = get_unop_op(node);
	dbg_info *dbgi     = get_irn_dbg_info(node);
	ir_node  *block    = get_nodes_block(node);
	ir_graph *irg      = get_irn_irg(block);
	ir_type  *mtp      = mode_is_signed(mode) ? unop_tp_s : unop_tp_u;
	ir_op    *irop     = get_irn_op(node);
	ir_node  *addr     = get_intrinsic_address(mtp, irop, mode, mode);
	ir_node  *nomem    = get_irg_no_mem(irg);
	ir_node  *in[2];
	ir_node  *call;
	ir_node  *resproj;

	if (env->params->little_endian) {
		in[0] = get_lowered_low(op);
		in[1] = get_lowered_high(op);
	} else {
		in[0] = get_lowered_high(op);
		in[1] = get_lowered_low(op);
	}
	call    = new_rd_Call(dbgi, block, nomem, addr, 2, in, mtp);
	resproj = new_r_Proj(call, mode_T, pn_Call_T_result);
	set_irn_pinned(call, get_irn_pinned(node));

	if (env->params->little_endian) {
		ir_node *res_low  = new_r_Proj(resproj, env->low_unsigned, 0);
		ir_node *res_high = new_r_Proj(resproj, mode,              1);
		ir_set_dw_lowered(node, res_low, res_high);
	} else {
		ir_node *res_low  = new_r_Proj(resproj, env->low_unsigned, 1);
		ir_node *res_high = new_r_Proj(resproj, mode,              0);
		ir_set_dw_lowered(node, res_low, res_high);
	}
}

/**
 * Translate a logical binop.
 *
 * Create two logical binops.
 */
static void lower_binop_logical(ir_node *node, ir_mode *mode,
								ir_node *(*constr_rd)(dbg_info *db, ir_node *block, ir_node *op1, ir_node *op2, ir_mode *mode) )
{
	ir_node               *left        = get_binop_left(node);
	ir_node               *right       = get_binop_right(node);
	const lower64_entry_t *left_entry  = get_node_entry(left);
	const lower64_entry_t *right_entry = get_node_entry(right);
	dbg_info              *dbgi        = get_irn_dbg_info(node);
	ir_node               *block       = get_nodes_block(node);
	ir_node               *res_low
		= constr_rd(dbgi, block, left_entry->low_word, right_entry->low_word,
		            env->low_unsigned);
	ir_node               *res_high
		= constr_rd(dbgi, block, left_entry->high_word, right_entry->high_word,
		            mode);
	ir_set_dw_lowered(node, res_low, res_high);
}

static void lower_And(ir_node *node, ir_mode *mode)
{
	lower_binop_logical(node, mode, new_rd_And);
}

static void lower_Or(ir_node *node, ir_mode *mode)
{
	lower_binop_logical(node, mode, new_rd_Or);
}

static void lower_Eor(ir_node *node, ir_mode *mode)
{
	lower_binop_logical(node, mode, new_rd_Eor);
}

/**
 * Translate a Not.
 *
 * Create two logical Nots.
 */
static void lower_Not(ir_node *node, ir_mode *mode)
{
	ir_node               *op       = get_Not_op(node);
	const lower64_entry_t *op_entry = get_node_entry(op);
	dbg_info              *dbgi     = get_irn_dbg_info(node);
	ir_node               *block    = get_nodes_block(node);
	ir_node               *res_low
		= new_rd_Not(dbgi, block, op_entry->low_word, env->low_unsigned);
	ir_node               *res_high
		= new_rd_Not(dbgi, block, op_entry->high_word, mode);
	ir_set_dw_lowered(node, res_low, res_high);
}

static void lower_Proj(ir_node *node, ir_mode *op_mode)
{
	ir_mode *mode = get_irn_mode(node);
	ir_node *pred;
	(void)op_mode;
	if (mode != env->high_signed && mode != env->high_unsigned)
		return;
	/* skip tuples */
	pred = get_Proj_pred(node);
	if (is_Tuple(pred)) {
		long                   pn    = get_Proj_proj(node);
		ir_node               *op    = get_irn_n(pred, pn);
		const lower64_entry_t *entry = get_node_entry(op);
		ir_set_dw_lowered(node, entry->low_word, entry->high_word);
	}
}

static bool is_equality_cmp(const ir_node *node)
{
	ir_relation relation = get_Cmp_relation(node);
	ir_node    *left     = get_Cmp_left(node);
	ir_node    *right    = get_Cmp_right(node);
	ir_mode    *mode     = get_irn_mode(left);

	/* this probably makes no sense if unordered is involved */
	assert(!mode_is_float(mode));

	if (relation == ir_relation_equal || relation == ir_relation_less_greater)
		return true;

	if (!is_Const(right) || !is_Const_null(right))
		return false;
	if (mode_is_signed(mode)) {
		return relation == ir_relation_less_greater;
	} else {
		return relation == ir_relation_greater;
	}
}

static ir_node *get_cfop_destination(const ir_node *cfop)
{
	const ir_edge_t *first = get_irn_out_edge_first(cfop);
	/* we should only have 1 destination */
	assert(get_irn_n_edges(cfop) == 1);
	return get_edge_src_irn(first);
}

static void lower_Switch(ir_node *node, ir_mode *high_mode)
{
	ir_node *selector = get_Switch_selector(node);
	ir_mode *mode     = get_irn_mode(selector);
	(void)high_mode;
	if (mode == env->high_signed || mode == env->high_unsigned) {
		/* we can't really handle Switch with 64bit offsets */
		panic("Switch with 64bit jumptable not supported");
	}
	lower_node(selector);
}

/**
 * Translate a Cond.
 */
static void lower_Cond(ir_node *node, ir_mode *high_mode)
{
	ir_node *left, *right, *block;
	ir_node *sel = get_Cond_selector(node);
	ir_mode *cmp_mode;
	const lower64_entry_t *lentry, *rentry;
	ir_node  *projT = NULL, *projF = NULL;
	ir_node  *new_bl, *irn;
	ir_node  *projHF, *projHT;
	ir_node  *dst_blk;
	ir_relation relation;
	ir_graph *irg;
	dbg_info *dbg;

	(void) high_mode;

	if (!is_Cmp(sel)) {
		lower_node(sel);
		return;
	}

	left     = get_Cmp_left(sel);
	cmp_mode = get_irn_mode(left);
	if (cmp_mode != env->high_signed && cmp_mode != env->high_unsigned) {
		lower_node(sel);
		return;
	}

	right  = get_Cmp_right(sel);
	lower_node(left);
	lower_node(right);
	lentry = get_node_entry(left);
	rentry = get_node_entry(right);

	/* all right, build the code */
	foreach_out_edge_safe(node, edge) {
		ir_node *proj    = get_edge_src_irn(edge);
		long     proj_nr;
		if (!is_Proj(proj))
			continue;
		proj_nr = get_Proj_proj(proj);

		if (proj_nr == pn_Cond_true) {
			assert(projT == NULL && "more than one Proj(true)");
			projT = proj;
		} else {
			assert(proj_nr == pn_Cond_false);
			assert(projF == NULL && "more than one Proj(false)");
			projF = proj;
		}
		mark_irn_visited(proj);
	}
	assert(projT && projF);

	/* create a new high compare */
	block    = get_nodes_block(node);
	irg      = get_Block_irg(block);
	dbg      = get_irn_dbg_info(sel);
	relation = get_Cmp_relation(sel);

	if (is_equality_cmp(sel)) {
		/* x ==/!= y ==> or(x_low^y_low,x_high^y_high) ==/!= 0 */
		ir_mode *mode       = env->low_unsigned;
		ir_node *low_left   = new_rd_Conv(dbg, block, lentry->low_word, mode);
		ir_node *high_left  = new_rd_Conv(dbg, block, lentry->high_word, mode);
		ir_node *low_right  = new_rd_Conv(dbg, block, rentry->low_word, mode);
		ir_node *high_right = new_rd_Conv(dbg, block, rentry->high_word, mode);
		ir_node *xor_low    = new_rd_Eor(dbg, block, low_left, low_right, mode);
		ir_node *xor_high   = new_rd_Eor(dbg, block, high_left, high_right, mode);
		ir_node *ornode = new_rd_Or(dbg, block, xor_low, xor_high, mode);
		ir_node *cmp    = new_rd_Cmp(dbg, block, ornode, new_r_Const(irg, get_mode_null(mode)), relation);
		set_Cond_selector(node, cmp);
		return;
	}

	if (relation == ir_relation_equal) {
		ir_node *proj;
		/* simple case:a == b <==> a_h == b_h && a_l == b_l */
		dst_blk = get_cfop_destination(projF);

		irn = new_rd_Cmp(dbg, block, lentry->high_word, rentry->high_word,
		                 ir_relation_equal);
		dbg = get_irn_dbg_info(node);
		irn = new_rd_Cond(dbg, block, irn);

		projHF = new_r_Proj(irn, mode_X, pn_Cond_false);
		mark_irn_visited(projHF);
		exchange(projF, projHF);

		projHT = new_r_Proj(irn, mode_X, pn_Cond_true);
		mark_irn_visited(projHT);

		new_bl = new_r_Block(irg, 1, &projHT);

		dbg = get_irn_dbg_info(sel);
		irn = new_rd_Cmp(dbg, new_bl, lentry->low_word, rentry->low_word,
		                  ir_relation_equal);
		dbg = get_irn_dbg_info(node);
		irn = new_rd_Cond(dbg, new_bl, irn);

		proj = new_r_Proj(irn, mode_X, pn_Cond_false);
		mark_irn_visited(proj);
		add_block_cf_input(dst_blk, projHF, proj);

		proj = new_r_Proj(irn, mode_X, pn_Cond_true);
		mark_irn_visited(proj);
		exchange(projT, proj);
	} else if (relation == ir_relation_less_greater) {
		ir_node *proj;
		/* simple case:a != b <==> a_h != b_h || a_l != b_l */
		dst_blk = get_cfop_destination(projT);

		irn = new_rd_Cmp(dbg, block, lentry->high_word, rentry->high_word,
		                 ir_relation_less_greater);
		dbg = get_irn_dbg_info(node);
		irn = new_rd_Cond(dbg, block, irn);

		projHT = new_r_Proj(irn, mode_X, pn_Cond_true);
		mark_irn_visited(projHT);
		exchange(projT, projHT);

		projHF = new_r_Proj(irn, mode_X, pn_Cond_false);
		mark_irn_visited(projHF);

		new_bl = new_r_Block(irg, 1, &projHF);

		dbg = get_irn_dbg_info(sel);
		irn = new_rd_Cmp(dbg, new_bl, lentry->low_word, rentry->low_word,
		                 ir_relation_less_greater);
		dbg = get_irn_dbg_info(node);
		irn = new_rd_Cond(dbg, new_bl, irn);

		proj = new_r_Proj(irn, mode_X, pn_Cond_true);
		mark_irn_visited(proj);
		add_block_cf_input(dst_blk, projHT, proj);

		proj = new_r_Proj(irn, mode_X, pn_Cond_false);
		mark_irn_visited(proj);
		exchange(projF, proj);
	} else {
		ir_node *proj;
		/* a rel b <==> a_h REL b_h || (a_h == b_h && a_l rel b_l) */
		ir_node *dstT, *dstF, *newbl_eq, *newbl_l;
		ir_node *projEqF;

		dstT = get_cfop_destination(projT);
		dstF = get_cfop_destination(projF);

		irn = new_rd_Cmp(dbg, block, lentry->high_word, rentry->high_word,
		                 relation & ~ir_relation_equal);
		dbg = get_irn_dbg_info(node);
		irn = new_rd_Cond(dbg, block, irn);

		projHT = new_r_Proj(irn, mode_X, pn_Cond_true);
		mark_irn_visited(projHT);

		projHF = new_r_Proj(irn, mode_X, pn_Cond_false);
		mark_irn_visited(projHF);

		newbl_eq = new_r_Block(irg, 1, &projHF);

		irn = new_rd_Cmp(dbg, block, lentry->high_word, rentry->high_word,
		                 ir_relation_equal);
		irn = new_rd_Cond(dbg, newbl_eq, irn);

		projEqF = new_r_Proj(irn, mode_X, pn_Cond_false);
		mark_irn_visited(projEqF);

		proj = new_r_Proj(irn, mode_X, pn_Cond_true);
		mark_irn_visited(proj);

		newbl_l = new_r_Block(irg, 1, &proj);

		dbg = get_irn_dbg_info(sel);
		irn = new_rd_Cmp(dbg, newbl_l, lentry->low_word, rentry->low_word,
		                 relation);
		dbg = get_irn_dbg_info(node);
		irn = new_rd_Cond(dbg, newbl_l, irn);

		proj = new_r_Proj(irn, mode_X, pn_Cond_true);
		mark_irn_visited(proj);
		add_block_cf_input(dstT, projT, proj);

		proj = new_r_Proj(irn, mode_X, pn_Cond_false);
		mark_irn_visited(proj);
		add_block_cf_input(dstF, projF, proj);

		exchange(projT, projHT);
		exchange(projF, projEqF);
	}

	/* we have changed the control flow */
	env->flags |= CF_CHANGED;
}

/**
 * Translate a Conv to higher_signed
 */
static void lower_Conv_to_Ll(ir_node *node)
{
	ir_mode  *omode        = get_irn_mode(node);
	ir_node  *op           = get_Conv_op(node);
	ir_mode  *imode        = get_irn_mode(op);
	ir_graph *irg          = get_irn_irg(node);
	ir_node  *block        = get_nodes_block(node);
	dbg_info *dbg          = get_irn_dbg_info(node);
	ir_node  *res_low;
	ir_node  *res_high;

	ir_mode  *low_unsigned = env->low_unsigned;
	ir_mode  *low_signed
		= mode_is_signed(omode) ? env->low_signed : low_unsigned;

	if (mode_is_int(imode) || mode_is_reference(imode)) {
		if (imode == env->high_signed || imode == env->high_unsigned) {
			/* a Conv from Lu to Ls or Ls to Lu */
			const lower64_entry_t *op_entry = get_node_entry(op);
			res_low  = op_entry->low_word;
			res_high = new_rd_Conv(dbg, block, op_entry->high_word, low_signed);
		} else {
			/* simple case: create a high word */
			if (imode != low_unsigned)
				op = new_rd_Conv(dbg, block, op, low_unsigned);

			res_low = op;

			if (mode_is_signed(imode)) {
				int      c       = get_mode_size_bits(low_signed) - 1;
				ir_node *cnst    = new_r_Const_long(irg, low_unsigned, c);
				if (get_irn_mode(op) != low_signed)
					op = new_rd_Conv(dbg, block, op, low_signed);
				res_high = new_rd_Shrs(dbg, block, op, cnst, low_signed);
			} else {
				res_high = new_r_Const(irg, get_mode_null(low_signed));
			}
		}
	} else if (imode == mode_b) {
		res_low  = new_rd_Conv(dbg, block, op, low_unsigned);
		res_high = new_r_Const(irg, get_mode_null(low_signed));
	} else {
		ir_node *irn, *call;
		ir_type *mtp = get_conv_type(imode, omode);

		irn = get_intrinsic_address(mtp, get_irn_op(node), imode, omode);
		call = new_rd_Call(dbg, block, get_irg_no_mem(irg), irn, 1, &op, mtp);
		set_irn_pinned(call, get_irn_pinned(node));
		irn = new_r_Proj(call, mode_T, pn_Call_T_result);

		if (env->params->little_endian) {
			res_low  = new_r_Proj(irn, low_unsigned, 0);
			res_high = new_r_Proj(irn, low_signed, 1);
		} else {
			res_low  = new_r_Proj(irn, low_unsigned, 1);
			res_high = new_r_Proj(irn, low_signed,   0);
		}
	}
	ir_set_dw_lowered(node, res_low, res_high);
}

/**
 * Translate a Conv from higher_unsigned
 */
static void lower_Conv_from_Ll(ir_node *node)
{
	ir_node               *op    = get_Conv_op(node);
	ir_mode               *omode = get_irn_mode(node);
	ir_node               *block = get_nodes_block(node);
	dbg_info              *dbg   = get_irn_dbg_info(node);
	ir_graph              *irg   = get_irn_irg(node);
	const lower64_entry_t *entry = get_node_entry(op);

	if (mode_is_int(omode) || mode_is_reference(omode)) {
		op = entry->low_word;

		/* simple case: create a high word */
		if (omode != env->low_unsigned)
			op = new_rd_Conv(dbg, block, op, omode);

		set_Conv_op(node, op);
	} else if (omode == mode_b) {
		/* llu ? true : false  <=> (low|high) ? true : false */
		ir_mode *mode   = env->low_unsigned;
		ir_node *ornode = new_rd_Or(dbg, block, entry->low_word,
		                            entry->high_word, mode);
		set_Conv_op(node, ornode);
	} else {
		ir_node *irn, *call, *in[2];
		ir_mode *imode = get_irn_mode(op);
		ir_type *mtp   = get_conv_type(imode, omode);
		ir_node *res;

		irn   = get_intrinsic_address(mtp, get_irn_op(node), imode, omode);
		if (env->params->little_endian) {
			in[0] = entry->low_word;
			in[1] = entry->high_word;
		} else {
			in[0] = entry->high_word;
			in[1] = entry->low_word;
		}

		call = new_rd_Call(dbg, block, get_irg_no_mem(irg), irn, 2, in, mtp);
		set_irn_pinned(call, get_irn_pinned(node));
		irn = new_r_Proj(call, mode_T, pn_Call_T_result);
		res = new_r_Proj(irn, omode, 0);

		exchange(node, res);
	}
}

/**
 * lower Cmp
 */
static void lower_Cmp(ir_node *cmp, ir_mode *m)
{
	ir_node  *l        = get_Cmp_left(cmp);
	ir_mode  *cmp_mode = get_irn_mode(l);
	ir_node  *r, *low, *high, *t, *res;
	ir_relation relation;
	ir_node  *block;
	dbg_info *dbg;
	const lower64_entry_t *lentry;
	const lower64_entry_t *rentry;
	(void) m;

	if (cmp_mode != env->high_signed && cmp_mode != env->high_unsigned)
		return;

	r        = get_Cmp_right(cmp);
	lentry   = get_node_entry(l);
	rentry   = get_node_entry(r);
	relation = get_Cmp_relation(cmp);
	block    = get_nodes_block(cmp);
	dbg      = get_irn_dbg_info(cmp);

	/* easy case for x ==/!= 0 (see lower_Cond for details) */
	if (is_equality_cmp(cmp)) {
		ir_graph *irg        = get_irn_irg(cmp);
		ir_mode  *mode       = env->low_unsigned;
		ir_node  *low_left   = new_rd_Conv(dbg, block, lentry->low_word, mode);
		ir_node  *high_left  = new_rd_Conv(dbg, block, lentry->high_word, mode);
		ir_node  *low_right  = new_rd_Conv(dbg, block, rentry->low_word, mode);
		ir_node  *high_right = new_rd_Conv(dbg, block, rentry->high_word, mode);
		ir_node  *xor_low    = new_rd_Eor(dbg, block, low_left, low_right, mode);
		ir_node  *xor_high   = new_rd_Eor(dbg, block, high_left, high_right, mode);
		ir_node  *ornode     = new_rd_Or(dbg, block, xor_low, xor_high, mode);
		ir_node  *new_cmp    = new_rd_Cmp(dbg, block, ornode, new_r_Const(irg, get_mode_null(mode)), relation);
		exchange(cmp, new_cmp);
		return;
	}

	if (relation == ir_relation_equal) {
		/* simple case:a == b <==> a_h == b_h && a_l == b_l */
		low  = new_rd_Cmp(dbg, block, lentry->low_word, rentry->low_word,
		                  relation);
		high = new_rd_Cmp(dbg, block, lentry->high_word, rentry->high_word,
		                  relation);
		res  = new_rd_And(dbg, block, low, high, mode_b);
	} else if (relation == ir_relation_less_greater) {
		/* simple case:a != b <==> a_h != b_h || a_l != b_l */
		low  = new_rd_Cmp(dbg, block, lentry->low_word, rentry->low_word,
		                  relation);
		high = new_rd_Cmp(dbg, block, lentry->high_word, rentry->high_word,
		                  relation);
		res = new_rd_Or(dbg, block, low, high, mode_b);
	} else {
		/* a rel b <==> a_h REL b_h || (a_h == b_h && a_l rel b_l) */
		ir_node *high1 = new_rd_Cmp(dbg, block, lentry->high_word,
			rentry->high_word, relation & ~ir_relation_equal);
		low  = new_rd_Cmp(dbg, block, lentry->low_word, rentry->low_word,
		                  relation);
		high = new_rd_Cmp(dbg, block, lentry->high_word, rentry->high_word,
		                  ir_relation_equal);
		t = new_rd_And(dbg, block, low, high, mode_b);
		res = new_rd_Or(dbg, block, high1, t, mode_b);
	}
	exchange(cmp, res);
}

/**
 * Translate a Conv.
 */
static void lower_Conv(ir_node *node, ir_mode *mode)
{
	mode = get_irn_mode(node);

	if (mode == env->high_signed || mode == env->high_unsigned) {
		lower_Conv_to_Ll(node);
	} else {
		ir_mode *op_mode = get_irn_mode(get_Conv_op(node));

		if (op_mode == env->high_signed || op_mode == env->high_unsigned) {
			lower_Conv_from_Ll(node);
		}
	}
}

static void fix_parameter_entities(ir_graph *irg, ir_type *orig_mtp)
{
	size_t      orig_n_params      = get_method_n_params(orig_mtp);
	ir_entity **parameter_entities;

	parameter_entities = ALLOCANZ(ir_entity*, orig_n_params);

	ir_type *frame_type = get_irg_frame_type(irg);
	size_t   n          = get_compound_n_members(frame_type);
	size_t   i;
	size_t   n_param;

	/* collect parameter entities */
	for (i = 0; i < n; ++i) {
		ir_entity *entity = get_compound_member(frame_type, i);
		size_t     p;
		if (!is_parameter_entity(entity))
			continue;
		p = get_entity_parameter_number(entity);
		if (p == IR_VA_START_PARAMETER_NUMBER)
			continue;
		assert(p < orig_n_params);
		assert(parameter_entities[p] == NULL);
		parameter_entities[p] = entity;
	}

	/* adjust indices */
	n_param = 0;
	for (i = 0; i < orig_n_params; ++i, ++n_param) {
		ir_entity *entity = parameter_entities[i];
		ir_type   *tp;

		if (entity != NULL)
			set_entity_parameter_number(entity, n_param);

		tp = get_method_param_type(orig_mtp, i);
		if (is_Primitive_type(tp)) {
			ir_mode *mode = get_type_mode(tp);
			if (mode == env->high_signed || mode == env->high_unsigned) {
				++n_param;
				/* note that we do not change the type of the parameter
				 * entities, as calling convention fixup later still needs to
				 * know which is/was a lowered doubleword.
				 * So we just mark/remember it for later */
				if (entity != NULL) {
					assert(entity->attr.parameter.doubleword_low_mode == NULL);
					entity->attr.parameter.doubleword_low_mode
						= env->low_unsigned;
				}
			}
		}
	}
}

/**
 * Lower the method type.
 *
 * @param env  the lower environment
 * @param mtp  the method type to lower
 *
 * @return the lowered type
 */
static ir_type *lower_mtp(ir_type *mtp)
{
	ir_type *res;
	size_t   i;
	size_t   orig_n_params;
	size_t   orig_n_res;
	size_t   n_param;
	size_t   n_res;
	bool     must_be_lowered;

	res = pmap_get(ir_type, lowered_type, mtp);
	if (res != NULL)
		return res;
	if (type_visited(mtp))
		return mtp;
	mark_type_visited(mtp);

	orig_n_params   = get_method_n_params(mtp);
	orig_n_res      = get_method_n_ress(mtp);
	n_param         = orig_n_params;
	n_res           = orig_n_res;
	must_be_lowered = false;

	/* count new number of params */
	for (i = orig_n_params; i > 0;) {
		ir_type *tp = get_method_param_type(mtp, --i);

		if (is_Primitive_type(tp)) {
			ir_mode *mode = get_type_mode(tp);

			if (mode == env->high_signed || mode == env->high_unsigned) {
				++n_param;
				must_be_lowered = true;
			}
		}
	}

	/* count new number of results */
	for (i = orig_n_res; i > 0;) {
		ir_type *tp = get_method_res_type(mtp, --i);

		if (is_Primitive_type(tp)) {
			ir_mode *mode = get_type_mode(tp);

			if (mode == env->high_signed || mode == env->high_unsigned) {
				++n_res;
				must_be_lowered = true;
			}
		}
	}
	if (!must_be_lowered) {
		set_type_link(mtp, NULL);
		return mtp;
	}

	res = new_d_type_method(n_param, n_res, get_type_dbg_info(mtp));

	/* set param types and result types */
	for (i = n_param = 0; i < orig_n_params; ++i) {
		ir_type *tp = get_method_param_type(mtp, i);

		if (is_Primitive_type(tp)) {
			ir_mode *mode = get_type_mode(tp);

			if (mode == env->high_signed) {
				if (env->params->little_endian) {
					set_method_param_type(res, n_param++, tp_u);
					set_method_param_type(res, n_param++, tp_s);
				} else {
					set_method_param_type(res, n_param++, tp_s);
					set_method_param_type(res, n_param++, tp_u);
				}
			} else if (mode == env->high_unsigned) {
				set_method_param_type(res, n_param++, tp_u);
				set_method_param_type(res, n_param++, tp_u);
			} else {
				set_method_param_type(res, n_param, tp);
				++n_param;
			}
		} else {
			set_method_param_type(res, n_param, tp);
			++n_param;
		}
	}
	for (i = n_res = 0; i < orig_n_res; ++i) {
		ir_type *tp = get_method_res_type(mtp, i);

		if (is_Primitive_type(tp)) {
			ir_mode *mode = get_type_mode(tp);

			if (mode == env->high_signed) {
				if (env->params->little_endian) {
					set_method_res_type(res, n_res++, tp_u);
					set_method_res_type(res, n_res++, tp_s);
				} else {
					set_method_res_type(res, n_res++, tp_s);
					set_method_res_type(res, n_res++, tp_u);
				}
			} else if (mode == env->high_unsigned) {
				set_method_res_type(res, n_res++, tp_u);
				set_method_res_type(res, n_res++, tp_u);
			} else {
				set_method_res_type(res, n_res++, tp);
			}
		} else {
			set_method_res_type(res, n_res++, tp);
		}
	}

	set_method_variadicity(res, get_method_variadicity(mtp));
	set_method_calling_convention(res, get_method_calling_convention(mtp));
	set_method_additional_properties(res, get_method_additional_properties(mtp));

	set_higher_type(res, mtp);
	set_type_link(res, mtp);

	mark_type_visited(res);
	pmap_insert(lowered_type, mtp, res);
	return res;
}

/**
 * Translate a Return.
 */
static void lower_Return(ir_node *node, ir_mode *mode)
{
	ir_graph  *irg = get_irn_irg(node);
	ir_entity *ent = get_irg_entity(irg);
	ir_type   *mtp = get_entity_type(ent);
	ir_node  **in;
	size_t     i, j, n;
	int        need_conv = 0;
	(void) mode;

	/* check if this return must be lowered */
	for (i = 0, n = get_Return_n_ress(node); i < n; ++i) {
		ir_node *pred  = get_Return_res(node, i);
		ir_mode *rmode = get_irn_op_mode(pred);

		if (rmode == env->high_signed || rmode == env->high_unsigned)
			need_conv = 1;
	}
	if (! need_conv)
		return;

	ent = get_irg_entity(irg);
	mtp = get_entity_type(ent);

	/* create a new in array */
	NEW_ARR_A(ir_node *, in, get_method_n_ress(mtp) + 1);
	j = 0;
	in[j++] = get_Return_mem(node);

	for (i = 0, n = get_Return_n_ress(node); i < n; ++i) {
		ir_node *pred      = get_Return_res(node, i);
		ir_mode *pred_mode = get_irn_mode(pred);

		if (pred_mode == env->high_signed || pred_mode == env->high_unsigned) {
			const lower64_entry_t *entry = get_node_entry(pred);
			if (env->params->little_endian) {
				in[j++] = entry->low_word;
				in[j++] = entry->high_word;
			} else {
				in[j++] = entry->high_word;
				in[j++] = entry->low_word;
			}
		} else {
			in[j++] = pred;
		}
	}
	assert(j == get_method_n_ress(mtp)+1);

	set_irn_in(node, j, in);
}

/**
 * Translate the parameters.
 */
static void lower_Start(ir_node *node, ir_mode *high_mode)
{
	ir_graph  *irg      = get_irn_irg(node);
	ir_entity *ent      = get_irg_entity(irg);
	ir_type   *mtp      = get_entity_type(ent);
	ir_type   *orig_mtp = (ir_type*)get_type_link(mtp);
	ir_node   *args;
	long      *new_projs;
	size_t    i, j, n_params;
	(void) high_mode;

	/* if type link is NULL then the type was not lowered, hence no changes
	 * at Start necessary */
	if (orig_mtp == NULL)
		return;

	n_params = get_method_n_params(orig_mtp);

	NEW_ARR_A(long, new_projs, n_params);

	/* Calculate mapping of proj numbers in new_projs */
	for (i = j = 0; i < n_params; ++i, ++j) {
		ir_type *ptp = get_method_param_type(orig_mtp, i);

		new_projs[i] = j;
		if (is_Primitive_type(ptp)) {
			ir_mode *amode = get_type_mode(ptp);
			if (amode == env->high_signed || amode == env->high_unsigned)
				++j;
		}
	}

	/* find args Proj */
	args = NULL;
	foreach_out_edge(node, edge) {
		ir_node *proj = get_edge_src_irn(edge);
		if (!is_Proj(proj))
			continue;
		if (get_Proj_proj(proj) == pn_Start_T_args) {
			args = proj;
			break;
		}
	}
	if (args == NULL)
		return;

	/* fix all Proj's and create new ones */
	foreach_out_edge_safe(args, edge) {
		ir_node *proj   = get_edge_src_irn(edge);
		ir_mode *mode   = get_irn_mode(proj);
		ir_mode *mode_l = env->low_unsigned;
		ir_node *pred;
		long     proj_nr;
		ir_mode *mode_h;
		ir_node *res_low;
		ir_node *res_high;
		int      old_cse;
		dbg_info *dbg;

		if (!is_Proj(proj))
			continue;
		pred    = get_Proj_pred(proj);
		proj_nr = get_Proj_proj(proj);

		if (mode == env->high_signed) {
			mode_h = env->low_signed;
		} else if (mode == env->high_unsigned) {
			mode_h = env->low_unsigned;
		} else {
			long new_pn = new_projs[proj_nr];
			set_Proj_proj(proj, new_pn);
			continue;
		}

		/* Switch off CSE or we might get an already existing Proj. */
		old_cse = get_opt_cse();
		set_opt_cse(0);
		dbg = get_irn_dbg_info(proj);
		if (env->params->little_endian) {
			res_low  = new_rd_Proj(dbg, pred, mode_l, new_projs[proj_nr]);
			res_high = new_rd_Proj(dbg, pred, mode_h, new_projs[proj_nr] + 1);
		} else {
			res_high = new_rd_Proj(dbg, pred, mode_h, new_projs[proj_nr]);
			res_low  = new_rd_Proj(dbg, pred, mode_l, new_projs[proj_nr] + 1);
		}
		set_opt_cse(old_cse);
		ir_set_dw_lowered(proj, res_low, res_high);
	}
}

/**
 * Translate a Call.
 */
static void lower_Call(ir_node *node, ir_mode *mode)
{
	ir_type  *tp = get_Call_type(node);
	ir_node  **in;
	size_t   n_params, n_res;
	bool     need_lower = false;
	size_t   i, j;
	size_t   p;
	long     *res_numbers = NULL;
	ir_node  *resproj;
	(void) mode;

	n_params = get_method_n_params(tp);
	for (p = 0; p < n_params; ++p) {
		ir_type *ptp = get_method_param_type(tp, p);

		if (is_Primitive_type(ptp)) {
			ir_mode *pmode = get_type_mode(ptp);
			if (pmode == env->high_signed || pmode == env->high_unsigned) {
				need_lower = true;
				break;
			}
		}
	}
	n_res = get_method_n_ress(tp);
	if (n_res > 0) {
		NEW_ARR_A(long, res_numbers, n_res);

		for (i = j = 0; i < n_res; ++i, ++j) {
			ir_type *ptp = get_method_res_type(tp, i);

			res_numbers[i] = j;
			if (is_Primitive_type(ptp)) {
				ir_mode *rmode = get_type_mode(ptp);
				if (rmode == env->high_signed || rmode == env->high_unsigned) {
					need_lower = true;
					++j;
				}
			}
		}
	}

	if (! need_lower)
		return;

	/* let's lower it */
	tp = lower_mtp(tp);
	set_Call_type(node, tp);

	NEW_ARR_A(ir_node *, in, get_method_n_params(tp) + 2);

	in[0] = get_Call_mem(node);
	in[1] = get_Call_ptr(node);

	for (j = 2, i = 0; i < n_params; ++i) {
		ir_node *pred      = get_Call_param(node, i);
		ir_mode *pred_mode = get_irn_mode(pred);

		if (pred_mode == env->high_signed || pred_mode == env->high_unsigned) {
			const lower64_entry_t *pred_entry = get_node_entry(pred);
			if (env->params->little_endian) {
				in[j++] = pred_entry->low_word;
				in[j++] = pred_entry->high_word;
			} else {
				in[j++] = pred_entry->high_word;
				in[j++] = pred_entry->low_word;
			}
		} else {
			in[j++] = pred;
		}
	}

	set_irn_in(node, j, in);

	/* find results T */
	resproj = NULL;
	foreach_out_edge(node, edge) {
		ir_node *proj = get_edge_src_irn(edge);
		if (!is_Proj(proj))
			continue;
		if (get_Proj_proj(proj) == pn_Call_T_result) {
			resproj = proj;
			break;
		}
	}
	if (resproj == NULL)
		return;

	/* fix the results */
	foreach_out_edge_safe(resproj, edge) {
		ir_node  *proj      = get_edge_src_irn(edge);
		ir_mode  *proj_mode = get_irn_mode(proj);
		ir_mode  *mode_l    = env->low_unsigned;
		ir_node  *pred;
		long      proj_nr;
		ir_mode  *mode_h;
		ir_node  *res_low;
		ir_node  *res_high;
		dbg_info *dbg;

		if (!is_Proj(proj))
			continue;
		pred    = get_Proj_pred(proj);
		proj_nr = get_Proj_proj(proj);

		if (proj_mode == env->high_signed) {
			mode_h = env->low_signed;
		} else if (proj_mode == env->high_unsigned) {
			mode_h = env->low_unsigned;
		} else {
			long new_nr = res_numbers[proj_nr];
			set_Proj_proj(proj, new_nr);
			continue;
		}

		dbg = get_irn_dbg_info(proj);
		if (env->params->little_endian) {
			res_low  = new_rd_Proj(dbg, pred, mode_l, res_numbers[proj_nr]);
			res_high = new_rd_Proj(dbg, pred, mode_h, res_numbers[proj_nr] + 1);
		} else {
			res_high = new_rd_Proj(dbg, pred, mode_h, res_numbers[proj_nr]);
			res_low  = new_rd_Proj(dbg, pred, mode_l, res_numbers[proj_nr] + 1);
		}
		ir_set_dw_lowered(proj, res_low, res_high);
	}
}

/**
 * Translate an Unknown into two.
 */
static void lower_Unknown(ir_node *node, ir_mode *mode)
{
	ir_mode  *low_mode = env->low_unsigned;
	ir_graph *irg      = get_irn_irg(node);
	ir_node  *res_low  = new_r_Unknown(irg, low_mode);
	ir_node  *res_high = new_r_Unknown(irg, mode);
	ir_set_dw_lowered(node, res_low, res_high);
}

/**
 * Translate a Bad into two.
 */
static void lower_Bad(ir_node *node, ir_mode *mode)
{
	ir_mode  *low_mode = env->low_unsigned;
	ir_graph *irg      = get_irn_irg(node);
	ir_node  *res_low  = new_r_Bad(irg, low_mode);
	ir_node  *res_high = new_r_Bad(irg, mode);
	ir_set_dw_lowered(node, res_low, res_high);
}

/**
 * Translate a Phi.
 *
 * First step: just create two templates
 */
static void lower_Phi(ir_node *phi)
{
	ir_mode  *mode = get_irn_mode(phi);
	int       i;
	int       arity;
	ir_node **in_l;
	ir_node **in_h;
	ir_node  *unk_l;
	ir_node  *unk_h;
	ir_node  *phi_l;
	ir_node  *phi_h;
	dbg_info *dbg;
	ir_node  *block;
	ir_graph *irg;
	ir_mode  *mode_l;
	ir_mode  *mode_h;

	/* enqueue predecessors */
	arity = get_Phi_n_preds(phi);
	for (i = 0; i < arity; ++i) {
		ir_node *pred = get_Phi_pred(phi, i);
		pdeq_putr(env->waitq, pred);
	}

	if (mode != env->high_signed && mode != env->high_unsigned)
		return;

	/* first create a new in array */
	NEW_ARR_A(ir_node *, in_l, arity);
	NEW_ARR_A(ir_node *, in_h, arity);
	irg    = get_irn_irg(phi);
	mode_l = env->low_unsigned;
	mode_h = mode == env->high_signed ? env->low_signed : env->low_unsigned;
	unk_l  = new_r_Dummy(irg, mode_l);
	unk_h  = new_r_Dummy(irg, mode_h);
	for (i = 0; i < arity; ++i) {
		in_l[i] = unk_l;
		in_h[i] = unk_h;
	}

	dbg   = get_irn_dbg_info(phi);
	block = get_nodes_block(phi);
	phi_l = new_rd_Phi(dbg, block, arity, in_l, mode_l);
	phi_h = new_rd_Phi(dbg, block, arity, in_h, mode_h);

	ir_set_dw_lowered(phi, phi_l, phi_h);

	/* remember that we need to fixup the predecessors later */
	ARR_APP1(ir_node*, env->lowered_phis, phi);
}

static void fixup_phi(ir_node *phi)
{
	const lower64_entry_t *entry = get_node_entry(phi);
	ir_node               *phi_l = entry->low_word;
	ir_node               *phi_h = entry->high_word;
	int                    arity = get_Phi_n_preds(phi);
	int                    i;

	/* exchange phi predecessors which are lowered by now */
	for (i = 0; i < arity; ++i) {
		ir_node               *pred       = get_Phi_pred(phi, i);
		const lower64_entry_t *pred_entry = get_node_entry(pred);

		set_Phi_pred(phi_l, i, pred_entry->low_word);
		set_Phi_pred(phi_h, i, pred_entry->high_word);
	}
}

/**
 * Translate a Mux.
 */
static void lower_Mux(ir_node *mux, ir_mode *mode)
{
	ir_node               *truen       = get_Mux_true(mux);
	ir_node               *falsen      = get_Mux_false(mux);
	ir_node               *sel         = get_Mux_sel(mux);
	const lower64_entry_t *true_entry  = get_node_entry(truen);
	const lower64_entry_t *false_entry = get_node_entry(falsen);
	ir_node               *true_l      = true_entry->low_word;
	ir_node               *true_h      = true_entry->high_word;
	ir_node               *false_l     = false_entry->low_word;
	ir_node               *false_h     = false_entry->high_word;
	dbg_info              *dbgi        = get_irn_dbg_info(mux);
	ir_node               *block       = get_nodes_block(mux);
	ir_node               *res_low
		= new_rd_Mux(dbgi, block, sel, false_l, true_l, env->low_unsigned);
	ir_node               *res_high
		= new_rd_Mux(dbgi, block, sel, false_h, true_h, mode);
	ir_set_dw_lowered(mux, res_low, res_high);
}

/**
 * Translate an ASM node.
 */
static void lower_ASM(ir_node *asmn, ir_mode *mode)
{
	ir_mode           *high_signed        = env->high_signed;
	ir_mode           *high_unsigned      = env->high_unsigned;
	int                n_outs             = get_ASM_n_output_constraints(asmn);
	ir_asm_constraint *output_constraints = get_ASM_output_constraints(asmn);
	ir_asm_constraint *input_constraints  = get_ASM_input_constraints(asmn);
	unsigned           n_64bit_outs       = 0;

	(void)mode;

	for (int i = get_irn_arity(asmn) - 1; i >= 0; --i) {
		ir_node *op      = get_irn_n(asmn, i);
		ir_mode *op_mode = get_irn_mode(op);
		if (op_mode == high_signed || op_mode == high_unsigned) {
			panic("lowering ASM 64bit input unimplemented");
		}
	}

	for (int o = 0; o < n_outs; ++o) {
		const ir_asm_constraint *constraint = &output_constraints[o];
		if (constraint->mode == high_signed || constraint->mode == high_unsigned) {
			const char *constr = get_id_str(constraint->constraint);
			++n_64bit_outs;
			/* TODO: How to do this architecture neutral? This is very
			 * i386 specific... */
			if (constr[0] != '=' || constr[1] != 'A') {
				panic("lowering ASM 64bit output only supports '=A' currently");
			}
		}
	}

	if (n_64bit_outs == 0)
		return;

	dbg_info          *dbgi       = get_irn_dbg_info(asmn);
	ir_node           *block      = get_nodes_block(asmn);
	ir_node           *mem        = get_ASM_mem(asmn);
	int                new_n_outs = 0;
	int                n_clobber  = get_ASM_n_clobbers(asmn);
	long              *proj_map   = ALLOCAN(long, n_outs);
	ident            **clobbers   = get_ASM_clobbers(asmn);
	ident             *asm_text   = get_ASM_text(asmn);
	ir_asm_constraint *new_outputs
		= ALLOCAN(ir_asm_constraint, n_outs+n_64bit_outs);
	ir_node           *new_asm;

	for (int o = 0; o < n_outs; ++o) {
		const ir_asm_constraint *constraint = &output_constraints[o];
		if (constraint->mode == high_signed || constraint->mode == high_unsigned) {
			new_outputs[new_n_outs].pos        = constraint->pos;
			new_outputs[new_n_outs].constraint = new_id_from_str("=a");
			new_outputs[new_n_outs].mode       = env->low_unsigned;
			proj_map[o] = new_n_outs;
			++new_n_outs;
			new_outputs[new_n_outs].pos        = constraint->pos;
			new_outputs[new_n_outs].constraint = new_id_from_str("=d");
			if (constraint->mode == high_signed)
				new_outputs[new_n_outs].mode = env->low_signed;
			else
				new_outputs[new_n_outs].mode = env->low_unsigned;
			++new_n_outs;
		} else {
			new_outputs[new_n_outs] = *constraint;
			proj_map[o] = new_n_outs;
			++new_n_outs;
		}
	}
	assert(new_n_outs == n_outs+(int)n_64bit_outs);

	int       n_inputs = get_ASM_n_inputs(asmn);
	ir_node **new_ins  = ALLOCAN(ir_node*, n_inputs);
	for (int i = 0; i < n_inputs; ++i)
		new_ins[i] = get_ASM_input(asmn, i);

	new_asm = new_rd_ASM(dbgi, block, mem, n_inputs, new_ins, input_constraints,
						 new_n_outs, new_outputs, n_clobber, clobbers,
						 asm_text);

	foreach_out_edge_safe(asmn, edge) {
		ir_node *proj      = get_edge_src_irn(edge);
		ir_mode *proj_mode = get_irn_mode(proj);
		long     pn;

		if (!is_Proj(proj))
			continue;
		pn = get_Proj_proj(proj);

		if (pn < n_outs)
			pn = proj_map[pn];
		else
			pn = new_n_outs + pn - n_outs;

		if (proj_mode == high_signed || proj_mode == high_unsigned) {
			ir_mode *high_mode
				= proj_mode == high_signed ? env->low_signed : env->low_unsigned;
			ir_node *np_low  = new_r_Proj(new_asm, env->low_unsigned, pn);
			ir_node *np_high = new_r_Proj(new_asm, high_mode, pn+1);
			ir_set_dw_lowered(proj, np_low, np_high);
		} else {
			ir_node *np = new_r_Proj(new_asm, proj_mode, pn);
			exchange(proj, np);
		}
	}
}

/**
 * Lower the builtin type to its higher part.
 *
 * @param mtp  the builtin type to lower
 *
 * @return the lowered type
 */
static ir_type *lower_Builtin_type_high(ir_type *mtp)
{
	ir_type *res;
	size_t   i;
	size_t   n_params;
	size_t   n_results;
	bool     must_be_lowered;

	res = pmap_get(ir_type, lowered_builtin_type_high, mtp);
	if (res != NULL)
		return res;

	n_params        = get_method_n_params(mtp);
	n_results       = get_method_n_ress(mtp);
	must_be_lowered = false;

	/* check for double word parameter */
	for (i = n_params; i > 0;) {
		ir_type *tp = get_method_param_type(mtp, --i);

		if (is_Primitive_type(tp)) {
			ir_mode *mode = get_type_mode(tp);

			if (mode == env->high_signed || mode == env->high_unsigned) {
				must_be_lowered = true;
				break;
			}
		}
	}

	if (!must_be_lowered) {
		set_type_link(mtp, NULL);
		return mtp;
	}

	res = new_d_type_method(n_params, n_results, get_type_dbg_info(mtp));

	/* set param types and result types */
	for (i = 0; i < n_params; ++i) {
		ir_type *tp = get_method_param_type(mtp, i);

		if (is_Primitive_type(tp)) {
			ir_mode *mode = get_type_mode(tp);

			if (mode == env->high_signed) {
				if (env->params->little_endian) {
					set_method_param_type(res, i, tp_u);
				} else {
					set_method_param_type(res, i, tp_s);
				}
			} else if (mode == env->high_unsigned) {
				set_method_param_type(res, i, tp_u);
			} else {
				set_method_param_type(res, i, tp);
			}
		} else {
			set_method_param_type(res, i, tp);
		}
	}
	for (i = n_results = 0; i < n_results; ++i) {
		ir_type *tp = get_method_res_type(mtp, i);

		if (is_Primitive_type(tp)) {
			ir_mode *mode = get_type_mode(tp);

			if (mode == env->high_signed) {
				if (env->params->little_endian) {
					set_method_res_type(res, i, tp_u);
				} else {
					set_method_res_type(res, i, tp_s);
				}
			} else if (mode == env->high_unsigned) {
				set_method_res_type(res, i, tp_u);
			} else {
				set_method_res_type(res, i, tp);
			}
		} else {
			set_method_res_type(res, i, tp);
		}
	}

	set_method_variadicity(res, get_method_variadicity(mtp));
	set_method_calling_convention(res, get_method_calling_convention(mtp));
	set_method_additional_properties(res, get_method_additional_properties(mtp));

	pmap_insert(lowered_builtin_type_high, mtp, res);
	return res;
}

/**
 * Lower the builtin type to its lower part.
 *
 * @param mtp  the builtin type to lower
 *
 * @return the lowered type
 */
static ir_type *lower_Builtin_type_low(ir_type *mtp)
{
	ir_type *res;
	size_t   i;
	size_t   n_params;
	size_t   n_results;
	bool     must_be_lowered;

	res = pmap_get(ir_type, lowered_builtin_type_low, mtp);
	if (res != NULL)
		return res;

	n_params        = get_method_n_params(mtp);
	n_results       = get_method_n_ress(mtp);
	must_be_lowered = false;

	/* check for double word parameter */
	for (i = n_params; i > 0;) {
		ir_type *tp = get_method_param_type(mtp, --i);

		if (is_Primitive_type(tp)) {
			ir_mode *mode = get_type_mode(tp);

			if (mode == env->high_signed || mode == env->high_unsigned) {
				must_be_lowered = true;
				break;
			}
		}
	}

	if (!must_be_lowered) {
		set_type_link(mtp, NULL);
		return mtp;
	}

	res = new_d_type_method(n_params, n_results, get_type_dbg_info(mtp));

	/* set param types and result types */
	for (i = 0; i < n_params; ++i) {
		ir_type *tp = get_method_param_type(mtp, i);

		if (is_Primitive_type(tp)) {
			ir_mode *mode = get_type_mode(tp);

			if (mode == env->high_signed) {
				if (env->params->little_endian) {
					set_method_param_type(res, i, tp_s);
				} else {
					set_method_param_type(res, i, tp_u);
				}
			} else if (mode == env->high_unsigned) {
				set_method_param_type(res, i, tp_u);
			} else {
				set_method_param_type(res, i, tp);
			}
		} else {
			set_method_param_type(res, i, tp);
		}
	}
	for (i = 0; i < n_results; ++i) {
		ir_type *tp = get_method_res_type(mtp, i);

		if (is_Primitive_type(tp)) {
			ir_mode *mode = get_type_mode(tp);

			if (mode == env->high_signed) {
				if (env->params->little_endian) {
					set_method_res_type(res, i, tp_s);
				} else {
					set_method_res_type(res, i, tp_u);
				}
			} else if (mode == env->high_unsigned) {
				set_method_res_type(res, i, tp_u);
			} else {
				set_method_res_type(res, i, tp);
			}
		} else {
			set_method_res_type(res, i, tp);
		}
	}

	set_method_variadicity(res, get_method_variadicity(mtp));
	set_method_calling_convention(res, get_method_calling_convention(mtp));
	set_method_additional_properties(res, get_method_additional_properties(mtp));

	pmap_insert(lowered_builtin_type_low, mtp, res);
	return res;
}

/**
 * lowers a builtin which reduces a 64bit value to a simple summary value
 * (popcount, ffs, ...)
 */
static void lower_reduce_builtin(ir_node *builtin, ir_mode *mode)
{
	ir_builtin_kind  kind         = get_Builtin_kind(builtin);
	ir_node         *operand      = get_Builtin_param(builtin, 0);
	ir_mode         *operand_mode = get_irn_mode(operand);
	if (operand_mode != env->high_signed && operand_mode != env->high_unsigned)
		return;

	{
	arch_allow_ifconv_func  allow_ifconv      = be_get_backend_param()->allow_ifconv;
	int                     arity             = get_irn_arity(builtin);
	dbg_info               *dbgi              = get_irn_dbg_info(builtin);
	ir_graph               *irg               = get_irn_irg(builtin);
	ir_type                *type              = get_Builtin_type(builtin);
	ir_type                *lowered_type_high = lower_Builtin_type_high(type);
	ir_type                *lowered_type_low  = lower_Builtin_type_low(type);
	ir_type                *result_type       = get_method_res_type(lowered_type_low, 0);
	ir_mode                *result_mode       = get_type_mode(result_type);
	ir_node                *block             = get_nodes_block(builtin);
	ir_node                *mem               = get_Builtin_mem(builtin);
	const lower64_entry_t  *entry             = get_node_entry(operand);
	ir_mode                *high_mode         = get_irn_mode(entry->high_word);
	ir_node                *in_high[1]        = {entry->high_word};
	ir_node                *in_low[1]         = {entry->low_word};
	ir_node                *res;

	assert(is_NoMem(mem));
	assert(arity == 2);

	switch (kind) {
	case ir_bk_ffs: {
		ir_node               *number_of_bits = new_r_Const_long(irg, result_mode, get_mode_size_bits(env->low_unsigned));
		ir_node               *zero_high      = new_rd_Const(dbgi, irg, get_mode_null(high_mode));
		ir_node               *zero_unsigned  = new_rd_Const(dbgi, irg, get_mode_null(env->low_unsigned));
		ir_node               *zero_result    = new_rd_Const(dbgi, irg, get_mode_null(result_mode));
		ir_node               *cmp_low        = new_rd_Cmp(dbgi, block, entry->low_word, zero_unsigned, ir_relation_equal);
		ir_node               *cmp_high       = new_rd_Cmp(dbgi, block, entry->high_word, zero_high, ir_relation_equal);
		ir_node               *ffs_high       = new_rd_Builtin(dbgi, block, mem, 1, in_high, kind, lowered_type_high);
		ir_node               *high_proj      = new_r_Proj(ffs_high, result_mode, pn_Builtin_max+1);
		ir_node               *high           = new_rd_Add(dbgi, block, high_proj, number_of_bits, result_mode);
		ir_node               *ffs_low        = new_rd_Builtin(dbgi, block, mem, 1, in_low, kind, lowered_type_low);
		ir_node               *low            = new_r_Proj(ffs_low, result_mode, pn_Builtin_max+1);
		ir_node               *mux_high       = new_rd_Mux(dbgi, block, cmp_high, high, zero_result, result_mode);

		if (! allow_ifconv(cmp_high, high, zero_result))
			ir_nodeset_insert(&created_mux_nodes, mux_high);

		res = new_rd_Mux(dbgi, block, cmp_low, low, mux_high, result_mode);

		if (! allow_ifconv(cmp_low, low, mux_high))
			ir_nodeset_insert(&created_mux_nodes, res);
		break;
	}
	case ir_bk_clz: {
		ir_node               *zero           = new_rd_Const(dbgi, irg, get_mode_null(high_mode));
		ir_node               *cmp_high       = new_rd_Cmp(dbgi, block, entry->high_word, zero, ir_relation_equal);
		ir_node               *clz_high       = new_rd_Builtin(dbgi, block, mem, 1, in_high, kind, lowered_type_high);
		ir_node               *high           = new_r_Proj(clz_high, result_mode, pn_Builtin_max+1);
		ir_node               *clz_low        = new_rd_Builtin(dbgi, block, mem, 1, in_low, kind, lowered_type_low);
		ir_node               *low_proj       = new_r_Proj(clz_low, result_mode, pn_Builtin_max+1);
		ir_node               *number_of_bits = new_r_Const_long(irg, result_mode, get_mode_size_bits(mode));
		ir_node               *low            = new_rd_Add(dbgi, block, low_proj, number_of_bits, result_mode);

		res = new_rd_Mux(dbgi, block, cmp_high, high, low, result_mode);

		if (! allow_ifconv(cmp_high, high, low))
			ir_nodeset_insert(&created_mux_nodes, res);
		break;
	}
	case ir_bk_ctz: {
		ir_node               *zero_unsigned  = new_rd_Const(dbgi, irg, get_mode_null(env->low_unsigned));
		ir_node               *cmp_low        = new_rd_Cmp(dbgi, block, entry->low_word, zero_unsigned, ir_relation_equal);
		ir_node               *ffs_high       = new_rd_Builtin(dbgi, block, mem, 1, in_high, kind, lowered_type_high);
		ir_node               *high_proj      = new_r_Proj(ffs_high, result_mode, pn_Builtin_max+1);
		ir_node               *number_of_bits = new_r_Const_long(irg, result_mode, get_mode_size_bits(env->low_unsigned));
		ir_node               *high           = new_rd_Add(dbgi, block, high_proj, number_of_bits, result_mode);
		ir_node               *ffs_low        = new_rd_Builtin(dbgi, block, mem, 1, in_low, kind, lowered_type_low);
		ir_node               *low            = new_r_Proj(ffs_low, result_mode, pn_Builtin_max+1);

		res = new_rd_Mux(dbgi, block, cmp_low, low, high, result_mode);

		if (! allow_ifconv(cmp_low, low, high))
			ir_nodeset_insert(&created_mux_nodes, res);
		break;
	}
	case ir_bk_popcount: {
		ir_node               *popcount_high = new_rd_Builtin(dbgi, block, mem, 1, in_high, kind, lowered_type_high);
		ir_node               *popcount_low  = new_rd_Builtin(dbgi, block, mem, 1, in_low, kind, lowered_type_low);
		ir_node               *high          = new_r_Proj(popcount_high, result_mode, pn_Builtin_max+1);
		ir_node               *low           = new_r_Proj(popcount_low, result_mode, pn_Builtin_max+1);

		res = new_rd_Add(dbgi, block, high, low, result_mode);
		break;
	}
	case ir_bk_parity: {
		ir_node  *parity_high;
		ir_node  *parity_low;
		ir_node  *high;
		ir_node  *low;

		assert(arity == 2);

		parity_high = new_rd_Builtin(dbgi, block, mem, 1, in_high, kind, lowered_type_high);
		high        = new_r_Proj(parity_high, result_mode, pn_Builtin_max+1);
		parity_low  = new_rd_Builtin(dbgi, block, mem, 1, in_low, kind, lowered_type_low);
		low         = new_r_Proj(parity_low, result_mode, pn_Builtin_max+1);
		res         = new_rd_Eor(dbgi, block, high, low, result_mode);
		break;
	}
	default:
		panic("unexpected builtin");
	}

	turn_into_tuple(builtin, 2);
	set_irn_n(builtin, pn_Builtin_M, mem);
	set_irn_n(builtin, pn_Builtin_max+1, res);
	}
}

/**
 * lowers builtins performing arithmetic (bswap)
 */
static void lower_arithmetic_builtin(ir_node *builtin, ir_mode *mode)
{
	ir_builtin_kind  kind         = get_Builtin_kind(builtin);
	ir_node         *operand      = get_Builtin_param(builtin, 0);
	ir_mode         *operand_mode = get_irn_mode(operand);
	(void) mode;
	if (operand_mode != env->high_signed && operand_mode != env->high_unsigned)
		return;

	{
	dbg_info              *dbgi              = get_irn_dbg_info(builtin);
	ir_type               *type              = get_Builtin_type(builtin);
	ir_type               *lowered_type_high = lower_Builtin_type_high(type);
	ir_type               *lowered_type_low  = lower_Builtin_type_low(type);
	ir_node               *block             = get_nodes_block(builtin);
	ir_node               *mem               = get_Builtin_mem(builtin);
	const lower64_entry_t *entry             = get_node_entry(operand);
	ir_mode               *mode_high         = get_irn_mode(entry->high_word);
	ir_node               *res_high;
	ir_node               *res_low;

	switch (kind) {
	case ir_bk_bswap: {
		ir_node               *in_high[1] = { entry->high_word };
		ir_node               *in_low[1]  = { entry->low_word };
		ir_node               *swap_high  = new_rd_Builtin(dbgi, block, mem, 1, in_high, kind, lowered_type_high);
		ir_node               *swap_low   = new_rd_Builtin(dbgi, block, mem, 1, in_low, kind, lowered_type_low);
		ir_node               *high       = new_r_Proj(swap_high, mode_high, pn_Builtin_max+1);
		ir_node               *low        = new_r_Proj(swap_low, env->low_unsigned, pn_Builtin_max+1);
		if (mode_high == env->low_signed) {
			res_high = new_rd_Conv(dbgi, block, low, env->low_signed);
			res_low  = new_rd_Conv(dbgi, block, high, env->low_unsigned);
		} else {
			res_high = low;
			res_low  = high;
		}
		break;
	}
	default:
		panic("unexpected builtin");
	}

	/* search result Proj */
	foreach_out_edge_safe(builtin, edge) {
		ir_node *proj = get_edge_src_irn(edge);
		if (!is_Proj(proj))
			continue;

		if (get_Proj_proj(proj) == pn_Builtin_max+1) {
			ir_set_dw_lowered(proj, res_low, res_high);
		}
	}
	}
}

/**
 * Lower double word builtins.
 */
static void lower_Builtin(ir_node *builtin, ir_mode *mode)
{
	ir_builtin_kind kind = get_Builtin_kind(builtin);

	switch (kind) {
	case ir_bk_trap:
	case ir_bk_debugbreak:
	case ir_bk_return_address:
	case ir_bk_frame_address:
	case ir_bk_prefetch:
	case ir_bk_inport:
	case ir_bk_outport:
	case ir_bk_inner_trampoline:
		/* Nothing to do. */
		return;
	case ir_bk_bswap:
		lower_arithmetic_builtin(builtin, mode);
		return;
	case ir_bk_ffs:
	case ir_bk_clz:
	case ir_bk_ctz:
	case ir_bk_popcount:
	case ir_bk_parity:
		lower_reduce_builtin(builtin, mode);
		return;
	}
	panic("unknown builtin");
}

/**
 * check for opcodes that must always be lowered.
 */
static bool always_lower(unsigned code)
{
	switch (code) {
	case iro_ASM:
	case iro_Builtin:
	case iro_Proj:
	case iro_Start:
	case iro_Call:
	case iro_Return:
	case iro_Cond:
	case iro_Conv:
	case iro_Sel:
		return true;
	default:
		return false;
	}
}

/**
 * Compare two op_mode_entry_t's.
 */
static int cmp_op_mode(const void *elt, const void *key, size_t size)
{
	const op_mode_entry_t *e1 = (const op_mode_entry_t*)elt;
	const op_mode_entry_t *e2 = (const op_mode_entry_t*)key;
	(void) size;

	return (e1->op != e2->op) | (e1->imode != e2->imode) | (e1->omode != e2->omode);
}

/**
 * Compare two conv_tp_entry_t's.
 */
static int cmp_conv_tp(const void *elt, const void *key, size_t size)
{
	const conv_tp_entry_t *e1 = (const conv_tp_entry_t*)elt;
	const conv_tp_entry_t *e2 = (const conv_tp_entry_t*)key;
	(void) size;

	return (e1->imode != e2->imode) | (e1->omode != e2->omode);
}

/**
 * Enter a lowering function into an ir_op.
 */
void ir_register_dw_lower_function(ir_op *op, lower_dw_func func)
{
	op->ops.generic = (op_func)func;
}

/* Determine which modes need to be lowered */
static void setup_modes(void)
{
	unsigned           size_bits           = env->params->doubleword_size;
	ir_mode           *doubleword_signed   = NULL;
	ir_mode           *doubleword_unsigned = NULL;
	size_t             n_modes             = ir_get_n_modes();
	ir_mode_arithmetic arithmetic;
	unsigned           modulo_shift;
	size_t             i;

	/* search for doubleword modes... */
	for (i = 0; i < n_modes; ++i) {
		ir_mode *mode = ir_get_mode(i);
		if (!mode_is_int(mode))
			continue;
		if (get_mode_size_bits(mode) != size_bits)
			continue;
		if (mode_is_signed(mode)) {
			if (doubleword_signed != NULL) {
				/* sigh - the lowerer should really just lower all mode with
				 * size_bits it finds. Unfortunately this required a bigger
				 * rewrite. */
				panic("multiple double word signed modes found");
			}
			doubleword_signed = mode;
		} else {
			if (doubleword_unsigned != NULL) {
				/* sigh - the lowerer should really just lower all mode with
				 * size_bits it finds. Unfortunately this required a bigger
				 * rewrite. */
				panic("multiple double word unsigned modes found");
			}
			doubleword_unsigned = mode;
		}
	}
	if (doubleword_signed == NULL || doubleword_unsigned == NULL) {
		panic("Couldn't find doubleword modes");
	}

	arithmetic   = get_mode_arithmetic(doubleword_signed);
	modulo_shift = get_mode_modulo_shift(doubleword_signed);

	assert(get_mode_size_bits(doubleword_unsigned) == size_bits);
	assert(size_bits % 2 == 0);
	assert(get_mode_sign(doubleword_signed) == 1);
	assert(get_mode_sign(doubleword_unsigned) == 0);
	assert(get_mode_sort(doubleword_signed) == irms_int_number);
	assert(get_mode_sort(doubleword_unsigned) == irms_int_number);
	assert(get_mode_arithmetic(doubleword_unsigned) == arithmetic);
	assert(get_mode_modulo_shift(doubleword_unsigned) == modulo_shift);

	/* try to guess a sensible modulo shift for the new mode.
	 * (This is IMO another indication that this should really be a node
	 *  attribute instead of a mode thing) */
	if (modulo_shift == size_bits) {
		modulo_shift = modulo_shift / 2;
	} else if (modulo_shift == 0) {
		/* fine */
	} else {
		panic("Don't know what new modulo shift to use for lowered doubleword mode");
	}
	size_bits /= 2;

	/* produce lowered modes */
	env->high_signed   = doubleword_signed;
	env->high_unsigned = doubleword_unsigned;
	env->low_signed    = new_int_mode("WS", arithmetic, size_bits, 1,
	                                  modulo_shift);
	env->low_unsigned  = new_int_mode("WU", arithmetic, size_bits, 0,
	                                  modulo_shift);
}

static void enqueue_preds(ir_node *node)
{
	int arity = get_irn_arity(node);
	int i;

	for (i = 0; i < arity; ++i) {
		ir_node *pred = get_irn_n(node, i);
		pdeq_putr(env->waitq, pred);
	}
}

static void lower_node(ir_node *node)
{
	int              arity;
	int              i;
	lower_dw_func    func;
	ir_op           *op;
	ir_mode         *mode;
	unsigned         idx;
	lower64_entry_t *entry;

	if (irn_visited_else_mark(node))
		return;

	/* cycles are always broken at Phi and Block nodes. So we don't need special
	 * magic in all the other lower functions */
	if (is_Block(node)) {
		enqueue_preds(node);
		return;
	} else if (is_Phi(node)) {
		lower_Phi(node);
		return;
	}

	/* depth-first: descend into operands */
	if (!is_Block(node)) {
		ir_node *block = get_nodes_block(node);
		lower_node(block);
	}

	if (!is_Cond(node)) {
		arity = get_irn_arity(node);
		for (i = 0; i < arity; ++i) {
			ir_node *pred = get_irn_n(node, i);
			lower_node(pred);
		}
	}

	op   = get_irn_op(node);
	func = (lower_dw_func) op->ops.generic;
	if (func == NULL)
		return;

	idx   = get_irn_idx(node);
	entry = idx < env->n_entries ? env->entries[idx] : NULL;
	if (entry != NULL || always_lower(get_irn_opcode(node))) {
		mode = get_irn_op_mode(node);
		if (mode == env->high_signed) {
			mode = env->low_signed;
		} else {
			mode = env->low_unsigned;
		}
		DB((dbg, LEVEL_1, "  %+F\n", node));
		func(node, mode);
	}
}

static void clear_node_and_phi_links(ir_node *node, void *data)
{
	(void) data;
	if (get_irn_mode(node) == mode_T) {
		set_irn_link(node, node);
	} else {
		set_irn_link(node, NULL);
	}
	if (is_Block(node))
		set_Block_phis(node, NULL);
	else if (is_Phi(node))
		set_Phi_next(node, NULL);
}

static void lower_irg(ir_graph *irg)
{
	ir_entity *ent;
	ir_type   *mtp;
	ir_type   *lowered_mtp;
	unsigned   n_idx;

	obstack_init(&env->obst);

	/* just here for debugging */
	current_ir_graph = irg;
	assure_edges(irg);

	n_idx = get_irg_last_idx(irg);
	n_idx = n_idx + (n_idx >> 2);  /* add 25% */
	env->n_entries = n_idx;
	env->entries   = NEW_ARR_F(lower64_entry_t*, n_idx);
	memset(env->entries, 0, sizeof(env->entries[0]) * n_idx);

	env->irg            = irg;
	env->flags          = 0;

	ent = get_irg_entity(irg);
	mtp = get_entity_type(ent);
	lowered_mtp = lower_mtp(mtp);

	if (lowered_mtp != mtp) {
		set_entity_type(ent, lowered_mtp);
		env->flags |= MUST_BE_LOWERED;

		fix_parameter_entities(irg, mtp);
	}

	/* first step: link all nodes and allocate data */
	ir_reserve_resources(irg, IR_RESOURCE_PHI_LIST | IR_RESOURCE_IRN_LINK);
	visit_all_identities(irg, clear_node_and_phi_links, NULL);
	irg_walk_graph(irg, NULL, prepare_links_and_handle_rotl, env);

	if (env->flags & MUST_BE_LOWERED) {
		size_t i;
		ir_reserve_resources(irg, IR_RESOURCE_IRN_VISITED);
		inc_irg_visited(irg);

		assert(pdeq_empty(env->waitq));
		pdeq_putr(env->waitq, get_irg_end(irg));

		env->lowered_phis = NEW_ARR_F(ir_node*, 0);
		while (!pdeq_empty(env->waitq)) {
			ir_node *node = (ir_node*)pdeq_getl(env->waitq);
			lower_node(node);
		}

		/* we need to fixup phis */
		for (i = 0; i < ARR_LEN(env->lowered_phis); ++i) {
			ir_node *phi = env->lowered_phis[i];
			fixup_phi(phi);
		}
		DEL_ARR_F(env->lowered_phis);


		ir_free_resources(irg, IR_RESOURCE_IRN_VISITED);

		if (env->flags & CF_CHANGED) {
			/* control flow changed, dominance info is invalid */
			clear_irg_properties(irg, IR_GRAPH_PROPERTY_CONSISTENT_DOMINANCE);
		}
		edges_deactivate(irg);
	}

	ir_free_resources(irg, IR_RESOURCE_PHI_LIST | IR_RESOURCE_IRN_LINK);

	DEL_ARR_F(env->entries);
	obstack_free(&env->obst, NULL);
}

static const lwrdw_param_t *param;

void ir_prepare_dw_lowering(const lwrdw_param_t *new_param)
{
	assert(new_param != NULL);
	FIRM_DBG_REGISTER(dbg, "firm.lower.dw");

	param = new_param;

	ir_clear_opcodes_generic_func();
	ir_register_dw_lower_function(op_ASM,     lower_ASM);
	ir_register_dw_lower_function(op_Add,     lower_binop);
	ir_register_dw_lower_function(op_And,     lower_And);
	ir_register_dw_lower_function(op_Bad,     lower_Bad);
	ir_register_dw_lower_function(op_Builtin, lower_Builtin);
	ir_register_dw_lower_function(op_Call,    lower_Call);
	ir_register_dw_lower_function(op_Cmp,     lower_Cmp);
	ir_register_dw_lower_function(op_Cond,    lower_Cond);
	ir_register_dw_lower_function(op_Const,   lower_Const);
	ir_register_dw_lower_function(op_Conv,    lower_Conv);
	ir_register_dw_lower_function(op_Div,     lower_Div);
	ir_register_dw_lower_function(op_Eor,     lower_Eor);
	ir_register_dw_lower_function(op_Load,    lower_Load);
	ir_register_dw_lower_function(op_Minus,   lower_unop);
	ir_register_dw_lower_function(op_Mod,     lower_Mod);
	ir_register_dw_lower_function(op_Mul,     lower_binop);
	ir_register_dw_lower_function(op_Mux,     lower_Mux);
	ir_register_dw_lower_function(op_Not,     lower_Not);
	ir_register_dw_lower_function(op_Or,      lower_Or);
	ir_register_dw_lower_function(op_Proj,    lower_Proj);
	ir_register_dw_lower_function(op_Return,  lower_Return);
	ir_register_dw_lower_function(op_Shl,     lower_Shl);
	ir_register_dw_lower_function(op_Shr,     lower_Shr);
	ir_register_dw_lower_function(op_Shrs,    lower_Shrs);
	ir_register_dw_lower_function(op_Start,   lower_Start);
	ir_register_dw_lower_function(op_Store,   lower_Store);
	ir_register_dw_lower_function(op_Sub,     lower_binop);
	ir_register_dw_lower_function(op_Switch,  lower_Switch);
	ir_register_dw_lower_function(op_Unknown, lower_Unknown);
}

/**
 * Callback to lower only the Mux nodes we created.
 */
static int lower_mux_cb(ir_node *mux)
{
	return ir_nodeset_contains(&created_mux_nodes, mux);
}

/*
 * Do the lowering.
 */
void ir_lower_dw_ops(void)
{
	lower_dw_env_t lenv;
	size_t      i, n;

	memset(&lenv, 0, sizeof(lenv));
	lenv.params = param;
	env = &lenv;

	setup_modes();

	/* create the necessary maps */
	if (! intrinsic_fkt)
		intrinsic_fkt = new_set(cmp_op_mode, iro_Last + 1);
	if (! conv_types)
		conv_types = new_set(cmp_conv_tp, 16);
	if (! lowered_type)
		lowered_type = pmap_create();
	if (! lowered_builtin_type_low)
		lowered_builtin_type_low = pmap_create();
	if (! lowered_builtin_type_high)
		lowered_builtin_type_high = pmap_create();

	/* create a primitive unsigned and signed type */
	if (! tp_u)
		tp_u = get_type_for_mode(lenv.low_unsigned);
	if (! tp_s)
		tp_s = get_type_for_mode(lenv.low_signed);

	/* create method types for the created binop calls */
	if (! binop_tp_u) {
		binop_tp_u = new_type_method(4, 2);
		set_method_param_type(binop_tp_u, 0, tp_u);
		set_method_param_type(binop_tp_u, 1, tp_u);
		set_method_param_type(binop_tp_u, 2, tp_u);
		set_method_param_type(binop_tp_u, 3, tp_u);
		set_method_res_type(binop_tp_u, 0, tp_u);
		set_method_res_type(binop_tp_u, 1, tp_u);
	}
	if (! binop_tp_s) {
		binop_tp_s = new_type_method(4, 2);
		if (env->params->little_endian) {
			set_method_param_type(binop_tp_s, 0, tp_u);
			set_method_param_type(binop_tp_s, 1, tp_s);
			set_method_param_type(binop_tp_s, 2, tp_u);
			set_method_param_type(binop_tp_s, 3, tp_s);
			set_method_res_type(binop_tp_s, 0, tp_u);
			set_method_res_type(binop_tp_s, 1, tp_s);
		} else {
			set_method_param_type(binop_tp_s, 0, tp_s);
			set_method_param_type(binop_tp_s, 1, tp_u);
			set_method_param_type(binop_tp_s, 2, tp_s);
			set_method_param_type(binop_tp_s, 3, tp_u);
			set_method_res_type(binop_tp_s, 0, tp_s);
			set_method_res_type(binop_tp_s, 1, tp_u);
		}
	}
	if (! unop_tp_u) {
		unop_tp_u = new_type_method(2, 2);
		set_method_param_type(unop_tp_u, 0, tp_u);
		set_method_param_type(unop_tp_u, 1, tp_u);
		set_method_res_type(unop_tp_u, 0, tp_u);
		set_method_res_type(unop_tp_u, 1, tp_u);
	}
	if (! unop_tp_s) {
		unop_tp_s = new_type_method(2, 2);
		if (env->params->little_endian) {
			set_method_param_type(unop_tp_s, 0, tp_u);
			set_method_param_type(unop_tp_s, 1, tp_s);
			set_method_res_type(unop_tp_s, 0, tp_u);
			set_method_res_type(unop_tp_s, 1, tp_s);
		} else {
			set_method_param_type(unop_tp_s, 0, tp_s);
			set_method_param_type(unop_tp_s, 1, tp_u);
			set_method_res_type(unop_tp_s, 0, tp_s);
			set_method_res_type(unop_tp_s, 1, tp_u);
		}
	}

	lenv.tv_mode_bytes = new_tarval_from_long(param->doubleword_size/(2*8), lenv.low_unsigned);
	lenv.waitq         = new_pdeq();
	lenv.first_id      = new_id_from_chars(param->little_endian ? ".l" : ".h", 2);
	lenv.next_id       = new_id_from_chars(param->little_endian ? ".h" : ".l", 2);

	irp_reserve_resources(irp, IRP_RESOURCE_TYPE_LINK | IRP_RESOURCE_TYPE_VISITED);
	inc_master_type_visited();
	/* transform all graphs */
	for (i = 0, n = get_irp_n_irgs(); i < n; ++i) {
		ir_graph *irg = get_irp_irg(i);

		ir_nodeset_init(&created_mux_nodes);

		lower_irg(irg);

		if (ir_nodeset_size(&created_mux_nodes) > 0)
			lower_mux(irg, lower_mux_cb);

		ir_nodeset_destroy(&created_mux_nodes);
	}
	irp_free_resources(irp, IRP_RESOURCE_TYPE_LINK | IRP_RESOURCE_TYPE_VISITED);
	del_pdeq(lenv.waitq);

	env = NULL;
}

/* Default implementation. */
ir_entity *def_create_intrinsic_fkt(ir_type *method, const ir_op *op,
                                    const ir_mode *imode, const ir_mode *omode,
                                    void *context)
{
	char buf[64];
	ident *id;
	ir_entity *ent;
	(void) context;

	if (imode == omode) {
		snprintf(buf, sizeof(buf), "__l%s%s", get_op_name(op), get_mode_name(imode));
	} else {
		snprintf(buf, sizeof(buf), "__l%s%s%s", get_op_name(op),
			get_mode_name(imode), get_mode_name(omode));
	}
	id = new_id_from_str(buf);

	ent = new_entity(get_glob_type(), id, method);
	set_entity_ld_ident(ent, get_entity_ident(ent));
	set_entity_visibility(ent, ir_visibility_external);
	return ent;
}
