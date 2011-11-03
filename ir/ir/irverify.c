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
 * @brief    Check irnodes for correctness.
 * @author   Christian Schaefer, Goetz Lindenmaier, Till Riedel, Michael Beck
 * @version  $Id$
 */
#include "config.h"

#include "irprog.h"
#include "irop_t.h"
#include "irgraph_t.h"
#include "irverify_t.h"
#include "irgwalk.h"
#include "irdump.h"
#include "irdom_t.h"
#include "irprintf.h"
#include "irouts.h"
#include "irflag_t.h"
#include "irpass_t.h"
#include "irnodeset.h"

/** if this flag is set, verify entity types in Load & Store nodes */
static int verify_entities = 0;

const char *firm_verify_failure_msg;

/* enable verification of Load/Store entities */
void verify_enable_entity_tests(int enable)
{
	verify_entities = enable;
}

#ifndef NDEBUG

/**
 * little helper for NULL modes
 */
static const char *get_mode_name_ex(ir_mode *mode)
{
	if (! mode)
		return "<no mode>";
	return get_mode_name(mode);
}

/** the last IRG, on which a verification error was found */
static ir_graph *last_irg_error = NULL;

/**
 * print the name of the entity of an verification failure
 *
 * @param node  the node caused the failure
 */
static void show_entity_failure(const ir_node *node)
{
	ir_graph *irg = get_irn_irg(node);

	if (last_irg_error == irg)
		return;

	last_irg_error = irg;

	if (irg == get_const_code_irg()) {
		fprintf(stderr, "\nFIRM: irn_verify_irg() <of CONST_CODE_IRG> failed\n");
	} else {
		ir_entity *ent = get_irg_entity(irg);

		if (ent) {
			ir_type *ent_type = get_entity_owner(ent);

			if (ent_type) {
				ir_fprintf(stderr, "\nFIRM: irn_verify_irg() %+F::%s failed\n",
				           ent_type, get_entity_name(ent));
			} else {
				fprintf(stderr, "\nFIRM: irn_verify_irg() <NULL>::%s failed\n", get_entity_name(ent));
			}
		} else {
			fprintf(stderr, "\nFIRM: irn_verify_irg() <IRG %p> failed\n", (void *)irg);
		}
	}
}

static const char *get_irn_modename(const ir_node *node)
{
	ir_mode *mode = get_irn_mode(node);
	return get_mode_name(mode);
}

/**
 * Prints a failure for a Node
 */
static void show_node_failure(const ir_node *n)
{
	show_entity_failure(n);
	fprintf(stderr, "  node %ld %s%s\n" ,
		get_irn_node_nr(n),
		get_irn_opname(n), get_irn_modename(n)
	);
}

/**
 * Prints a failure message for a binop
 */
static void show_binop_failure(const ir_node *n, const char *text)
{
	ir_node *left  = get_binop_left(n);
	ir_node *right = get_binop_right(n);

	show_entity_failure(n);
	fprintf(stderr, "  node %ld %s%s(%s%s, %s%s) did not match (%s)\n",
		get_irn_node_nr(n),
		get_irn_opname(n), get_irn_modename(n),
		get_irn_opname(left), get_irn_modename(left),
		get_irn_opname(right), get_irn_modename(right),
		text);
}

/**
 * Prints a failure message for an unop
 */
static void show_unop_failure(const ir_node *n, const char *text)
{
	ir_node *op  = get_unop_op(n);

	show_entity_failure(n);
	fprintf(stderr, "  node %ld %s%s(%s%s) did not match (%s)\n",
		get_irn_node_nr(n),
		get_irn_opname(n), get_irn_modename(n),
		get_irn_opname(op), get_irn_modename(op),
		text);
}

/**
 * Prints a failure message for an op with 3 operands
 */
static void show_triop_failure(const ir_node *n, const char *text)
{
	ir_node *op0  = get_irn_n(n, 0);
	ir_node *op1  = get_irn_n(n, 1);
	ir_node *op2  = get_irn_n(n, 2);

	show_entity_failure(n);
	fprintf(stderr, "  of node %ld %s%s(%s%s, %s%s, %s%s) did not match (%s)\n",
		get_irn_node_nr(n),
		get_irn_opname(n), get_irn_modename(n),
		get_irn_opname(op0), get_irn_modename(op0),
		get_irn_opname(op1), get_irn_modename(op1),
		get_irn_opname(op2), get_irn_modename(op2),
		text);
}

/**
 * Prints a failure message for a proj
 */
static void show_proj_failure(const ir_node *n)
{
	ir_node *op  = get_Proj_pred(n);
	int proj     = get_Proj_proj(n);

	show_entity_failure(n);
	fprintf(stderr, "  node %ld %s%s %d(%s%s) failed\n" ,
		get_irn_node_nr(n),
		get_irn_opname(n), get_irn_modename(n), proj,
		get_irn_opname(op), get_irn_modename(op));
}

/**
 * Prints a failure message for a proj from Start
 */
static void show_proj_mode_failure(const ir_node *n, ir_type *ty)
{
	long proj  = get_Proj_proj(n);
	ir_mode *m = get_type_mode(ty);
	char type_name[256];
	ir_print_type(type_name, sizeof(type_name), ty);

	show_entity_failure(n);
	fprintf(stderr, "  Proj %ld mode %s proj %ld (type %s mode %s) failed\n" ,
		get_irn_node_nr(n),
		get_irn_modename(n),
		proj,
		type_name,
		get_mode_name_ex(m));
}

/**
 * Prints a failure message for a proj
 */
static void show_proj_failure_ent(const ir_node *n, ir_entity *ent)
{
	ir_node *op  = get_Proj_pred(n);
	int proj     = get_Proj_proj(n);
	ir_mode *m   = get_type_mode(get_entity_type(ent));
	char type_name[256];
	ir_print_type(type_name, sizeof(type_name), get_entity_type(ent));

	show_entity_failure(n);
	fprintf(stderr, "  node %ld %s%s %d(%s%s) entity %s(type %s mode %s)failed\n" ,
		get_irn_node_nr(n),
		get_irn_opname(n), get_irn_modename(n), proj,
		get_irn_opname(op), get_irn_modename(op),
		get_entity_name(ent), type_name,
		get_mode_name_ex(m));
}

/**
 * Show a node and a graph
 */
static void show_node_on_graph(const ir_graph *irg, const ir_node *n)
{
	ir_fprintf(stderr, "\nFIRM: irn_verify_irg() of %+F, node %+F\n", irg, n);
}

/**
 * Show call parameters
 */
static void show_call_param(const ir_node *n, ir_type *mt)
{
	size_t i;
	char type_name[256];
	ir_print_type(type_name, sizeof(type_name), mt);

	show_entity_failure(n);
	fprintf(stderr, "  Call type-check failed: %s(", type_name);
	for (i = 0; i < get_method_n_params(mt); ++i) {
		fprintf(stderr, "%s ", get_mode_name_ex(get_type_mode(get_method_param_type(mt, i))));
	}
	fprintf(stderr, ") != CALL(");

	for (i = 0; i < get_Call_n_params(n); ++i) {
		fprintf(stderr, "%s ", get_mode_name_ex(get_irn_mode(get_Call_param(n, i))));
	}
	fprintf(stderr, ")\n");
}

/**
 * Show return modes
 */
static void show_return_modes(const ir_graph *irg, const ir_node *n,
                              ir_type *mt, int i)
{
	ir_entity *ent = get_irg_entity(irg);

	show_entity_failure(n);
	fprintf(stderr, "  Return node %ld in entity \"%s\" mode %s different from type mode %s\n",
		get_irn_node_nr(n), get_entity_name(ent),
		get_mode_name_ex(get_irn_mode(get_Return_res(n, i))),
		get_mode_name_ex(get_type_mode(get_method_res_type(mt, i)))
	);
}

/**
 * Show return number of results
 */
static void show_return_nres(const ir_graph *irg, const ir_node *n, ir_type *mt)
{
	ir_entity *ent = get_irg_entity(irg);

	show_entity_failure(n);
	fprintf(stderr, "  Return node %ld in entity \"%s\" has %lu results different from type %lu\n",
		get_irn_node_nr(n), get_entity_name(ent),
		(unsigned long) get_Return_n_ress(n),
		(unsigned long) get_method_n_ress(mt));
}

/**
 * Show Phi input
 */
static void show_phi_failure(const ir_node *phi, const ir_node *pred, int pos)
{
	(void) pos;
	show_entity_failure(phi);
	fprintf(stderr, "  Phi node %ld has mode %s different from predeccessor node %ld mode %s\n",
		get_irn_node_nr(phi), get_mode_name_ex(get_irn_mode(phi)),
		get_irn_node_nr(pred), get_mode_name_ex(get_irn_mode(pred)));
}

/**
 * Show Phi inputs
 */
static void show_phi_inputs(const ir_node *phi, const ir_node *block)
{
	show_entity_failure(phi);
	fprintf(stderr, "  Phi node %ld has %d inputs, its Block %ld has %d\n",
		get_irn_node_nr(phi),   get_irn_arity(phi),
		get_irn_node_nr(block), get_irn_arity(block));
}

#endif /* #ifndef NDEBUG */

/**
 * If the address is Sel or SymConst, return the entity.
 *
 * @param ptr  the node representing the address
 */
static ir_entity *get_ptr_entity(const ir_node *ptr)
{
	if (is_Sel(ptr)) {
		return get_Sel_entity(ptr);
	} else if (is_SymConst_addr_ent(ptr)) {
		return get_SymConst_entity(ptr);
	}
	return NULL;
}

