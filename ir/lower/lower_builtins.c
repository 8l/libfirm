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
 * @brief   Lowering of builtins to compiler-lib calls
 * @author  Matthias Braun
 */
#include "config.h"

#include "lower_builtins.h"
#include <stdbool.h>
#include <stdlib.h>
#include "adt/pmap.h"
#include "irnode_t.h"
#include "ircons_t.h"
#include "irgmod.h"
#include "irgwalk.h"
#include "error.h"

static pmap *entities;
static bool dont_lower[ir_bk_last+1];

static const char *get_builtin_name(ir_builtin_kind kind)
{
	switch (kind) {
	case ir_bk_ffs:      return "ffs";
	case ir_bk_clz:      return "clz";
	case ir_bk_ctz:      return "ctz";
	case ir_bk_popcount: return "popcount";
	case ir_bk_parity:   return "parity";
	case ir_bk_bswap:    return "bswap";
	case ir_bk_prefetch:
	case ir_bk_trap:
	case ir_bk_debugbreak:
	case ir_bk_return_address:
	case ir_bk_frame_address:
	case ir_bk_inport:
	case ir_bk_outport:
	case ir_bk_inner_trampoline:
		break;
	}
	abort();
}

static const char *get_gcc_machmode(ir_type *type)
{
	assert(is_Primitive_type(type));
	switch (get_type_size_bytes(type)) {
	case 4: return "si";
	case 8: return "di";
	default:
		panic("couldn't determine gcc machmode for type %+F", type);
	}
}

static void replace_with_call(ir_node *node)
{
	ir_graph       *irg   = get_irn_irg(node);
	ir_node        *block = get_nodes_block(node);
	ir_builtin_kind kind  = get_Builtin_kind(node);
	const char     *name  = get_builtin_name(kind);
	ir_type        *mtp   = get_Builtin_type(node);
	ir_type        *arg1  = get_method_param_type(mtp, 0);
	dbg_info       *dbgi  = get_irn_dbg_info(node);
	ir_node        *mem   = get_Builtin_mem(node);
	const char     *gcc_machmode = get_gcc_machmode(arg1);
	int             n_params     = get_Builtin_n_params(node);
	ir_node       **params       = get_Builtin_param_arr(node);
	ir_type        *res_type = get_method_res_type(mtp, 0);
	ir_mode        *res_mode = get_type_mode(res_type);
	ir_node        *call_mem;
	ir_node        *call_ress;
	ir_node        *call_res;
	ir_entity      *entity;
	ir_node        *symconst;
	ir_node        *call;
	ident          *id;
	union symconst_symbol sym;

	char buf[64];
	snprintf(buf, sizeof(buf), "__%s%s2", name, gcc_machmode);
	id = new_id_from_str(buf);

	entity = pmap_get(entities, id);
	if (entity == NULL) {
		ir_type   *glob   = get_glob_type();
		entity = new_entity(glob, id, mtp);
		set_entity_visibility(entity, ir_visibility_external);
		pmap_insert(entities, id, entity);
	}

	sym.entity_p = entity;
	symconst  = new_r_SymConst(irg, mode_P, sym, symconst_addr_ent);
	call      = new_rd_Call(dbgi, block, mem, symconst, n_params, params, mtp);
	call_mem  = new_r_Proj(call, mode_M, pn_Call_M);
	call_ress = new_r_Proj(call, mode_T, pn_Call_T_result);
	call_res  = new_r_Proj(call_ress, res_mode, 0);

	turn_into_tuple(node, 2);
	set_irn_n(node, pn_Builtin_M, call_mem);
	set_irn_n(node, pn_Builtin_1_result, call_res);
}

static void lower_builtin(ir_node *node, void *env)
{
	ir_builtin_kind kind;
	(void) env;
	if (!is_Builtin(node))
		return;

	kind = get_Builtin_kind(node);
	if (dont_lower[kind])
		return;

	switch (kind) {
	case ir_bk_prefetch: {
		/* just remove it */
		ir_node *mem = get_Builtin_mem(node);
		turn_into_tuple(node, 1);
		set_irn_n(node, pn_Builtin_M, mem);
		break;
	}
	case ir_bk_ffs:
	case ir_bk_clz:
	case ir_bk_ctz:
	case ir_bk_popcount:
	case ir_bk_parity:
	case ir_bk_bswap:
		/* replace with a call */
		replace_with_call(node);
		return;

	case ir_bk_trap:
	case ir_bk_debugbreak:
	case ir_bk_return_address:
	case ir_bk_frame_address:
	case ir_bk_inport:
	case ir_bk_outport:
	case ir_bk_inner_trampoline:
		/* can't do anything about these, backend will probably fail now */
		panic("Can't lower Builtin node of kind %+F", node);
	}
}

void lower_builtins(size_t n_exceptions, ir_builtin_kind *exceptions)
{
	size_t i;
	size_t n_irgs;
	memset(dont_lower, 0, sizeof(dont_lower));
	for (i = 0; i < n_exceptions; ++i) {
		dont_lower[exceptions[i]] = true;
	}

	entities = pmap_create();

	n_irgs = get_irp_n_irgs();
	for (i = 0; i < n_irgs; ++i) {
		ir_graph *irg = get_irp_irg(i);
		irg_walk_graph(irg, NULL, lower_builtin, NULL);
	}

	pmap_destroy(entities);
}
