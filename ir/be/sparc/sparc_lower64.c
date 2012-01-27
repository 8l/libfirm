/*
 * Copyright (C) 1995-2010 University of Karlsruhe.  All right reserved.
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
 * @brief    Sparc 64bit lowering
 * @author   Matthias Braun
 */
#include "config.h"

#include "bearch_sparc_t.h"
#include "gen_sparc_new_nodes.h"
#include "lower_dw.h"
#include "ircons_t.h"
#include "util.h"

static void lower64_add(ir_node *node, ir_mode *mode)
{
	dbg_info *dbgi       = get_irn_dbg_info(node);
	ir_node  *block      = get_nodes_block(node);
	ir_node  *left       = get_Add_left(node);
	ir_node  *right      = get_Add_right(node);
	ir_node  *left_low   = get_lowered_low(left);
	ir_node  *left_high  = get_lowered_high(left);
	ir_node  *right_low  = get_lowered_low(right);
	ir_node  *right_high = get_lowered_high(right);
	ir_node  *addcc      = new_bd_sparc_AddCC_t(dbgi, block, left_low,
	                                            right_low);
	ir_node  *res_low    = new_r_Proj(addcc, mode_Iu, pn_sparc_AddCC_t_res);
	ir_node  *res_flags  = new_r_Proj(addcc, mode_ANY, pn_sparc_AddCC_t_flags);
	ir_node  *addx       = new_bd_sparc_AddX_t(dbgi, block, left_high,
	                                           right_high, res_flags, mode);
	ir_set_dw_lowered(node, res_low, addx);
}

static void lower64_sub(ir_node *node, ir_mode *mode)
{
	dbg_info *dbgi       = get_irn_dbg_info(node);
	ir_node  *block      = get_nodes_block(node);
	ir_node  *left       = get_Sub_left(node);
	ir_node  *right      = get_Sub_right(node);
	ir_node  *left_low   = get_lowered_low(left);
	ir_node  *left_high  = get_lowered_high(left);
	ir_node  *right_low  = get_lowered_low(right);
	ir_node  *right_high = get_lowered_high(right);
	ir_node  *subcc      = new_bd_sparc_SubCC_t(dbgi, block, left_low,
	                                            right_low);
	ir_node  *res_low    = new_r_Proj(subcc, mode_Iu, pn_sparc_SubCC_t_res);
	ir_node  *res_flags  = new_r_Proj(subcc, mode_ANY, pn_sparc_SubCC_t_flags);
	ir_node  *subx       = new_bd_sparc_SubX_t(dbgi, block, left_high,
	                                           right_high, res_flags, mode);
	ir_set_dw_lowered(node, res_low, subx);
}

static void lower64_minus(ir_node *node, ir_mode *mode)
{
	dbg_info *dbgi         = get_irn_dbg_info(node);
	ir_graph *irg          = get_irn_irg(node);
	ir_node  *block        = get_nodes_block(node);
	ir_node  *op           = get_Minus_op(node);
	ir_node  *right_low    = get_lowered_low(op);
	ir_node  *right_high   = get_lowered_high(op);
	ir_mode  *low_unsigned = get_irn_mode(right_low);
	ir_node  *left_low     = new_r_Const(irg, get_mode_null(low_unsigned));
	ir_node  *left_high    = new_r_Const(irg, get_mode_null(mode));
	ir_node  *subcc        = new_bd_sparc_SubCC_t(dbgi, block, left_low,
	                                              right_low);
	ir_node  *res_low      = new_r_Proj(subcc, mode_Iu, pn_sparc_SubCC_t_res);
	ir_node  *res_flags    = new_r_Proj(subcc, mode_ANY, pn_sparc_SubCC_t_flags);
	ir_node  *subx         = new_bd_sparc_SubX_t(dbgi, block, left_high,
	                                             right_high, res_flags, mode);
	ir_set_dw_lowered(node, res_low, subx);
}

static ir_entity *create_64_intrinsic_fkt(ir_type *method, const ir_op *op,
                                          const ir_mode *imode,
                                          const ir_mode *omode, void *context)
{
	ir_type    *glob = get_glob_type();
	const char *name;
	ident      *id;
	ir_entity  *result;
	(void) context;
	(void) omode;

	if (op == op_Mul) {
		name = "__muldi3";
	} else if (op == op_Div) {
		name = mode_is_signed(imode) ? "__divdi3" : "__udivdi3";
	} else if (op == op_Mod) {
		name = mode_is_signed(imode) ? "__moddi3" : "__umoddi3";
	} else if (op == op_Conv) {
		if (mode_is_float(imode)) {
			assert(get_mode_size_bits(omode) == 64);
			if (get_mode_size_bits(imode) == 64) {
				name = mode_is_signed(omode) ? "__fixdfdi" : "__fixunsdfdi";
			} else if (get_mode_size_bits(imode) == 32) {
				name = mode_is_signed(omode) ? "__fixsfdi" : "__fixunssfdi";
			} else {
				assert(get_mode_size_bits(imode) == 128);
				panic("can't conver long double to long long yet");
			}
		} else if (mode_is_float(omode)) {
			assert(get_mode_size_bits(imode) == 64);
			if (get_mode_size_bits(omode) == 64) {
				name = mode_is_signed(imode) ? "__floatdidf" : "__floatundidf";
			} else if (get_mode_size_bits(omode) == 32) {
				name = mode_is_signed(imode) ? "__floatdisf" : "__floatundisf";
			} else {
				assert(get_mode_size_bits(omode) == 128);
				panic("can't convert long long to long double yet");
			}
		} else {
			panic("can't lower 64bit Conv");
		}
	} else {
		panic("Can't lower unexpected 64bit operation %s", get_op_name(op));
	}
	id     = new_id_from_str(name);
	result = new_entity(glob, id, method);
	set_entity_ld_ident(result, id);
	set_entity_visibility(result, ir_visibility_external);
	return result;
}

void sparc_lower_64bit(void)
{
	lwrdw_param_t lower_dw_params = {
		0,  /* big endian */
		64, /* doubleword size */
		create_64_intrinsic_fkt,
		NULL
	};

	/* make sure opcodes are initialized */
	sparc_create_opcodes(&sparc_irn_ops);

	ir_prepare_dw_lowering(&lower_dw_params);
	ir_register_dw_lower_function(op_Add,   lower64_add);
	ir_register_dw_lower_function(op_Minus, lower64_minus);
	ir_register_dw_lower_function(op_Sub,   lower64_sub);
	ir_lower_dw_ops();
}