/**
 * verify a Proj(Start) node
 */
static int verify_node_Proj_Start(const ir_node *p)
{
	ir_mode *mode = get_irn_mode(p);
	long proj     = get_Proj_proj(p);

	ASSERT_AND_RET_DBG(
		(
			(proj == pn_Start_X_initial_exec && mode == mode_X) ||
			(proj == pn_Start_M              && mode == mode_M) ||
			(proj == pn_Start_P_frame_base   && mode_is_reference(mode)) ||
			(proj == pn_Start_T_args         && mode == mode_T)
		),
		"wrong Proj from Start", 0,
		show_proj_failure(p);
	);
	return 1;
}

/**
 * verify a Proj(Cond) node
 */
static int verify_node_Proj_Cond(const ir_node *p)
{
	ir_mode *mode = get_irn_mode(p);
	long     proj = get_Proj_proj(p);

	ASSERT_AND_RET_DBG(
		mode == mode_X && (proj == pn_Cond_false || proj == pn_Cond_true),
		"wrong Proj from Cond", 0,
		show_proj_failure(p);
	);
	return 1;
}

static int verify_node_Proj_Switch(const ir_node *p)
{
	ir_mode *mode = get_irn_mode(p);
	long     pn   = get_Proj_proj(p);
	ir_node *pred = get_Proj_pred(p);
	ASSERT_AND_RET_DBG(
		mode == mode_X && (pn >= 0 && pn < (long)get_Switch_n_outs(pred)),
		"wrong Proj from Switch", 0,
		show_proj_failure(p);
	);
	return 1;
}

/**
 * verify a Proj(Raise) node
 */
static int verify_node_Proj_Raise(const ir_node *p)
{
	ir_mode *mode = get_irn_mode(p);
	long proj     = get_Proj_proj(p);

	ASSERT_AND_RET_DBG(
		((proj == pn_Raise_X && mode == mode_X) || (proj == pn_Raise_M && mode == mode_M)),
		"wrong Proj from Raise", 0,
		show_proj_failure(p);
	);
	return 1;
}

/**
 * verify a Proj(InstOf) node
 */
static int verify_node_Proj_InstOf(const ir_node *p)
{
	ir_mode *mode = get_irn_mode(p);
	long proj     = get_Proj_proj(p);

	ASSERT_AND_RET_DBG(
		(
			(proj == pn_InstOf_M         && mode == mode_M) ||
			(proj == pn_InstOf_X_regular && mode == mode_X) ||
			(proj == pn_InstOf_X_except  && mode == mode_X) ||
			(proj == pn_InstOf_res       && mode_is_reference(mode))
		),
		"wrong Proj from InstOf", 0,
		show_proj_failure(p);
	);
	return 1;
}

/**
 * verify a Proj(Call) node
 */
static int verify_node_Proj_Call(const ir_node *p)
{
	ir_mode *mode = get_irn_mode(p);
	ir_node *n    = get_Proj_pred(p);
	long proj     = get_Proj_proj(p);

	ASSERT_AND_RET_DBG(
		(
			(proj == pn_Call_M                && mode == mode_M) ||
			(proj == pn_Call_X_regular        && mode == mode_X) ||
			(proj == pn_Call_X_except         && mode == mode_X) ||
			(proj == pn_Call_T_result         && mode == mode_T)
		),
		"wrong Proj from Call", 0,
		show_proj_failure(p);
	);
	/* if we have exception flow, we must have a real Memory input */
	if (proj == pn_Call_X_regular)
		ASSERT_AND_RET(
			!is_NoMem(get_Call_mem(n)),
			"Regular Proj from FunctionCall", 0);
	else if (proj == pn_Call_X_except)
		ASSERT_AND_RET(
			!is_NoMem(get_Call_mem(n)),
			"Exception Proj from FunctionCall", 0);
	return 1;
}

/**
 * verify a Proj(Div) node
 */
static int verify_node_Proj_Div(const ir_node *p)
{
	ir_mode *mode = get_irn_mode(p);
	ir_node *n    = get_Proj_pred(p);
	long proj     = get_Proj_proj(p);

	ASSERT_AND_RET_DBG(
		(
			(proj == pn_Div_M         && mode == mode_M) ||
			(proj == pn_Div_X_regular && mode == mode_X) ||
			(proj == pn_Div_X_except  && mode == mode_X) ||
			(proj == pn_Div_res       && mode_is_data(mode) && mode == get_Div_resmode(n))
		),
		"wrong Proj from Div", 0,
		show_proj_failure(p);
	);
	if (proj == pn_Div_X_regular)
		ASSERT_AND_RET(
			get_irn_pinned(n) == op_pin_state_pinned,
			"Regular Proj from unpinned Div", 0);
	else if (proj == pn_Div_X_except)
		ASSERT_AND_RET(
			get_irn_pinned(n) == op_pin_state_pinned,
			"Exception Proj from unpinned Div", 0);
	else if (proj == pn_Div_M)
		ASSERT_AND_RET(
			get_irn_pinned(n) == op_pin_state_pinned,
			"Memory Proj from unpinned Div", 0);
	return 1;
}

/**
 * verify a Proj(Mod) node
 */
static int verify_node_Proj_Mod(const ir_node *p)
{
	ir_mode *mode = get_irn_mode(p);
	ir_node *n    = get_Proj_pred(p);
	long proj     = get_Proj_proj(p);

	ASSERT_AND_RET_DBG(
		(
			(proj == pn_Mod_M         && mode == mode_M) ||
			(proj == pn_Mod_X_regular && mode == mode_X) ||
			(proj == pn_Mod_X_except  && mode == mode_X) ||
			(proj == pn_Mod_res       && mode == get_Mod_resmode(n))
		),
		"wrong Proj from Mod", 0,
		show_proj_failure(p);
	);
	if (proj == pn_Mod_X_regular)
		ASSERT_AND_RET(
			get_irn_pinned(n) == op_pin_state_pinned,
			"Regular Proj from unpinned Mod", 0);
	else if (proj == pn_Mod_X_except)
		ASSERT_AND_RET(
			get_irn_pinned(n) == op_pin_state_pinned,
			"Exception Proj from unpinned Mod", 0);
	else if (proj == pn_Mod_M)
		ASSERT_AND_RET(
			get_irn_pinned(n) == op_pin_state_pinned,
			"Memory Proj from unpinned Div", 0);
	return 1;
}

/**
 * verify a Proj(Load) node
 */
static int verify_node_Proj_Load(const ir_node *p)
{
	ir_mode *mode = get_irn_mode(p);
	ir_node *n    = get_Proj_pred(p);
	long proj     = get_Proj_proj(p);

	if (proj == pn_Load_res) {
		ir_node   *ptr = get_Load_ptr(n);
		ir_entity *ent = get_ptr_entity(ptr);
		ir_graph  *irg = get_irn_irg(n);

		if (verify_entities && ent && get_irg_phase_state(irg) == phase_high) {
			/* do NOT check this for lowered phases, see comment on Store */
			ASSERT_AND_RET_DBG(
				(mode == get_type_mode(get_entity_type(ent))),
				"wrong data Proj from Load, entity type_mode failed", 0,
				show_proj_failure_ent(p, ent);
			);
		}
		else {
			ASSERT_AND_RET_DBG(
				mode_is_data(mode) && mode == get_Load_mode(n),
				"wrong data Proj from Load", 0,
				show_proj_failure(p);
			);
		}
	}
	else {
		ASSERT_AND_RET_DBG(
			(
				(proj == pn_Load_M         && mode == mode_M) ||
				(proj == pn_Load_X_regular && mode == mode_X) ||
				(proj == pn_Load_X_except  && mode == mode_X)
			),
			"wrong Proj from Load", 0,
			show_proj_failure(p);
		);
	}
	if (proj == pn_Load_X_regular) {
		ASSERT_AND_RET(
			get_irn_pinned(n) == op_pin_state_pinned,
			"Regular Proj from unpinned Load", 0);
	} else if (proj == pn_Load_X_except) {
		ASSERT_AND_RET(
			get_irn_pinned(n) == op_pin_state_pinned,
			"Exception Proj from unpinned Load", 0);
	}
	return 1;
}

/**
 * verify a Proj(Store) node
 */
static int verify_node_Proj_Store(const ir_node *p)
{
	ir_mode *mode = get_irn_mode(p);
	ir_node *n    = get_Proj_pred(p);
	long proj     = get_Proj_proj(p);

	ASSERT_AND_RET_DBG(
		(
			(proj == pn_Store_M         && mode == mode_M) ||
			(proj == pn_Store_X_regular && mode == mode_X) ||
			(proj == pn_Store_X_except  && mode == mode_X)
		),
		"wrong Proj from Store", 0,
		show_proj_failure(p);
	);
	if (proj == pn_Store_X_regular) {
		ASSERT_AND_RET(
			get_irn_pinned(n) == op_pin_state_pinned,
			"Regular Proj from unpinned Store", 0);
	} else if (proj == pn_Store_X_except) {
		ASSERT_AND_RET(
			get_irn_pinned(n) == op_pin_state_pinned,
			"Exception Proj from unpinned Store", 0);
	}
	return 1;
}

/**
 * verify a Proj(Alloc) node
 */
static int verify_node_Proj_Alloc(const ir_node *p)
{
	ir_mode *mode = get_irn_mode(p);
	long proj     = get_Proj_proj(p);

	ASSERT_AND_RET_DBG(
		(
			(proj == pn_Alloc_M         && mode == mode_M) ||
			(proj == pn_Alloc_X_regular && mode == mode_X) ||
			(proj == pn_Alloc_X_except  && mode == mode_X) ||
			(proj == pn_Alloc_res       && mode_is_reference(mode))
		),
		"wrong Proj from Alloc", 0,
		show_proj_failure(p);
	);
	return 1;
}

/**
 * verify a Proj(Proj) node
 */
static int verify_node_Proj_Proj(const ir_node *p)
{
	ir_mode *mode = get_irn_mode(p);
	ir_node *pred = get_Proj_pred(p);
	long proj     = get_Proj_proj(p);
	long nr       = get_Proj_proj(pred);
	ir_type *mt; /* A method type */

	pred = skip_Id(get_Proj_pred(pred));
	ASSERT_AND_RET((get_irn_mode(pred) == mode_T), "Proj from something not a tuple", 0);

	switch (get_irn_opcode(pred)) {
	case iro_Start:
		mt = get_entity_type(get_irg_entity(get_irn_irg(pred)));

		if (nr == pn_Start_T_args) {
			ASSERT_AND_RET(
				(proj >= 0 && mode_is_datab(mode)),
				"wrong Proj from Proj from Start", 0);
			ASSERT_AND_RET(
				(proj < (int)get_method_n_params(mt)),
				"More Projs for args than args in type", 0
				);
			if ((mode_is_reference(mode)) && is_compound_type(get_method_param_type(mt, proj)))
				/* value argument */ break;

			if (get_irg_phase_state(get_irn_irg(pred)) != phase_backend) {
				ASSERT_AND_RET_DBG(
						(mode == get_type_mode(get_method_param_type(mt, proj))),
						"Mode of Proj from Start doesn't match mode of param type.", 0,
						show_proj_mode_failure(p, get_method_param_type(mt, proj));
						);
			}
		}
		break;

	case iro_Call:
		{
			ASSERT_AND_RET(
				(proj >= 0 && mode_is_datab(mode)),
				"wrong Proj from Proj from Call", 0);
			mt = get_Call_type(pred);
			ASSERT_AND_RET(mt == get_unknown_type() || is_Method_type(mt),
					"wrong call type on call", 0);
			ASSERT_AND_RET(
				(proj < (int)get_method_n_ress(mt)),
				"More Projs for results than results in type.", 0);
			if ((mode_is_reference(mode)) && is_compound_type(get_method_res_type(mt, proj)))
				/* value result */ break;

				ASSERT_AND_RET(
				(mode == get_type_mode(get_method_res_type(mt, proj))),
				"Mode of Proj from Call doesn't match mode of result type.", 0);
		}
		break;

	case iro_Tuple:
		/* We don't test */
		break;

	default:
		/* ASSERT_AND_RET(0, "Unknown opcode", 0); */
		break;
	}
	return 1;
}

/**
 * verify a Proj(Tuple) node
 */
static int verify_node_Proj_Tuple(const ir_node *p)
{
	(void) p;
	/* We don't test */
	return 1;
}

/**
 * verify a Proj(CopyB) node
 */
static int verify_node_Proj_CopyB(const ir_node *p)
{
	ir_mode *mode = get_irn_mode(p);
	ir_node *n    = get_Proj_pred(p);
	long proj     = get_Proj_proj(p);

	ASSERT_AND_RET_DBG(
		(
			(proj == pn_CopyB_M         && mode == mode_M) ||
			(proj == pn_CopyB_X_regular && mode == mode_X) ||
			(proj == pn_CopyB_X_except  && mode == mode_X)
		),
		"wrong Proj from CopyB", 0,
		show_proj_failure(p);
	);
	if (proj == pn_CopyB_X_regular)
		ASSERT_AND_RET(
			get_irn_pinned(n) == op_pin_state_pinned,
			"Regular Proj from unpinned CopyB", 0);
	else if (proj == pn_CopyB_X_except)
		ASSERT_AND_RET(
			get_irn_pinned(n) == op_pin_state_pinned,
			"Exception Proj from unpinned CopyB", 0);
	return 1;
}

/**
 * verify a Proj(Bound) node
 */
static int verify_node_Proj_Bound(const ir_node *p)
{
	ir_mode *mode = get_irn_mode(p);
	ir_node *n    = get_Proj_pred(p);
	long proj     = get_Proj_proj(p);

	ASSERT_AND_RET_DBG(
		(
			(proj == pn_Bound_M         && mode == mode_M) ||
			(proj == pn_Bound_X_regular && mode == mode_X) ||
			(proj == pn_Bound_X_except  && mode == mode_X) ||
			(proj == pn_Bound_res       && mode == get_irn_mode(get_Bound_index(n)))
		),
		"wrong Proj from Bound", 0,
		show_proj_failure(p);
	);
	return 1;
}

static int verify_node_Proj_fragile(const ir_node *node)
{
	ir_node *pred             = get_Proj_pred(node);
	int      throws_exception = ir_throws_exception(pred);
	ASSERT_AND_RET((!is_x_except_Proj(node) || throws_exception)
	    && (!is_x_regular_Proj(node) || throws_exception),
	    "X_except und X_regular Proj only allowed when throws_exception is set",
	    0);
	return 1;
}

/**
 * verify a Proj node
 */
static int verify_node_Proj(const ir_node *p)
{
	ir_graph *irg = get_irn_irg(p);
	ir_node *pred;
	ir_op *op;

	pred = skip_Id(get_Proj_pred(p));
	ASSERT_AND_RET(get_irn_mode(pred) == mode_T, "mode of a 'projed' node is not Tuple", 0);
	ASSERT_AND_RET(get_irg_pinned(irg) == op_pin_state_floats || get_nodes_block(pred) == get_nodes_block(p), "Proj must be in same block as its predecessor", 0);

	if (is_fragile_op(pred)) {
		int res = verify_node_Proj_fragile(p);
		if (res != 1)
			return res;
	}

	op = get_irn_op(pred);
	if (op->ops.verify_proj_node)
		return op->ops.verify_proj_node(p);

	/* all went ok */
	return 1;
}

/**
 * verify a Block node
 */
static int verify_node_Block(const ir_node *n)
{
	ir_graph *irg = get_irn_irg(n);
	int i;

	for (i = get_Block_n_cfgpreds(n) - 1; i >= 0; --i) {
		ir_node *pred         = get_Block_cfgpred(n, i);
		ir_node *skipped_pred = skip_Proj(skip_Tuple(pred));
		ASSERT_AND_RET(get_irn_mode(pred) == mode_X,
			"Block node must have a mode_X predecessor", 0);
		ASSERT_AND_RET(is_cfop(skipped_pred) || is_Bad(skipped_pred), "Block predecessor must be a cfop (or Bad)", 0);
	}

	if (n == get_irg_start_block(irg)) {
		ASSERT_AND_RET(get_Block_n_cfgpreds(n) == 0, "Start Block node", 0);
	}

	if (n == get_irg_end_block(irg) && get_irg_phase_state(irg) != phase_backend) {
		/* End block may only have Return, Raise or fragile ops as preds. */
		for (i = get_Block_n_cfgpreds(n) - 1; i >= 0; --i) {
			ir_node *pred =  skip_Proj(get_Block_cfgpred(n, i));
			if (is_Proj(pred) || is_Tuple(pred))
				break;   /*  We can not test properly.  How many tuples are there? */
			ASSERT_AND_RET(
				(
					is_Return(pred) ||
					is_Bad(pred)    ||
					is_Raise(pred)  ||
					is_fragile_op(pred)
				),
				"End Block node", 0);
		}
	}
	/*  irg attr must == graph we are in. */
	ASSERT_AND_RET(((get_irn_irg(n) && get_irn_irg(n) == irg)), "Block node has wrong irg attribute", 0);
	return 1;
}

/**
 * verify a Start node
 */
static int verify_node_Start(const ir_node *n)
{
	ir_mode *mymode = get_irn_mode(n);

	ASSERT_AND_RET(
		/* Start: BB --> X x M x ref x data1 x ... x datan x ref */
		mymode == mode_T, "Start node", 0
		);
	return 1;
}

/**
 * verify a Jmp node
 */
static int verify_node_Jmp(const ir_node *n)
{
	ir_mode *mymode = get_irn_mode(n);

	ASSERT_AND_RET(
		/* Jmp: BB --> X */
		mymode == mode_X, "Jmp node", 0
	);
	return 1;
}

/**
 * verify an IJmp node
 */
static int verify_node_IJmp(const ir_node *n)
{
	ir_mode *mymode  = get_irn_mode(n);
	ir_mode *op1mode = get_irn_mode(get_IJmp_target(n));

	ASSERT_AND_RET(
		/* IJmp: BB x ref --> X */
		mymode == mode_X && mode_is_reference(op1mode), "IJmp node", 0
	);
	return 1;
}

/**
 * verify a Cond node
 */
static int verify_node_Cond(const ir_node *n)
{
	ir_mode *mymode  = get_irn_mode(n);
	ir_mode *op1mode = get_irn_mode(get_Cond_selector(n));

	ASSERT_AND_RET(op1mode == mode_b, "Cond operand not mode_b", 0);
	ASSERT_AND_RET(mymode == mode_T, "Cond mode is not a tuple", 0);
	return 1;
}

static int verify_switch_table(const ir_node *n)
{
	const ir_switch_table *table     = get_Switch_table(n);
	size_t                 n_entries = ir_switch_table_get_n_entries(table);
	unsigned               n_outs    = get_Switch_n_outs(n);
	size_t                 e;

	for (e = 0; e < n_entries; ++e) {
		const ir_switch_table_entry *entry
			= ir_switch_table_get_entry_const(table, e);
		if (entry->pn == 0)
			continue;
		ASSERT_AND_RET(entry->min != NULL && entry->max != NULL,
		               "switch table entry without min+max value", 0);
		ASSERT_AND_RET(tarval_cmp(entry->min, entry->max) != ir_relation_greater,
		               "switch table entry without min+max value", 0);
		ASSERT_AND_RET(entry->pn >= 0 && entry->pn < (long)n_outs,
					   "switch table entry with invalid proj number", 0);
	}
	return 1;
}

static int verify_node_Switch(const ir_node *n)
{
	ir_mode *mymode  = get_irn_mode(n);
	ir_mode *op1mode = get_irn_mode(get_Switch_selector(n));
	if (!verify_switch_table(n))
		return 0;

	ASSERT_AND_RET(mode_is_int(op1mode), "Switch operand not integer", 0);
	ASSERT_AND_RET(mymode == mode_T, "Switch mode is not a tuple", 0);
	return 1;
}

/**
 * verify a Return node
 */
static int verify_node_Return(const ir_node *n)
{
	ir_graph *irg      = get_irn_irg(n);
	ir_mode  *mymode   = get_irn_mode(n);
	ir_mode  *mem_mode = get_irn_mode(get_Return_mem(n));
	ir_type  *mt;
	int       i;

	/* Return: BB x M x data1 x ... x datan --> X */

	ASSERT_AND_RET( mem_mode == mode_M, "Return node", 0 );  /* operand M */

	for (i = get_Return_n_ress(n) - 1; i >= 0; --i) {
		ASSERT_AND_RET( mode_is_datab(get_irn_mode(get_Return_res(n, i))), "Return node", 0 );  /* operand datai */
	}
	ASSERT_AND_RET( mymode == mode_X, "Result X", 0 );   /* result X */
	/* Compare returned results with result types of method type */
	mt = get_entity_type(get_irg_entity(irg));
	ASSERT_AND_RET_DBG(get_Return_n_ress(n) == get_method_n_ress(mt),
		"Number of results for Return doesn't match number of results in type.", 0,
		show_return_nres(irg, n, mt););
	for (i = get_Return_n_ress(n) - 1; i >= 0; --i) {
		ir_type *res_type = get_method_res_type(mt, i);

		if (get_irg_phase_state(irg) != phase_backend) {
			if (is_atomic_type(res_type)) {
				ASSERT_AND_RET_DBG(
					get_irn_mode(get_Return_res(n, i)) == get_type_mode(res_type),
					"Mode of result for Return doesn't match mode of result type.", 0,
					show_return_modes(irg, n, mt, i);
				);
			} else {
				ASSERT_AND_RET_DBG(
					mode_is_reference(get_irn_mode(get_Return_res(n, i))),
					"Mode of result for Return doesn't match mode of result type.", 0,
					show_return_modes(irg, n, mt, i);
				);
			}
		}
	}
	return 1;
}

/**
 * verify a Raise node
 */
static int verify_node_Raise(const ir_node *n)
{
	ir_mode *mymode  = get_irn_mode(n);
	ir_mode *op1mode = get_irn_mode(get_Raise_mem(n));
	ir_mode *op2mode = get_irn_mode(get_Raise_exo_ptr(n));

	ASSERT_AND_RET(
		/* Sel: BB x M x ref --> X x M */
		op1mode == mode_M && mode_is_reference(op2mode) &&
		mymode == mode_T, "Raise node", 0
	);
	return 1;
}

/**
 * verify a Const node
 */
static int verify_node_Const(const ir_node *n)
{
	ir_mode *mymode = get_irn_mode(n);

	ASSERT_AND_RET(
		/* Const: BB --> data */
		(mode_is_data(mymode) ||
		mymode == mode_b)      /* we want boolean constants for static evaluation */
		,"Const node", 0       /* of Cmp. */
	);
	ASSERT_AND_RET(
		/* the modes of the constant and teh tarval must match */
		mymode == get_tarval_mode(get_Const_tarval(n)),
		"Const node, tarval and node mode mismatch", 0
	);
	return 1;
}

/**
 * verify a SymConst node
 */
static int verify_node_SymConst(const ir_node *n)
{
	ir_mode *mymode = get_irn_mode(n);

	ASSERT_AND_RET(
		/* SymConst: BB --> int*/
		(mode_is_int(mymode) ||
		/* SymConst: BB --> ref */
		mode_is_reference(mymode))
		,"SymConst node", 0);
	return 1;
}

/**
 * verify a Sel node
 */
static int verify_node_Sel(const ir_node *n)
{
	int i;
	ir_mode *mymode  = get_irn_mode(n);
	ir_mode *op1mode = get_irn_mode(get_Sel_mem(n));
	ir_mode *op2mode = get_irn_mode(get_Sel_ptr(n));
	ir_entity *ent;

	ASSERT_AND_RET_DBG(
		/* Sel: BB x M x ref x int^n --> ref */
		(op1mode == mode_M && op2mode == mymode && mode_is_reference(mymode)),
		"Sel node", 0, show_node_failure(n);
	);

	for (i = get_Sel_n_indexs(n) - 1; i >= 0; --i) {
		ASSERT_AND_RET_DBG(mode_is_int(get_irn_mode(get_Sel_index(n, i))), "Sel node", 0, show_node_failure(n););
	}
	ent = get_Sel_entity(n);
	ASSERT_AND_RET_DBG(ent, "Sel node with empty entity", 0, show_node_failure(n););
	return 1;
}

/**
 * verify an InstOf node
 */
static int verify_node_InstOf(const ir_node *n)
{
	ir_mode *mymode  = get_irn_mode(n);
	ir_mode *op1mode = get_irn_mode(get_InstOf_obj(n));

	ASSERT_AND_RET(mode_T == mymode, "mode of Instof is not a tuple", 0);
	ASSERT_AND_RET(mode_is_data(op1mode), "Instof not on data", 0);
	return 1;
}

/**
 * Check if the pinned state is right.
 */
static int verify_right_pinned(const ir_node *n)
{
	ir_node *mem;

	if (get_irn_pinned(n) == op_pin_state_pinned)
		return 1;
	mem = get_Call_mem(n);

	/* if it's not pinned, its memory predecessor must be NoMem or Pin */
	if (is_NoMem(mem) || is_Pin(mem))
		return 1;
	return 0;
}

/**
 * verify a Call node
 */
static int verify_node_Call(const ir_node *n)
{
	ir_graph *irg     = get_irn_irg(n);
	ir_mode  *mymode  = get_irn_mode(n);
	ir_mode  *op1mode = get_irn_mode(get_Call_mem(n));
	ir_mode  *op2mode = get_irn_mode(get_Call_ptr(n));
	ir_type  *mt;
	size_t    i;
	size_t    n_params;

	/* Call: BB x M x ref x data1 x ... x datan
	--> M x datan+1 x ... x data n+m */
	ASSERT_AND_RET( op1mode == mode_M && mode_is_reference(op2mode), "Call node", 0 );  /* operand M x ref */

	/* NoMem nodes are only allowed as memory input if the Call is NOT pinned */
	ASSERT_AND_RET(verify_right_pinned(n),"Call node with wrong memory input", 0 );

	mt = get_Call_type(n);
	if (get_unknown_type() == mt) {
		return 1;
	}

	n_params = get_Call_n_params(n);
	for (i = 0; i < n_params; ++i) {
		ASSERT_AND_RET( mode_is_datab(get_irn_mode(get_Call_param(n, i))), "Call node", 0 );  /* operand datai */
	}

	ASSERT_AND_RET( mymode == mode_T, "Call result not a tuple", 0 );   /* result T */
	/* Compare arguments of node with those of type */

	if (get_method_variadicity(mt) == variadicity_variadic) {
		ASSERT_AND_RET_DBG(
			get_Call_n_params(n) >= get_method_n_params(mt),
			"Number of args for Call doesn't match number of args in variadic type.",
			0,
			ir_fprintf(stderr, "Call %+F has %d params, type %d\n",
			n, get_Call_n_params(n), get_method_n_params(mt));
		);
	} else {
		ASSERT_AND_RET_DBG(
			get_Call_n_params(n) == get_method_n_params(mt),
			"Number of args for Call doesn't match number of args in non variadic type.",
			0,
			ir_fprintf(stderr, "Call %+F has %d params, type %d\n",
			n, get_Call_n_params(n), get_method_n_params(mt));
		);
	}

	for (i = 0; i < get_method_n_params(mt); i++) {
		ir_type *t = get_method_param_type(mt, i);

		if (get_irg_phase_state(irg) != phase_backend) {
			if (is_atomic_type(t)) {
				ASSERT_AND_RET_DBG(
					get_irn_mode(get_Call_param(n, i)) == get_type_mode(t),
					"Mode of arg for Call doesn't match mode of arg type.", 0,
					show_call_param(n, mt);
				);
			} else {
				/* call with a compound type, mode must be reference */
				ASSERT_AND_RET_DBG(
					mode_is_reference(get_irn_mode(get_Call_param(n, i))),
					"Mode of arg for Call doesn't match mode of arg type.", 0,
					show_call_param(n, mt);
				);
			}
		}
	}

	return 1;
}

/**
 * verify an Add node
 */
static int verify_node_Add(const ir_node *n)
{
	ir_mode *mymode  = get_irn_mode(n);
	ir_mode *op1mode = get_irn_mode(get_Add_left(n));
	ir_mode *op2mode = get_irn_mode(get_Add_right(n));

	ASSERT_AND_RET_DBG(
		(
			/* common Add: BB x numP x numP --> numP */
			(op1mode == mymode && op2mode == op1mode && mode_is_data(mymode)) ||
			/* Pointer Add: BB x ref x int --> ref */
			(mode_is_reference(op1mode) && mode_is_int(op2mode) && op1mode == mymode) ||
			/* Pointer Add: BB x int x ref --> ref */
			(mode_is_int(op1mode) && op2mode == mymode && mode_is_reference(mymode))
		),
		"Add node", 0,
		show_binop_failure(n, "/* common Add: BB x numP x numP --> numP */ |\n"
			"/* Pointer Add: BB x ref x int --> ref */   |\n"
			"/* Pointer Add: BB x int x ref --> ref */");
	);
	return 1;
}

/**
 * verify a Sub node
 */
static int verify_node_Sub(const ir_node *n)
{
	ir_mode *mymode  = get_irn_mode(n);
	ir_mode *op1mode = get_irn_mode(get_Sub_left(n));
	ir_mode *op2mode = get_irn_mode(get_Sub_right(n));

	ASSERT_AND_RET_DBG(
		(
			/* common Sub: BB x numP x numP --> numP */
			(mymode ==op1mode && mymode == op2mode && mode_is_data(op1mode)) ||
			/* Pointer Sub: BB x ref x int --> ref */
			(op1mode == mymode && mode_is_int(op2mode) && mode_is_reference(mymode)) ||
			/* Pointer Sub: BB x ref x ref --> int */
			(op1mode == op2mode && mode_is_reference(op2mode) && mode_is_int(mymode))
		),
		"Sub node", 0,
		show_binop_failure(n, "/* common Sub: BB x numP x numP --> numP */ |\n"
			"/* Pointer Sub: BB x ref x int --> ref */   |\n"
			"/* Pointer Sub: BB x ref x ref --> int */" );
		);
	return 1;
}

/**
 * verify a Minus node
 */
static int verify_node_Minus(const ir_node *n)
{
	ir_mode *mymode  = get_irn_mode(n);
	ir_mode *op1mode = get_irn_mode(get_Minus_op(n));

	ASSERT_AND_RET_DBG(
		/* Minus: BB x num --> num */
		op1mode == mymode && mode_is_num(op1mode), "Minus node", 0,
		show_unop_failure(n , "/* Minus: BB x num --> num */");
	);
	return 1;
}

/**
 * verify a Mul node
 */
static int verify_node_Mul(const ir_node *n)
{
	ir_mode *mymode  = get_irn_mode(n);
	ir_mode *op1mode = get_irn_mode(get_Mul_left(n));
	ir_mode *op2mode = get_irn_mode(get_Mul_right(n));

	ASSERT_AND_RET_DBG(
		(
			/* Mul: BB x int_n x int_n --> int_n|int_2n */
			(mode_is_int(op1mode)   && op2mode == op1mode && mode_is_int(mymode) &&
			 (op1mode == mymode || get_mode_size_bits(op1mode) * 2 == get_mode_size_bits(mymode))) ||
			/* Mul: BB x float x float --> float */
			(mode_is_float(op1mode) && op2mode == op1mode && mymode == op1mode)
		),
		"Mul node",0,
		show_binop_failure(n, "/* Mul: BB x int_n x int_n --> int_n|int_2n */ |\n"
		"/* Mul: BB x float x float --> float */");
	);
	return 1;
}

/**
 * verify a Mulh node
 */
static int verify_node_Mulh(const ir_node *n)
{
	ir_mode *mymode  = get_irn_mode(n);
	ir_mode *op1mode = get_irn_mode(get_Mulh_left(n));
	ir_mode *op2mode = get_irn_mode(get_Mulh_right(n));

	ASSERT_AND_RET_DBG(
		(
			/* Mulh: BB x int x int --> int */
			(mode_is_int(op1mode) && op2mode == op1mode && op1mode == mymode)
		),
		"Mulh node",0,
		show_binop_failure(n, "/* Mulh: BB x int x int --> int */");
	);
	return 1;
}

/**
 * verify a Div node
 */
static int verify_node_Div(const ir_node *n)
{
	ir_mode *mymode  = get_irn_mode(n);
	ir_mode *op1mode = get_irn_mode(get_Div_mem(n));
	ir_mode *op2mode = get_irn_mode(get_Div_left(n));
	ir_mode *op3mode = get_irn_mode(get_Div_right(n));

	ASSERT_AND_RET(
		/* Div: BB x M x data x data --> M x X x data */
		op1mode == mode_M &&
		op2mode == op3mode &&
		mode_is_data(op2mode) &&
		mymode == mode_T,
		"Div node", 0
		);
	return 1;
}

/**
 * verify a Mod node
 */
static int verify_node_Mod(const ir_node *n)
{
	ir_mode *mymode  = get_irn_mode(n);
	ir_mode *op1mode = get_irn_mode(get_Mod_mem(n));
	ir_mode *op2mode = get_irn_mode(get_Mod_left(n));
	ir_mode *op3mode = get_irn_mode(get_Mod_right(n));

	ASSERT_AND_RET(
		/* Mod: BB x M x int x int --> M x X x int */
		op1mode == mode_M &&
		op2mode == op3mode &&
		mode_is_int(op2mode) &&
		mymode == mode_T,
		"Mod node", 0
		);
	return 1;
}

/**
 * verify a logical And, Or, Eor node
 */
static int verify_node_Logic(const ir_node *n)
{
	ir_mode *mymode  = get_irn_mode(n);
	ir_mode *op1mode = get_irn_mode(get_binop_left(n));
	ir_mode *op2mode = get_irn_mode(get_binop_right(n));

	ASSERT_AND_RET_DBG(
		/* And or Or or Eor: BB x int x int --> int */
		(mode_is_int(mymode) || mode_is_reference(mymode) || mymode == mode_b) &&
		op2mode == op1mode &&
		mymode == op2mode,
		"And, Or or Eor node", 0,
		show_binop_failure(n, "/* And or Or or Eor: BB x int x int --> int */");
	);
	return 1;
}

static int verify_node_And(const ir_node *n)
{
	return verify_node_Logic(n);
}

static int verify_node_Or(const ir_node *n)
{
	return verify_node_Logic(n);
}

static int verify_node_Eor(const ir_node *n)
{
	return verify_node_Logic(n);
}

/**
 * verify a Not node
 */
static int verify_node_Not(const ir_node *n)
{
	ir_mode *mymode  = get_irn_mode(n);
	ir_mode *op1mode = get_irn_mode(get_Not_op(n));

	ASSERT_AND_RET_DBG(
		/* Not: BB x int --> int */
		(mode_is_int(mymode) || mymode == mode_b) &&
		mymode == op1mode,
		"Not node", 0,
		show_unop_failure(n, "/* Not: BB x int --> int */");
	);
	return 1;
}

/**
 * verify a Cmp node
 */
static int verify_node_Cmp(const ir_node *n)
{
	ir_mode *mymode  = get_irn_mode(n);
	ir_mode *op1mode = get_irn_mode(get_Cmp_left(n));
	ir_mode *op2mode = get_irn_mode(get_Cmp_right(n));

	ASSERT_AND_RET_DBG(
		/* Cmp: BB x datab x datab --> b16 */
		mode_is_datab(op1mode) &&
		op2mode == op1mode &&
		mymode == mode_b,
		"Cmp node", 0,
		show_binop_failure(n, "/* Cmp: BB x datab x datab --> b16 */");
	);
	return 1;
}

/**
 * verify a Shift node
 */
static int verify_node_Shift(const ir_node *n)
{
	ir_mode *mymode  = get_irn_mode(n);
	ir_mode *op1mode = get_irn_mode(get_binop_left(n));
	ir_mode *op2mode = get_irn_mode(get_binop_right(n));

	ASSERT_AND_RET_DBG(
		/* Shl, Shr or Shrs: BB x int x int_u --> int */
		mode_is_int(op1mode) &&
		mode_is_int(op2mode) &&
		!mode_is_signed(op2mode) &&
		mymode == op1mode,
		"Shl, Shr or Shrs node", 0,
		show_binop_failure(n, "/* Shl, Shr or Shrs: BB x int x int_u --> int */");
	);
	return 1;
}

static int verify_node_Shl(const ir_node *n)
{
	return verify_node_Shift(n);
}

static int verify_node_Shr(const ir_node *n)
{
	return verify_node_Shift(n);
}

static int verify_node_Shrs(const ir_node *n)
{
	return verify_node_Shift(n);
}

/**
 * verify a Rotl node
 */
static int verify_node_Rotl(const ir_node *n)
{
	ir_mode *mymode  = get_irn_mode(n);
	ir_mode *op1mode = get_irn_mode(get_Rotl_left(n));
	ir_mode *op2mode = get_irn_mode(get_Rotl_right(n));

	ASSERT_AND_RET_DBG(
		/* Rotl: BB x int x int --> int */
		mode_is_int(op1mode) &&
		mode_is_int(op2mode) &&
		mymode == op1mode,
		"Rotl node", 0,
		show_binop_failure(n, "/* Rotl: BB x int x int --> int */");
	);
	return 1;
}

/**
 * verify a Conv node
 */
static int verify_node_Conv(const ir_node *n)
{
	ir_mode *mymode  = get_irn_mode(n);
	ir_mode *op1mode = get_irn_mode(get_Conv_op(n));

	ASSERT_AND_RET_DBG(mode_is_datab(op1mode) && mode_is_data(mymode),
		"Conv node", 0,
		show_unop_failure(n, "/* Conv: BB x datab --> data */");
	);
	return 1;
}

/**
 * verify a Cast node
 */
static int verify_node_Cast(const ir_node *n)
{
	ir_mode *mymode  = get_irn_mode(n);
	ir_mode *op1mode = get_irn_mode(get_Cast_op(n));

	ASSERT_AND_RET_DBG(
		/* Conv: BB x datab1 --> datab2 */
		mode_is_data(op1mode) && op1mode == mymode,
		"Cast node", 0,
		show_unop_failure(n, "/* Conv: BB x datab1 --> datab2 */");
	);
	return 1;
}

/**
 * verify a Phi node
 */
static int verify_node_Phi(const ir_node *n)
{
	ir_mode *mymode = get_irn_mode(n);
	ir_node *block  = get_nodes_block(n);
	int i;

	/* a Phi node MUST have the same number of inputs as its block
	 * Exception is a phi with 0 inputs which is used when (re)constructing the
	 * SSA form */
	if (! is_Bad(block) && get_irg_phase_state(get_irn_irg(n)) != phase_building && get_irn_arity(n) > 0) {
		ASSERT_AND_RET_DBG(
			get_irn_arity(n) == get_irn_arity(block),
			"wrong number of inputs in Phi node", 0,
			show_phi_inputs(n, block);
		);
	}

	/* Phi: BB x dataM^n --> dataM */
	for (i = get_Phi_n_preds(n) - 1; i >= 0; --i) {
		ir_node *pred = get_Phi_pred(n, i);
		ASSERT_AND_RET_DBG(get_irn_mode(pred) == mymode,
		                   "Phi node", 0, show_phi_failure(n, pred, i);
		);
	}
	ASSERT_AND_RET(mode_is_dataM(mymode) || mymode == mode_b, "Phi node", 0 );

	return 1;
}

/**
 * verify a Load node
 */
static int verify_node_Load(const ir_node *n)
{
	ir_graph *irg     = get_irn_irg(n);
	ir_mode  *mymode  = get_irn_mode(n);
	ir_mode  *op1mode = get_irn_mode(get_Load_mem(n));
	ir_mode  *op2mode = get_irn_mode(get_Load_ptr(n));

	ASSERT_AND_RET(op1mode == mode_M, "Load node", 0);
	if (get_irg_phase_state(irg) != phase_backend) {
		ASSERT_AND_RET(mode_is_reference(op2mode), "Load node", 0 );
	}
	ASSERT_AND_RET( mymode == mode_T, "Load node", 0 );

	/*
	 * jack's gen_add_firm_code:simpleSel seems to build Load (Load
	 * (Proj (Proj))) sometimes ...

	 * interprete.c:ai_eval seems to assume that this happens, too

	 * obset.c:get_abstval_any can't deal with this if the load has
	 * mode_T
	 *
	  {
	  ir_entity *ent = hunt_for_entity (get_Load_ptr (n), n);
	  assert ((NULL != ent) || (mymode != mode_T));
	  }
	 */

	return 1;
}

/**
 * verify a Store node
 */
static int verify_node_Store(const ir_node *n)
{
	ir_graph  *irg = get_irn_irg(n);
	ir_entity *target;

	ir_mode *mymode  = get_irn_mode(n);
	ir_mode *op1mode = get_irn_mode(get_Store_mem(n));
	ir_mode *op2mode = get_irn_mode(get_Store_ptr(n));
	ir_mode *op3mode = get_irn_mode(get_Store_value(n));

	ASSERT_AND_RET(op1mode == mode_M && mode_is_datab(op3mode), "Store node", 0 );
	if (get_irg_phase_state(irg) != phase_backend) {
		ASSERT_AND_RET(mode_is_reference(op2mode), "Store node", 0 );
	}
	ASSERT_AND_RET(mymode == mode_T, "Store node", 0);

	target = get_ptr_entity(get_Store_ptr(n));
	if (verify_entities && target && get_irg_phase_state(irg) == phase_high) {
		/*
		 * If lowered code, any Sels that add 0 may be removed, causing
		 * an direct access to entities of array or compound type.
		 * Prevent this by checking the phase.
		 */
		ASSERT_AND_RET( op3mode == get_type_mode(get_entity_type(target)),
			"Store node", 0);
	}

	return 1;
}

/**
 * verify an Alloc node
 */
static int verify_node_Alloc(const ir_node *n)
{
	ir_mode *mymode  = get_irn_mode(n);
	ir_mode *op1mode = get_irn_mode(get_Alloc_mem(n));
	ir_mode *op2mode = get_irn_mode(get_Alloc_count(n));

	ASSERT_AND_RET_DBG(
		/* Alloc: BB x M x int_u --> M x X x ref */
		op1mode == mode_M &&
		mode_is_int(op2mode) &&
		!mode_is_signed(op2mode) &&
		mymode == mode_T,
		"Alloc node", 0,
		show_node_failure(n);
	);
	return 1;
}

/**
 * verify a Free node
 */
static int verify_node_Free(const ir_node *n)
{
	ir_mode *mymode  = get_irn_mode(n);
	ir_mode *op1mode = get_irn_mode(get_Free_mem(n));
	ir_mode *op2mode = get_irn_mode(get_Free_ptr(n));
	ir_mode *op3mode = get_irn_mode(get_Free_count(n));

	ASSERT_AND_RET_DBG(
		/* Free: BB x M x ref x int_u --> M */
		op1mode == mode_M && mode_is_reference(op2mode) &&
		mode_is_int(op3mode) &&
		!mode_is_signed(op3mode) &&
		mymode == mode_M,
		"Free node", 0,
		show_triop_failure(n, "/* Free: BB x M x ref x int_u --> M */");
	);
	return 1;
}

/**
 * verify a Sync node
 */
static int verify_node_Sync(const ir_node *n)
{
	int i;
	ir_mode *mymode  = get_irn_mode(n);

	/* Sync: BB x M^n --> M */
	for (i = get_Sync_n_preds(n) - 1; i >= 0; --i) {
		ASSERT_AND_RET( get_irn_mode(get_Sync_pred(n, i)) == mode_M, "Sync node", 0 );
	}
	ASSERT_AND_RET( mymode == mode_M, "Sync node", 0 );
	return 1;
}

/**
 * verify a Confirm node
 */
static int verify_node_Confirm(const ir_node *n)
{
	ir_mode *mymode  = get_irn_mode(n);
	ir_mode *op1mode = get_irn_mode(get_Confirm_value(n));
	ir_mode *op2mode = get_irn_mode(get_Confirm_bound(n));

	ASSERT_AND_RET_DBG(
		/* Confirm: BB x T x T --> T */
		op1mode == mymode &&
		op2mode == mymode,
		"Confirm node", 0,
		show_binop_failure(n, "/* Confirm: BB x T x T --> T */");
	);
	return 1;
}

/**
 * verify a Mux node
 */
static int verify_node_Mux(const ir_node *n)
{
	ir_mode *mymode  = get_irn_mode(n);
	ir_mode *op1mode = get_irn_mode(get_Mux_sel(n));
	ir_mode *op2mode = get_irn_mode(get_Mux_true(n));
	ir_mode *op3mode = get_irn_mode(get_Mux_false(n));

	ASSERT_AND_RET(
		/* Mux: BB x b x datab x datab --> datab */
		op1mode == mode_b &&
		op2mode == mymode &&
		op3mode == mymode &&
		mode_is_datab(mymode),
		"Mux node", 0
		);
	return 1;
}

/**
 * verify a CopyB node
 */
static int verify_node_CopyB(const ir_node *n)
{
	ir_graph *irg     = get_irn_irg(n);
	ir_mode  *mymode  = get_irn_mode(n);
	ir_mode  *op1mode = get_irn_mode(get_CopyB_mem(n));
	ir_mode  *op2mode = get_irn_mode(get_CopyB_dst(n));
	ir_mode  *op3mode = get_irn_mode(get_CopyB_src(n));
	ir_type  *t = get_CopyB_type(n);

	/* CopyB: BB x M x ref x ref --> M x X */
	ASSERT_AND_RET(mymode == mode_T && op1mode == mode_M, "CopyB node", 0);
	if (get_irg_phase_state(irg) != phase_backend) {
		ASSERT_AND_RET(mode_is_reference(op2mode) && mode_is_reference(op3mode),
			"CopyB node", 0 );
	}

	ASSERT_AND_RET(
		is_compound_type(t) || is_Array_type(t),
		"CopyB node should copy compound types only", 0 );

	/* NoMem nodes are only allowed as memory input if the CopyB is NOT pinned.
	   This should happen RARELY, as CopyB COPIES MEMORY */
	ASSERT_AND_RET(verify_right_pinned(n), "CopyB node with wrong memory input", 0 );
	return 1;
}

/**
 * verify a Bound node
 */
static int verify_node_Bound(const ir_node *n)
{
	ir_mode *mymode  = get_irn_mode(n);
	ir_mode *op1mode = get_irn_mode(get_Bound_mem(n));
	ir_mode *op2mode = get_irn_mode(get_Bound_index(n));
	ir_mode *op3mode = get_irn_mode(get_Bound_lower(n));
	ir_mode *op4mode = get_irn_mode(get_Bound_upper(n));

	/* Bound: BB x M x int x int x int --> M x X */
	ASSERT_AND_RET(
		mymode == mode_T &&
		op1mode == mode_M &&
		op2mode == op3mode &&
		op3mode == op4mode &&
		mode_is_int(op3mode),
		"Bound node", 0 );
	return 1;
}

/**
 * Check dominance.
 * For each usage of a node, it is checked, if the block of the
 * node dominates the block of the usage (for phis: the predecessor
 * block of the phi for the corresponding edge).
 *
 * @return non-zero on success, 0 on dominance error
 */
static int check_dominance_for_node(const ir_node *use)
{
	/* This won't work for blocks and the end node */
	if (!is_Block(use) && !is_End(use) && !is_Anchor(use)) {
		int i;
		ir_node *bl = get_nodes_block(use);

		for (i = get_irn_arity(use) - 1; i >= 0; --i) {
			ir_node  *def    = get_irn_n(use, i);
			ir_node  *def_bl = get_nodes_block(def);
			ir_node  *use_bl = bl;
			ir_graph *irg;

			/* we have no dominance relation for unreachable blocks, so we can't
			 * check the dominance property there */
			if (!is_Block(def_bl) || get_Block_dom_depth(def_bl) == -1)
				continue;

			if (is_Phi(use)) {
				if (is_Bad(def))
					continue;
				use_bl = get_Block_cfgpred_block(bl, i);
			}

			if (!is_Block(use_bl) || get_Block_dom_depth(use_bl) == -1)
				continue;

			irg = get_irn_irg(use);
			ASSERT_AND_RET_DBG(
				block_dominates(def_bl, use_bl),
				"the definition of a value used violates the dominance property", 0,
				ir_fprintf(stderr,
				"graph %+F: %+F of %+F must dominate %+F of user %+F input %d\n",
				irg, def_bl, def, use_bl, use, i
				);
			);
		}
	}
	return 1;
}

/* Tests the modes of n and its predecessors. */
int irn_verify_irg(const ir_node *n, ir_graph *irg)
{
	ir_op *op;

	if (!get_node_verification_mode())
		return 1;

	/*
	 * do NOT check placement in interprocedural view, as we don't always
	 * know the "right" graph ...
	 */

#ifndef NDEBUG
	/* this is an expensive check for large graphs (it has a quadratic
	 * runtime but with a small constant); so do NOT run it in release mode
	 */
	ASSERT_AND_RET_DBG(
		node_is_in_irgs_storage(irg, n),
		"Node is not stored on proper IR graph!", 0,
		show_node_on_graph(irg, n);
	);
#endif
	assert(get_irn_irg(n) == irg);
	{
		unsigned idx           = get_irn_idx(n);
		ir_node *node_from_map = get_idx_irn(irg, idx);
		ASSERT_AND_RET_DBG(node_from_map == n, "Node index and index map entry differ", 0,
			ir_printf("node %+F node in map %+F(%p)\n", n, node_from_map, node_from_map);
		);
	}

	op = get_irn_op(n);

	if (get_op_pinned(op) >= op_pin_state_exc_pinned) {
		op_pin_state state = get_irn_pinned(n);
		ASSERT_AND_RET_DBG(
			state == op_pin_state_floats ||
			state == op_pin_state_pinned,
			"invalid pin state", 0,
			ir_printf("node %+F", n);
		);
	} else if (!is_Block(n) && is_irn_pinned_in_irg(n)
	           && is_irg_state(irg, IR_GRAPH_STATE_NO_BADS)) {
		ASSERT_AND_RET_DBG(is_Block(get_nodes_block(n)) || is_Anchor(n),
				"block input is not a block", 0,
				ir_printf("node %+F", n);
		);
	}

	if (op->ops.verify_node)
		return op->ops.verify_node(n);

	/* All went ok */
	return 1;
}

int irn_verify(const ir_node *n)
{
#ifdef DEBUG_libfirm
	return irn_verify_irg(n, get_irn_irg(n));
#else
	(void)n;
	return 1;
#endif
}

/*-----------------------------------------------------------------*/
/* Verify the whole graph.                                         */
/*-----------------------------------------------------------------*/

#ifdef DEBUG_libfirm
/**
 * Walker to check every node
 */
static void verify_wrap(ir_node *node, void *env)
{
	int *res = (int*)env;
	*res = irn_verify_irg(node, get_irn_irg(node));
}

/**
 * Walker to check every node including SSA property.
 * Only called if dominance info is available.
 */
static void verify_wrap_ssa(ir_node *node, void *env)
{
	int *res = (int*)env;

	*res = irn_verify_irg(node, get_irn_irg(node));
	if (*res) {
		*res = check_dominance_for_node(node);
	}
}

#endif /* DEBUG_libfirm */

typedef struct check_cfg_env_t {
	pmap *branch_nodes; /**< map blocks to their branching nodes,
	                         map mode_X nodes to the blocks they branch to */
	int   res;
	ir_nodeset_t reachable_blocks;
	ir_nodeset_t kept_nodes;
	ir_nodeset_t true_projs;
	ir_nodeset_t false_projs;
} check_cfg_env_t;

static int check_block_cfg(const ir_node *block, check_cfg_env_t *env)
{
	pmap *branch_nodes;
	int   n_cfgpreds;
	int   i;

	ASSERT_AND_RET_DBG(ir_nodeset_contains(&env->reachable_blocks, block),
	                   "Block is not reachable by blockwalker (endless loop with no kept block?)", 0,
	                   ir_printf("block %+F\n", block);
	);

	n_cfgpreds   = get_Block_n_cfgpreds(block);
	branch_nodes = env->branch_nodes;
	for (i = 0; i < n_cfgpreds; ++i) {
		/* check that each mode_X node is only connected
		 * to 1 user */
		ir_node *branch = get_Block_cfgpred(block, i);
		ir_node *former_dest;
		ir_node *former_branch;
		ir_node *branch_proj;
		ir_node *branch_block;
		branch = skip_Tuple(branch);
		if (is_Bad(branch))
			continue;
		former_dest = pmap_get(branch_nodes, branch);
		ASSERT_AND_RET_DBG(former_dest==NULL || is_unknown_jump(skip_Proj(branch)),
						   "Multiple users on mode_X node", 0,
						   ir_printf("node %+F\n", branch);
		);
		pmap_insert(branch_nodes, branch, (void*)block);

		/* check that there's only 1 branching instruction in each block */
		branch_block = get_nodes_block(branch);
		branch_proj  = branch;
		if (is_Proj(branch)) {
			branch = skip_Proj(branch);
		}
		former_branch = pmap_get(branch_nodes, branch_block);

		ASSERT_AND_RET_DBG(former_branch == NULL || former_branch == branch,
						   "Multiple branching nodes in a block", 0,
						   ir_printf("nodes %+F,%+F in block %+F\n",
									 branch, former_branch, branch_block);
		);
		pmap_insert(branch_nodes, branch_block, branch);

		if (is_Cond(branch)) {
			long pn = get_Proj_proj(branch_proj);
			if (pn == pn_Cond_true)
				ir_nodeset_insert(&env->true_projs, branch);
			if (pn == pn_Cond_false)
				ir_nodeset_insert(&env->false_projs, branch);
		} else if (is_Switch(branch)) {
			long pn = get_Proj_proj(branch_proj);
			if (pn == pn_Switch_default)
				ir_nodeset_insert(&env->true_projs, branch);
		}
	}

	return 1;
}

static void check_cfg_walk_func(ir_node *node, void *data)
{
	check_cfg_env_t *env = (check_cfg_env_t*)data;
	if (!is_Block(node))
		return;
	env->res &= check_block_cfg(node, env);
}

static int verify_block_branch(const ir_node *block, check_cfg_env_t *env)
{
	ir_node *branch = pmap_get(env->branch_nodes, block);
	ASSERT_AND_RET_DBG(branch != NULL
	                   || ir_nodeset_contains(&env->kept_nodes, block)
	                   || block == get_irg_end_block(get_irn_irg(block)),
	                   "block contains no cfop", 0,
	                   ir_printf("block %+F\n", block);
	);
	return 1;
}

static int verify_cond_projs(const ir_node *cond, check_cfg_env_t *env)
{
	ASSERT_AND_RET_DBG(ir_nodeset_contains(&env->true_projs, cond),
					   "Cond node lacks true proj", 0,
					   ir_printf("Cond %+F\n", cond);
	);
	ASSERT_AND_RET_DBG(ir_nodeset_contains(&env->false_projs, cond),
					   "Cond node lacks false proj", 0,
					   ir_printf("Cond %+F\n", cond);
	);
	return 1;
}

static int verify_switch_projs(const ir_node *sw, check_cfg_env_t *env)
{
	ASSERT_AND_RET_DBG(ir_nodeset_contains(&env->true_projs, sw),
					   "Switch node lacks default Proj", 0,
					   ir_printf("Switch %+F\n", sw);
	);
	return 1;
}

static void assert_branch(ir_node *node, void *data)
{
	check_cfg_env_t *env = (check_cfg_env_t*)data;
	if (is_Block(node)) {
		env->res &= verify_block_branch(node, env);
	} else if (is_Cond(node)) {
		env->res &= verify_cond_projs(node, env);
	} else if (is_Switch(node)) {
		env->res &= verify_switch_projs(node, env);
	}
}

static void collect_reachable_blocks(ir_node *block, void *data)
{
	ir_nodeset_t *reachable_blocks = (ir_nodeset_t*) data;
	ir_nodeset_insert(reachable_blocks, block);
}

/**
 * Checks CFG well-formedness
 */
static int check_cfg(ir_graph *irg)
{
	check_cfg_env_t env;
	env.branch_nodes = pmap_create(); /**< map blocks to branch nodes */
	env.res          = 1;
	ir_nodeset_init(&env.reachable_blocks);
	ir_nodeset_init(&env.true_projs);
	ir_nodeset_init(&env.false_projs);

	irg_block_walk_graph(irg, collect_reachable_blocks, NULL,
	                     &env.reachable_blocks);

	/* note that we do not use irg_walk_block because it will miss these
	 * invalid blocks without a jump instruction which we want to detect
	 * here */
	irg_walk_graph(irg, check_cfg_walk_func, NULL, &env);

	ir_nodeset_init(&env.kept_nodes);
	{
		ir_node *end   = get_irg_end(irg);
		int      arity = get_irn_arity(end);
		int      i;
		for (i = 0; i < arity; ++i) {
			ir_node *n = get_irn_n(end, i);
			ir_nodeset_insert(&env.kept_nodes, n);
		}
	}
	irg_walk_graph(irg, assert_branch, NULL, &env);

	ir_nodeset_destroy(&env.false_projs);
	ir_nodeset_destroy(&env.true_projs);
	ir_nodeset_destroy(&env.kept_nodes);
	ir_nodeset_destroy(&env.reachable_blocks);
	pmap_destroy(env.branch_nodes);
	return env.res;
}

/*
 * Calls irn_verify for each node in irg.
 * Graph must be in state "op_pin_state_pinned".
 * If dominance info is available, check the SSA property.
 */
int irg_verify(ir_graph *irg, unsigned flags)
{
	int res = 1;
#ifdef DEBUG_libfirm
	int pinned = get_irg_pinned(irg) == op_pin_state_pinned;

#ifndef NDEBUG
	last_irg_error = NULL;
#endif /* NDEBUG */

	if (pinned && !check_cfg(irg))
		res = 0;

	if (res == 1 && (flags & VERIFY_ENFORCE_SSA) && pinned)
		compute_doms(irg);

	irg_walk_anchors(
		irg,
		pinned && is_irg_state(irg, IR_GRAPH_STATE_CONSISTENT_DOMINANCE)
			? verify_wrap_ssa : verify_wrap,
		NULL,
		&res
	);

	if (get_node_verification_mode() == FIRM_VERIFICATION_REPORT && ! res) {
		ir_entity *ent = get_irg_entity(irg);

		if (ent)
			fprintf(stderr, "irg_verify: Verifying graph %s failed\n", get_entity_name(ent));
		else
			fprintf(stderr, "irg_verify: Verifying graph %p failed\n", (void *)irg);
	}

#else
	(void)irg;
	(void)flags;
#endif /* DEBUG_libfirm */

	return res;
}

typedef struct pass_t {
	ir_graph_pass_t pass;
	unsigned        flags;
} pass_t;

/**
 * Wrapper to irg_verify to be run as an ir_graph pass.
 */
static int irg_verify_wrapper(ir_graph *irg, void *context)
{
	pass_t *pass = (pass_t*)context;
	irg_verify(irg, pass->flags);
	/* do NOT rerun the pass if verify is ok :-) */
	return 0;
}

/* Creates an ir_graph pass for irg_verify(). */
ir_graph_pass_t *irg_verify_pass(const char *name, unsigned flags)
{
	pass_t *pass = XMALLOCZ(pass_t);

	def_graph_pass_constructor(
		&pass->pass, name ? name : "irg_verify", irg_verify_wrapper);

	/* neither dump for verify */
	pass->pass.dump_irg   = (DUMP_ON_IRG_FUNC)ir_prog_no_dump;
	pass->pass.verify_irg = (RUN_ON_IRG_FUNC)ir_prog_no_verify;

	pass->flags = flags;
	return &pass->pass;
}

/* create a verify pass */
int irn_verify_irg_dump(const ir_node *n, ir_graph *irg,
                        const char **bad_string)
{
	int res;
	firm_verification_t old = get_node_verification_mode();

	firm_verify_failure_msg = NULL;
	do_node_verification(FIRM_VERIFICATION_ERROR_ONLY);
	res = irn_verify_irg(n, irg);
	if (res && is_irg_state(irg, IR_GRAPH_STATE_CONSISTENT_DOMINANCE) &&
	    get_irg_pinned(irg) == op_pin_state_pinned)
		res = check_dominance_for_node(n);
	do_node_verification(old);
	*bad_string = firm_verify_failure_msg;

	return res;
}

typedef struct verify_bad_env_t {
	int flags;
	int res;
} verify_bad_env_t;

/**
 * Pre-Walker: check Bad predecessors of node.
 */
static void check_bads(ir_node *node, void *env)
{
	verify_bad_env_t *venv = (verify_bad_env_t*)env;
	int i, arity = get_irn_arity(node);
	ir_graph *irg = get_irn_irg(node);

	if (is_Block(node)) {
		if ((venv->flags & BAD_CF) == 0) {

			/* check for Bad Block predecessor */
			for (i = 0; i < arity; ++i) {
				ir_node *pred = get_irn_n(node, i);

				if (is_Bad(pred)) {
					venv->res |= BAD_CF;

					if (get_node_verification_mode() == FIRM_VERIFICATION_REPORT) {
						fprintf(stderr, "irg_verify_bads: Block %ld has Bad predecessor\n", get_irn_node_nr(node));
					}
					if (get_node_verification_mode() == FIRM_VERIFICATION_ON) {
						dump_ir_graph(irg, "assert");
						assert(0 && "Bad CF detected");
					}
				}
			}
		}
	} else {
		if ((venv->flags & BAD_BLOCK) == 0) {

			/* check for Bad Block */
			if (is_Bad(get_nodes_block(node))) {
				venv->res |= BAD_BLOCK;

				if (get_node_verification_mode() == FIRM_VERIFICATION_REPORT) {
					fprintf(stderr, "irg_verify_bads: node %ld has Bad Block\n", get_irn_node_nr(node));
				}
				if (get_node_verification_mode() == FIRM_VERIFICATION_ON) {
					dump_ir_graph(irg, "assert");
					assert(0 && "Bad CF detected");
				}
			}
		}

		if ((venv->flags & TUPLE) == 0) {
			if (is_Tuple(node)) {
				venv->res |= TUPLE;

				if (get_node_verification_mode() == FIRM_VERIFICATION_REPORT) {
					fprintf(stderr, "irg_verify_bads: node %ld is a Tuple\n", get_irn_node_nr(node));
				}
				if (get_node_verification_mode() == FIRM_VERIFICATION_ON) {
					dump_ir_graph(irg, "assert");
					assert(0 && "Tuple detected");
				}
			}
		}

		for (i = 0; i < arity; ++i) {
			ir_node *pred = get_irn_n(node, i);

			if (is_Bad(pred)) {
				/* check for Phi with Bad inputs */
				if (is_Phi(node) && !is_Bad(get_nodes_block(node)) && is_Bad(get_irn_n(get_nodes_block(node), i))) {
					if (venv->flags & BAD_CF)
						continue;
					else {
						venv->res |= BAD_CF;

						if (get_node_verification_mode() == FIRM_VERIFICATION_REPORT) {
							fprintf(stderr, "irg_verify_bads: Phi %ld has Bad Input\n", get_irn_node_nr(node));
						}
						if (get_node_verification_mode() == FIRM_VERIFICATION_ON) {
							dump_ir_graph(irg, "assert");
							assert(0 && "Bad CF detected");
						}
					}
				}

				/* Bad node input */
				if ((venv->flags & BAD_DF) == 0) {
					venv->res |= BAD_DF;

					if (get_node_verification_mode() == FIRM_VERIFICATION_REPORT) {
						fprintf(stderr, "irg_verify_bads: node %ld has Bad Input\n", get_irn_node_nr(node));
					}
					if (get_node_verification_mode() == FIRM_VERIFICATION_ON) {
						dump_ir_graph(irg, "assert");
						assert(0 && "Bad NON-CF detected");
					}
				}
			}
		}
	}
}

/*
 * verify occurrence of bad nodes
 */
int irg_verify_bads(ir_graph *irg, int flags)
{
	verify_bad_env_t env;

	env.flags = flags;
	env.res   = 0;

	irg_walk_graph(irg, check_bads, NULL, &env);

	return env.res;
}

/*
 * set the default verify operation
 */
void firm_set_default_verifier(unsigned code, ir_op_ops *ops)
{
#define CASE(a)                           \
   case iro_##a:                          \
     ops->verify_node  = verify_node_##a; \
     break

	switch (code) {
	CASE(Add);
	CASE(Alloc);
	CASE(And);
	CASE(Block);
	CASE(Bound);
	CASE(Call);
	CASE(Cast);
	CASE(Cmp);
	CASE(Cond);
	CASE(Confirm);
	CASE(Const);
	CASE(Conv);
	CASE(CopyB);
	CASE(Div);
	CASE(Eor);
	CASE(Free);
	CASE(IJmp);
	CASE(InstOf);
	CASE(Jmp);
	CASE(Load);
	CASE(Minus);
	CASE(Mod);
	CASE(Mul);
	CASE(Mulh);
	CASE(Mux);
	CASE(Not);
	CASE(Or);
	CASE(Phi);
	CASE(Proj);
	CASE(Raise);
	CASE(Return);
	CASE(Rotl);
	CASE(Sel);
	CASE(Shl);
	CASE(Shr);
	CASE(Shrs);
	CASE(Start);
	CASE(Store);
	CASE(Sub);
	CASE(Switch);
	CASE(SymConst);
	CASE(Sync);
	default:
		break;
	}
#undef CASE

#define CASE(a)                          \
   case iro_##a:                         \
     ops->verify_proj_node  = verify_node_Proj_##a; \
     break

	switch (code) {
	CASE(Alloc);
	CASE(Bound);
	CASE(Call);
	CASE(Cond);
	CASE(CopyB);
	CASE(Div);
	CASE(InstOf);
	CASE(Load);
	CASE(Mod);
	CASE(Proj);
	CASE(Raise);
	CASE(Start);
	CASE(Store);
	CASE(Switch);
	CASE(Tuple);
	default:
		break;
	}
#undef CASE
}
