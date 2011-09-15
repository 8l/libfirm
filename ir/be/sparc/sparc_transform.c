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
 * @brief   code selection (transform FIRM into SPARC FIRM)
 * @author  Hannes Rapp, Matthias Braun
 * @version $Id$
 */
#include "config.h"

#include <stdint.h>
#include <stdbool.h>

#include "irnode_t.h"
#include "irgraph_t.h"
#include "irmode_t.h"
#include "irgmod.h"
#include "iredges.h"
#include "ircons.h"
#include "irprintf.h"
#include "iroptimize.h"
#include "dbginfo.h"
#include "iropt_t.h"
#include "debug.h"
#include "error.h"
#include "util.h"

#include "../benode.h"
#include "../beirg.h"
#include "../beutil.h"
#include "../betranshlp.h"
#include "../beabihelper.h"
#include "bearch_sparc_t.h"

#include "sparc_nodes_attr.h"
#include "sparc_transform.h"
#include "sparc_new_nodes.h"
#include "gen_sparc_new_nodes.h"

#include "gen_sparc_regalloc_if.h"
#include "sparc_cconv.h"

#include <limits.h>

DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

static const arch_register_t *sp_reg = &sparc_registers[REG_SP];
static const arch_register_t *fp_reg = &sparc_registers[REG_FRAME_POINTER];
static calling_convention_t  *current_cconv = NULL;
static be_stackorder_t       *stackorder;
static ir_mode               *mode_gp;
static ir_mode               *mode_flags;
static ir_mode               *mode_fp;
static ir_mode               *mode_fp2;
//static ir_mode               *mode_fp4;
static pmap                  *node_to_stack;
static size_t                 start_mem_offset;
static ir_node               *start_mem;
static size_t                 start_g0_offset;
static ir_node               *start_g0;
static size_t                 start_g7_offset;
static ir_node               *start_g7;
static size_t                 start_sp_offset;
static ir_node               *start_sp;
static size_t                 start_fp_offset;
static ir_node               *start_fp;
static ir_node               *frame_base;
static size_t                 start_params_offset;
static size_t                 start_callee_saves_offset;

static const arch_register_t *const omit_fp_callee_saves[] = {
	&sparc_registers[REG_L0],
	&sparc_registers[REG_L1],
	&sparc_registers[REG_L2],
	&sparc_registers[REG_L3],
	&sparc_registers[REG_L4],
	&sparc_registers[REG_L5],
	&sparc_registers[REG_L6],
	&sparc_registers[REG_L7],
	&sparc_registers[REG_I0],
	&sparc_registers[REG_I1],
	&sparc_registers[REG_I2],
	&sparc_registers[REG_I3],
	&sparc_registers[REG_I4],
	&sparc_registers[REG_I5],
};

static inline bool mode_needs_gp_reg(ir_mode *mode)
{
	if (mode_is_int(mode) || mode_is_reference(mode)) {
		/* we should only see 32bit code */
		assert(get_mode_size_bits(mode) <= 32);
		return true;
	}
	return false;
}

/**
 * Create an And that will zero out upper bits.
 *
 * @param dbgi      debug info
 * @param block     the basic block
 * @param op        the original node
 * @param src_bits  number of lower bits that will remain
 */
static ir_node *gen_zero_extension(dbg_info *dbgi, ir_node *block, ir_node *op,
                                   int src_bits)
{
	if (src_bits == 8) {
		return new_bd_sparc_And_imm(dbgi, block, op, NULL, 0xFF);
	} else if (src_bits == 16) {
		ir_node *lshift = new_bd_sparc_Sll_imm(dbgi, block, op, NULL, 16);
		ir_node *rshift = new_bd_sparc_Srl_imm(dbgi, block, lshift, NULL, 16);
		return rshift;
	} else {
		panic("zero extension only supported for 8 and 16 bits");
	}
}

/**
 * Generate code for a sign extension.
 *
 * @param dbgi      debug info
 * @param block     the basic block
 * @param op        the original node
 * @param src_bits  number of lower bits that will remain
 */
static ir_node *gen_sign_extension(dbg_info *dbgi, ir_node *block, ir_node *op,
                                   int src_bits)
{
	int shift_width = 32 - src_bits;
	ir_node *lshift_node = new_bd_sparc_Sll_imm(dbgi, block, op, NULL, shift_width);
	ir_node *rshift_node = new_bd_sparc_Sra_imm(dbgi, block, lshift_node, NULL, shift_width);
	return rshift_node;
}

/**
 * returns true if it is assured, that the upper bits of a node are "clean"
 * which means for a 16 or 8 bit value, that the upper bits in the register
 * are 0 for unsigned and a copy of the last significant bit for signed
 * numbers.
 */
static bool upper_bits_clean(ir_node *node, ir_mode *mode)
{
	switch ((ir_opcode)get_irn_opcode(node)) {
	case iro_And:
		if (!mode_is_signed(mode)) {
			return upper_bits_clean(get_And_left(node), mode)
			    || upper_bits_clean(get_And_right(node), mode);
		}
		/* FALLTHROUGH */
	case iro_Or:
	case iro_Eor:
		return upper_bits_clean(get_binop_left(node), mode)
		    && upper_bits_clean(get_binop_right(node), mode);

	case iro_Shr:
		if (mode_is_signed(mode)) {
			return false; /* TODO */
		} else {
			ir_node *right = get_Shr_right(node);
			if (is_Const(right)) {
				ir_tarval *tv  = get_Const_tarval(right);
				long       val = get_tarval_long(tv);
				if (val >= 32 - (long)get_mode_size_bits(mode))
					return true;
			}
			return upper_bits_clean(get_Shr_left(node), mode);
		}

	case iro_Shrs:
		return upper_bits_clean(get_Shrs_left(node), mode);

	case iro_Const: {
		ir_tarval *tv  = get_Const_tarval(node);
		long       val = get_tarval_long(tv);
		if (mode_is_signed(mode)) {
			long    shifted = val >> (get_mode_size_bits(mode)-1);
			return shifted == 0 || shifted == -1;
		} else {
			unsigned long shifted = (unsigned long)val;
			shifted >>= get_mode_size_bits(mode)-1;
			shifted >>= 1;
			return shifted == 0;
		}
	}

	case iro_Conv: {
		ir_mode *dest_mode = get_irn_mode(node);
		ir_node *op        = get_Conv_op(node);
		ir_mode *src_mode  = get_irn_mode(op);
		unsigned src_bits  = get_mode_size_bits(src_mode);
		unsigned dest_bits = get_mode_size_bits(dest_mode);
		/* downconvs are a nop */
		if (src_bits <= dest_bits)
			return upper_bits_clean(op, mode);
		if (dest_bits <= get_mode_size_bits(mode)
		    && mode_is_signed(dest_mode) == mode_is_signed(mode))
			return true;
		return false;
	}

	case iro_Proj: {
		ir_node *pred = get_Proj_pred(node);
		switch (get_irn_opcode(pred)) {
		case iro_Load: {
			ir_mode *load_mode = get_Load_mode(pred);
			unsigned load_bits = get_mode_size_bits(load_mode);
			unsigned bits      = get_mode_size_bits(mode);
			if (load_bits > bits)
				return false;
			if (mode_is_signed(mode) != mode_is_signed(load_mode))
				return false;
			return true;
		}
		default:
			break;
		}
	}
	default:
		break;
	}
	return false;
}

/**
 * Extend a value to 32 bit signed/unsigned depending on its mode.
 *
 * @param dbgi      debug info
 * @param block     the basic block
 * @param op        the original node
 * @param orig_mode the original mode of op
 */
static ir_node *gen_extension(dbg_info *dbgi, ir_node *block, ir_node *op,
                              ir_mode *orig_mode)
{
	int bits = get_mode_size_bits(orig_mode);
	assert(bits < 32);

	if (mode_is_signed(orig_mode)) {
		return gen_sign_extension(dbgi, block, op, bits);
	} else {
		return gen_zero_extension(dbgi, block, op, bits);
	}
}

typedef enum {
	MATCH_NONE         = 0,
	MATCH_COMMUTATIVE  = 1U << 0, /**< commutative operation. */
	MATCH_MODE_NEUTRAL = 1U << 1, /**< the higher bits of the inputs don't
	                                   influence the significant lower bit at
	                                   all (for cases where mode < 32bit) */
} match_flags_t;
ENUM_BITSET(match_flags_t)

typedef ir_node* (*new_binop_reg_func) (dbg_info *dbgi, ir_node *block, ir_node *op1, ir_node *op2);
typedef ir_node* (*new_binop_fp_func) (dbg_info *dbgi, ir_node *block, ir_node *op1, ir_node *op2, ir_mode *mode);
typedef ir_node* (*new_binop_imm_func) (dbg_info *dbgi, ir_node *block, ir_node *op1, ir_entity *entity, int32_t immediate);
typedef ir_node* (*new_unop_fp_func) (dbg_info *dbgi, ir_node *block, ir_node *op1, ir_mode *mode);

/**
 * checks if a node's value can be encoded as a immediate
 */
static bool is_imm_encodeable(const ir_node *node)
{
	long value;
	if (!is_Const(node))
		return false;

	value = get_tarval_long(get_Const_tarval(node));
	return sparc_is_value_imm_encodeable(value);
}

static bool needs_extension(ir_node *op)
{
	ir_mode *mode = get_irn_mode(op);
	if (get_mode_size_bits(mode) >= get_mode_size_bits(mode_gp))
		return false;
	return !upper_bits_clean(op, mode);
}

/**
 * Check, if a given node is a Down-Conv, ie. a integer Conv
 * from a mode with a mode with more bits to a mode with lesser bits.
 * Moreover, we return only true if the node has not more than 1 user.
 *
 * @param node   the node
 * @return non-zero if node is a Down-Conv
 */
static bool is_downconv(const ir_node *node)
{
	ir_mode *src_mode;
	ir_mode *dest_mode;

	if (!is_Conv(node))
		return false;

	src_mode  = get_irn_mode(get_Conv_op(node));
	dest_mode = get_irn_mode(node);
	return
		mode_needs_gp_reg(src_mode)  &&
		mode_needs_gp_reg(dest_mode) &&
		get_mode_size_bits(dest_mode) <= get_mode_size_bits(src_mode);
}

static ir_node *skip_downconv(ir_node *node)
{
	while (is_downconv(node)) {
		node = get_Conv_op(node);
	}
	return node;
}

/**
 * helper function for binop operations
 *
 * @param new_reg  register generation function ptr
 * @param new_imm  immediate generation function ptr
 */
static ir_node *gen_helper_binop_args(ir_node *node,
                                      ir_node *op1, ir_node *op2,
                                      match_flags_t flags,
                                      new_binop_reg_func new_reg,
                                      new_binop_imm_func new_imm)
{
	dbg_info *dbgi  = get_irn_dbg_info(node);
	ir_node  *block = be_transform_node(get_nodes_block(node));
	ir_node  *new_op1;
	ir_node  *new_op2;
	ir_mode  *mode1;
	ir_mode  *mode2;

	if (flags & MATCH_MODE_NEUTRAL) {
		op1 = skip_downconv(op1);
		op2 = skip_downconv(op2);
	}
	mode1 = get_irn_mode(op1);
	mode2 = get_irn_mode(op2);
	/* we shouldn't see 64bit code */
	assert(get_mode_size_bits(mode1) <= 32);
	assert(get_mode_size_bits(mode2) <= 32);

	if (is_imm_encodeable(op2)) {
		int32_t  immediate = get_tarval_long(get_Const_tarval(op2));
		new_op1 = be_transform_node(op1);
		if (! (flags & MATCH_MODE_NEUTRAL) && needs_extension(op1)) {
			new_op1 = gen_extension(dbgi, block, new_op1, mode1);
		}
		return new_imm(dbgi, block, new_op1, NULL, immediate);
	}
	new_op2 = be_transform_node(op2);
	if (! (flags & MATCH_MODE_NEUTRAL) && needs_extension(op2)) {
		new_op2 = gen_extension(dbgi, block, new_op2, mode2);
	}

	if ((flags & MATCH_COMMUTATIVE) && is_imm_encodeable(op1)) {
		int32_t immediate = get_tarval_long(get_Const_tarval(op1));
		return new_imm(dbgi, block, new_op2, NULL, immediate);
	}

	new_op1 = be_transform_node(op1);
	if (! (flags & MATCH_MODE_NEUTRAL) && needs_extension(op1)) {
		new_op1 = gen_extension(dbgi, block, new_op1, mode1);
	}
	return new_reg(dbgi, block, new_op1, new_op2);
}

static ir_node *gen_helper_binop(ir_node *node, match_flags_t flags,
                                 new_binop_reg_func new_reg,
                                 new_binop_imm_func new_imm)
{
	ir_node *op1 = get_binop_left(node);
	ir_node *op2 = get_binop_right(node);
	return gen_helper_binop_args(node, op1, op2, flags, new_reg, new_imm);
}

/**
 * helper function for FP binop operations
 */
static ir_node *gen_helper_binfpop(ir_node *node, ir_mode *mode,
                                   new_binop_fp_func new_func_single,
                                   new_binop_fp_func new_func_double,
                                   new_binop_fp_func new_func_quad)
{
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	ir_node  *op1     = get_binop_left(node);
	ir_node  *new_op1 = be_transform_node(op1);
	ir_node  *op2     = get_binop_right(node);
	ir_node  *new_op2 = be_transform_node(op2);
	dbg_info *dbgi    = get_irn_dbg_info(node);
	unsigned  bits    = get_mode_size_bits(mode);

	switch (bits) {
	case 32:
		return new_func_single(dbgi, block, new_op1, new_op2, mode);
	case 64:
		return new_func_double(dbgi, block, new_op1, new_op2, mode);
	case 128:
		return new_func_quad(dbgi, block, new_op1, new_op2, mode);
	default:
		break;
	}
	panic("unsupported mode %+F for float op", mode);
}

static ir_node *gen_helper_unfpop(ir_node *node, ir_mode *mode,
                                  new_unop_fp_func new_func_single,
                                  new_unop_fp_func new_func_double,
                                  new_unop_fp_func new_func_quad)
{
	ir_node  *block  = be_transform_node(get_nodes_block(node));
	ir_node  *op     = get_unop_op(node);
	ir_node  *new_op = be_transform_node(op);
	dbg_info *dbgi   = get_irn_dbg_info(node);
	unsigned  bits   = get_mode_size_bits(mode);

	switch (bits) {
	case 32:
		return new_func_single(dbgi, block, new_op, mode);
	case 64:
		return new_func_double(dbgi, block, new_op, mode);
	case 128:
		return new_func_quad(dbgi, block, new_op, mode);
	default:
		break;
	}
	panic("unsupported mode %+F for float op", mode);
}

typedef ir_node* (*new_binopx_imm_func)(dbg_info *dbgi, ir_node *block,
                                        ir_node *op1, ir_node *flags,
                                        ir_entity *imm_entity, int32_t imm);

typedef ir_node* (*new_binopx_reg_func)(dbg_info *dbgi, ir_node *block,
                                        ir_node *op1, ir_node *op2,
                                        ir_node *flags);

static ir_node *gen_helper_binopx(ir_node *node, match_flags_t match_flags,
                                  new_binopx_reg_func new_binopx_reg,
                                  new_binopx_imm_func new_binopx_imm)
{
	dbg_info *dbgi      = get_irn_dbg_info(node);
	ir_node  *block     = be_transform_node(get_nodes_block(node));
	ir_node  *op1       = get_irn_n(node, 0);
	ir_node  *op2       = get_irn_n(node, 1);
	ir_node  *flags     = get_irn_n(node, 2);
	ir_node  *new_flags = be_transform_node(flags);
	ir_node  *new_op1;
	ir_node  *new_op2;

	/* only support for mode-neutral implemented so far */
	assert(match_flags & MATCH_MODE_NEUTRAL);

	if (is_imm_encodeable(op2)) {
		int32_t  immediate = get_tarval_long(get_Const_tarval(op2));
		new_op1 = be_transform_node(op1);
		return new_binopx_imm(dbgi, block, new_op1, new_flags, NULL, immediate);
	}
	new_op2 = be_transform_node(op2);
	if ((match_flags & MATCH_COMMUTATIVE) && is_imm_encodeable(op1)) {
		int32_t immediate = get_tarval_long(get_Const_tarval(op1));
		return new_binopx_imm(dbgi, block, new_op2, new_flags, NULL, immediate);
	}
	new_op1 = be_transform_node(op1);
	return new_binopx_reg(dbgi, block, new_op1, new_op2, new_flags);

}

static ir_node *get_g0(ir_graph *irg)
{
	if (start_g0 == NULL) {
		/* this is already the transformed start node */
		ir_node *start = get_irg_start(irg);
		assert(is_sparc_Start(start));
		start_g0 = new_r_Proj(start, mode_gp, start_g0_offset);
	}
	return start_g0;
}

static ir_node *get_g7(ir_graph *irg)
{
	if (start_g7 == NULL) {
		ir_node *start = get_irg_start(irg);
		assert(is_sparc_Start(start));
		start_g7 = new_r_Proj(start, mode_gp, start_g7_offset);
	}
	return start_g7;
}

static ir_node *make_tls_offset(dbg_info *dbgi, ir_node *block,
                                ir_entity *entity, int32_t offset)
{
	ir_node  *hi  = new_bd_sparc_SetHi(dbgi, block, entity, offset);
	ir_node  *low = new_bd_sparc_Xor_imm(dbgi, block, hi, entity, offset);
	return low;
}

static ir_node *make_address(dbg_info *dbgi, ir_node *block, ir_entity *entity,
                             int32_t offset)
{
	if (get_entity_owner(entity) == get_tls_type()) {
		ir_graph *irg     = get_irn_irg(block);
		ir_node  *g7      = get_g7(irg);
		ir_node  *offsetn = make_tls_offset(dbgi, block, entity, offset);
		ir_node  *add     = new_bd_sparc_Add_reg(dbgi, block, g7, offsetn);
		return add;
	} else {
		ir_node *hi  = new_bd_sparc_SetHi(dbgi, block, entity, offset);
		ir_node *low = new_bd_sparc_Or_imm(dbgi, block, hi, entity, offset);
		return low;
	}
}

typedef struct address_t {
	ir_node   *ptr;
	ir_node   *ptr2;
	ir_entity *entity;
	int32_t    offset;
} address_t;

/**
 * Match a load/store address
 */
static void match_address(ir_node *ptr, address_t *address, bool use_ptr2)
{
	ir_node   *base   = ptr;
	ir_node   *ptr2   = NULL;
	int32_t    offset = 0;
	ir_entity *entity = NULL;

	if (is_Add(base)) {
		ir_node *add_right = get_Add_right(base);
		if (is_Const(add_right)) {
			base    = get_Add_left(base);
			offset += get_tarval_long(get_Const_tarval(add_right));
		}
	}
	/* Note that we don't match sub(x, Const) or chains of adds/subs
	 * because this should all be normalized by now */

	/* we only use the symconst if we're the only user otherwise we probably
	 * won't save anything but produce multiple sethi+or combinations with
	 * just different offsets */
	if (is_SymConst(base) && get_irn_n_edges(base) == 1) {
		ir_entity *sc_entity = get_SymConst_entity(base);
		dbg_info  *dbgi      = get_irn_dbg_info(ptr);
		ir_node   *block     = get_nodes_block(ptr);
		ir_node   *new_block = be_transform_node(block);

		if (get_entity_owner(sc_entity) == get_tls_type()) {
			if (!use_ptr2) {
				goto only_offset;
			} else {
				ptr2   = make_tls_offset(dbgi, new_block, sc_entity, offset);
				offset = 0;
				base   = get_g7(get_irn_irg(base));
			}
		} else {
			entity = sc_entity;
			base   = new_bd_sparc_SetHi(dbgi, new_block, entity, offset);
		}
	} else if (use_ptr2 && is_Add(base) && offset == 0) {
		ptr2 = be_transform_node(get_Add_right(base));
		base = be_transform_node(get_Add_left(base));
	} else {
only_offset:
		if (sparc_is_value_imm_encodeable(offset)) {
			base = be_transform_node(base);
		} else {
			base   = be_transform_node(ptr);
			offset = 0;
		}
	}

	address->ptr    = base;
	address->ptr2   = ptr2;
	address->entity = entity;
	address->offset = offset;
}

/**
 * Creates an sparc Add.
 *
 * @param node   FIRM node
 * @return the created sparc Add node
 */
static ir_node *gen_Add(ir_node *node)
{
	ir_mode *mode = get_irn_mode(node);
	ir_node *right;

	if (mode_is_float(mode)) {
		return gen_helper_binfpop(node, mode, new_bd_sparc_fadd_s,
		                          new_bd_sparc_fadd_d, new_bd_sparc_fadd_q);
	}

	/* special case: + 0x1000 can be represented as - 0x1000 */
	right = get_Add_right(node);
	if (is_Const(right)) {
		ir_node   *left = get_Add_left(node);
		ir_tarval *tv;
		uint32_t   val;
		/* is this simple address arithmetic? then we can let the linker do
		 * the calculation. */
		if (is_SymConst(left) && get_irn_n_edges(left) == 1) {
			dbg_info *dbgi  = get_irn_dbg_info(node);
			ir_node  *block = be_transform_node(get_nodes_block(node));
			address_t address;

			/* the value of use_ptr2 shouldn't matter here */
			match_address(node, &address, false);
			assert(is_sparc_SetHi(address.ptr));
			return new_bd_sparc_Or_imm(dbgi, block, address.ptr,
			                           address.entity, address.offset);
		}

		tv  = get_Const_tarval(right);
		val = get_tarval_long(tv);
		if (val == 0x1000) {
			dbg_info *dbgi   = get_irn_dbg_info(node);
			ir_node  *block  = be_transform_node(get_nodes_block(node));
			ir_node  *op     = get_Add_left(node);
			ir_node  *new_op = be_transform_node(op);
			return new_bd_sparc_Sub_imm(dbgi, block, new_op, NULL, -0x1000);
		}
	}

	return gen_helper_binop(node, MATCH_COMMUTATIVE | MATCH_MODE_NEUTRAL,
	                        new_bd_sparc_Add_reg, new_bd_sparc_Add_imm);
}

static ir_node *gen_AddCC_t(ir_node *node)
{
	return gen_helper_binop(node, MATCH_COMMUTATIVE | MATCH_MODE_NEUTRAL,
	                        new_bd_sparc_AddCC_reg, new_bd_sparc_AddCC_imm);
}

static ir_node *gen_Proj_AddCC_t(ir_node *node)
{
	long     pn       = get_Proj_proj(node);
	ir_node *pred     = get_Proj_pred(node);
	ir_node *new_pred = be_transform_node(pred);

	switch (pn) {
	case pn_sparc_AddCC_t_res:
		return new_r_Proj(new_pred, mode_gp, pn_sparc_AddCC_res);
	case pn_sparc_AddCC_t_flags:
		return new_r_Proj(new_pred, mode_flags, pn_sparc_AddCC_flags);
	default:
		panic("Invalid AddCC_t proj found");
	}
}

static ir_node *gen_AddX_t(ir_node *node)
{
	return gen_helper_binopx(node, MATCH_COMMUTATIVE | MATCH_MODE_NEUTRAL,
	                         new_bd_sparc_AddX_reg, new_bd_sparc_AddX_imm);
}

/**
 * Creates an sparc Sub.
 *
 * @param node       FIRM node
 * @return the created sparc Sub node
 */
static ir_node *gen_Sub(ir_node *node)
{
	ir_mode *mode = get_irn_mode(node);

	if (mode_is_float(mode)) {
		return gen_helper_binfpop(node, mode, new_bd_sparc_fsub_s,
		                          new_bd_sparc_fsub_d, new_bd_sparc_fsub_q);
	}

	return gen_helper_binop(node, MATCH_MODE_NEUTRAL,
	                        new_bd_sparc_Sub_reg, new_bd_sparc_Sub_imm);
}

static ir_node *gen_SubCC_t(ir_node *node)
{
	return gen_helper_binop(node, MATCH_MODE_NEUTRAL,
	                        new_bd_sparc_SubCC_reg, new_bd_sparc_SubCC_imm);
}

static ir_node *gen_Proj_SubCC_t(ir_node *node)
{
	long     pn       = get_Proj_proj(node);
	ir_node *pred     = get_Proj_pred(node);
	ir_node *new_pred = be_transform_node(pred);

	switch (pn) {
	case pn_sparc_SubCC_t_res:
		return new_r_Proj(new_pred, mode_gp, pn_sparc_SubCC_res);
	case pn_sparc_SubCC_t_flags:
		return new_r_Proj(new_pred, mode_flags, pn_sparc_SubCC_flags);
	default:
		panic("Invalid SubCC_t proj found");
	}
}

static ir_node *gen_SubX_t(ir_node *node)
{
	return gen_helper_binopx(node, MATCH_MODE_NEUTRAL,
	                         new_bd_sparc_SubX_reg, new_bd_sparc_SubX_imm);
}

ir_node *create_ldf(dbg_info *dbgi, ir_node *block, ir_node *ptr,
                    ir_node *mem, ir_mode *mode, ir_entity *entity,
                    long offset, bool is_frame_entity)
{
	unsigned bits = get_mode_size_bits(mode);
	assert(mode_is_float(mode));
	if (bits == 32) {
		return new_bd_sparc_Ldf_s(dbgi, block, ptr, mem, mode, entity,
		                          offset, is_frame_entity);
	} else if (bits == 64) {
		return new_bd_sparc_Ldf_d(dbgi, block, ptr, mem, mode, entity,
		                          offset, is_frame_entity);
	} else {
		assert(bits == 128);
		return new_bd_sparc_Ldf_q(dbgi, block, ptr, mem, mode, entity,
		                          offset, is_frame_entity);
	}
}

ir_node *create_stf(dbg_info *dbgi, ir_node *block, ir_node *value,
                    ir_node *ptr, ir_node *mem, ir_mode *mode,
                    ir_entity *entity, long offset,
                    bool is_frame_entity)
{
	unsigned bits = get_mode_size_bits(mode);
	assert(mode_is_float(mode));
	if (bits == 32) {
		return new_bd_sparc_Stf_s(dbgi, block, value, ptr, mem, mode, entity,
		                          offset, is_frame_entity);
	} else if (bits == 64) {
		return new_bd_sparc_Stf_d(dbgi, block, value, ptr, mem, mode, entity,
		                          offset, is_frame_entity);
	} else {
		assert(bits == 128);
		return new_bd_sparc_Stf_q(dbgi, block, value, ptr, mem, mode, entity,
		                          offset, is_frame_entity);
	}
}

/**
 * Transforms a Load.
 *
 * @param node    the ir Load node
 * @return the created sparc Load node
 */
static ir_node *gen_Load(ir_node *node)
{
	dbg_info *dbgi     = get_irn_dbg_info(node);
	ir_mode  *mode     = get_Load_mode(node);
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *ptr      = get_Load_ptr(node);
	ir_node  *mem      = get_Load_mem(node);
	ir_node  *new_mem  = be_transform_node(mem);
	ir_node  *new_load = NULL;
	address_t address;

	if (get_Load_unaligned(node) == align_non_aligned) {
		panic("sparc: transformation of unaligned Loads not implemented yet");
	}

	if (mode_is_float(mode)) {
		match_address(ptr, &address, false);
		new_load = create_ldf(dbgi, block, address.ptr, new_mem, mode,
		                      address.entity, address.offset, false);
	} else {
		match_address(ptr, &address, true);
		if (address.ptr2 != NULL) {
			assert(address.entity == NULL && address.offset == 0);
			new_load = new_bd_sparc_Ld_reg(dbgi, block, address.ptr,
			                               address.ptr2, new_mem, mode);
		} else {
			new_load = new_bd_sparc_Ld_imm(dbgi, block, address.ptr, new_mem,
			                               mode, address.entity, address.offset,
			                               false);
		}
	}
	set_irn_pinned(new_load, get_irn_pinned(node));

	return new_load;
}

/**
 * Transforms a Store.
 *
 * @param node    the ir Store node
 * @return the created sparc Store node
 */
static ir_node *gen_Store(ir_node *node)
{
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *ptr      = get_Store_ptr(node);
	ir_node  *mem      = get_Store_mem(node);
	ir_node  *new_mem  = be_transform_node(mem);
	ir_node  *val      = get_Store_value(node);
	ir_mode  *mode     = get_irn_mode(val);
	dbg_info *dbgi     = get_irn_dbg_info(node);
	ir_node  *new_store = NULL;
	address_t address;

	if (get_Store_unaligned(node) == align_non_aligned) {
		panic("sparc: transformation of unaligned Stores not implemented yet");
	}

	if (mode_is_float(mode)) {
		ir_node *new_val = be_transform_node(val);
		/* TODO: variants with reg+reg address mode */
		match_address(ptr, &address, false);
		new_store = create_stf(dbgi, block, new_val, address.ptr, new_mem,
		                       mode, address.entity, address.offset, false);
	} else {
		ir_node *new_val;
		unsigned dest_bits = get_mode_size_bits(mode);
		while (is_downconv(node)
		       && get_mode_size_bits(get_irn_mode(node)) >= dest_bits) {
		    val = get_Conv_op(val);
		}
		new_val = be_transform_node(val);

		assert(dest_bits <= 32);
		match_address(ptr, &address, true);
		if (address.ptr2 != NULL) {
			assert(address.entity == NULL && address.offset == 0);
			new_store = new_bd_sparc_St_reg(dbgi, block, new_val, address.ptr,
			                                address.ptr2, new_mem, mode);
		} else {
			new_store = new_bd_sparc_St_imm(dbgi, block, new_val, address.ptr,
			                                new_mem, mode, address.entity,
			                                address.offset, false);
		}
	}
	set_irn_pinned(new_store, get_irn_pinned(node));

	return new_store;
}

/**
 * Creates an sparc Mul.
 * returns the lower 32bits of the 64bit multiply result
 *
 * @return the created sparc Mul node
 */
static ir_node *gen_Mul(ir_node *node)
{
	ir_mode *mode = get_irn_mode(node);
	if (mode_is_float(mode)) {
		return gen_helper_binfpop(node, mode, new_bd_sparc_fmul_s,
		                          new_bd_sparc_fmul_d, new_bd_sparc_fmul_q);
	}

	return gen_helper_binop(node, MATCH_COMMUTATIVE | MATCH_MODE_NEUTRAL,
	                        new_bd_sparc_Mul_reg, new_bd_sparc_Mul_imm);
}

/**
 * Creates an sparc Mulh.
 * Mulh returns the upper 32bits of a mul instruction
 *
 * @return the created sparc Mulh node
 */
static ir_node *gen_Mulh(ir_node *node)
{
	ir_mode *mode = get_irn_mode(node);
	ir_node *mul;

	if (mode_is_float(mode))
		panic("FP not supported yet");

	if (mode_is_signed(mode)) {
		mul = gen_helper_binop(node, MATCH_COMMUTATIVE, new_bd_sparc_SMulh_reg, new_bd_sparc_SMulh_imm);
		return new_r_Proj(mul, mode_gp, pn_sparc_SMulh_low);
	} else {
		mul = gen_helper_binop(node, MATCH_COMMUTATIVE, new_bd_sparc_UMulh_reg, new_bd_sparc_UMulh_imm);
		return new_r_Proj(mul, mode_gp, pn_sparc_UMulh_low);
	}
}

static ir_node *gen_sign_extension_value(ir_node *node)
{
	ir_node *block     = get_nodes_block(node);
	ir_node *new_block = be_transform_node(block);
	ir_node *new_node  = be_transform_node(node);
	/* TODO: we could do some shortcuts for some value types probably.
	 * (For constants or other cases where we know the sign bit in
	 *  advance) */
	return new_bd_sparc_Sra_imm(NULL, new_block, new_node, NULL, 31);
}

/**
 * Creates an sparc Div.
 *
 * @return the created sparc Div node
 */
static ir_node *gen_Div(ir_node *node)
{
	dbg_info *dbgi      = get_irn_dbg_info(node);
	ir_node  *block     = get_nodes_block(node);
	ir_node  *new_block = be_transform_node(block);
	ir_mode  *mode      = get_Div_resmode(node);
	ir_node  *left      = get_Div_left(node);
	ir_node  *left_low  = be_transform_node(left);
	ir_node  *right     = get_Div_right(node);
	ir_node  *res;

	if (mode_is_float(mode)) {
		return gen_helper_binfpop(node, mode, new_bd_sparc_fdiv_s,
								  new_bd_sparc_fdiv_d, new_bd_sparc_fdiv_q);
	}

	if (mode_is_signed(mode)) {
		ir_node *left_high = gen_sign_extension_value(left);

		if (is_imm_encodeable(right)) {
			int32_t immediate = get_tarval_long(get_Const_tarval(right));
			res = new_bd_sparc_SDiv_imm(dbgi, new_block, left_high, left_low,
			                            NULL, immediate);
		} else {
			ir_node *new_right = be_transform_node(right);
			res = new_bd_sparc_SDiv_reg(dbgi, new_block, left_high, left_low,
			                            new_right);
		}
	} else {
		ir_graph *irg       = get_irn_irg(node);
		ir_node  *left_high = get_g0(irg);
		if (is_imm_encodeable(right)) {
			int32_t immediate = get_tarval_long(get_Const_tarval(right));
			res = new_bd_sparc_UDiv_imm(dbgi, new_block, left_high, left_low,
			                            NULL, immediate);
		} else {
			ir_node *new_right = be_transform_node(right);
			res = new_bd_sparc_UDiv_reg(dbgi, new_block, left_high, left_low,
			                            new_right);
		}
	}

	return res;
}

/**
 * Transforms a Not node.
 *
 * @return the created sparc Not node
 */
static ir_node *gen_Not(ir_node *node)
{
	ir_node  *op     = get_Not_op(node);
	ir_graph *irg    = get_irn_irg(node);
	ir_node  *zero   = get_g0(irg);
	dbg_info *dbgi   = get_irn_dbg_info(node);
	ir_node  *block  = be_transform_node(get_nodes_block(node));
	ir_node  *new_op = be_transform_node(op);

	/* Note: Not(Eor()) is normalize in firm localopts already so
	 * we don't match it for xnor here */

	/* Not can be represented with xnor 0, n */
	return new_bd_sparc_XNor_reg(dbgi, block, zero, new_op);
}

static ir_node *gen_helper_bitop(ir_node *node,
                                 new_binop_reg_func new_reg,
                                 new_binop_imm_func new_imm,
                                 new_binop_reg_func new_not_reg,
                                 new_binop_imm_func new_not_imm,
                                 match_flags_t flags)
{
	ir_node *op1 = get_binop_left(node);
	ir_node *op2 = get_binop_right(node);
	if (is_Not(op1)) {
		return gen_helper_binop_args(node, op2, get_Not_op(op1),
		                             flags,
		                             new_not_reg, new_not_imm);
	}
	if (is_Not(op2)) {
		return gen_helper_binop_args(node, op1, get_Not_op(op2),
		                             flags,
		                             new_not_reg, new_not_imm);
	}
	return gen_helper_binop_args(node, op1, op2,
								 flags | MATCH_COMMUTATIVE,
								 new_reg, new_imm);
}

static ir_node *gen_And(ir_node *node)
{
	return gen_helper_bitop(node,
	                        new_bd_sparc_And_reg,
	                        new_bd_sparc_And_imm,
	                        new_bd_sparc_AndN_reg,
	                        new_bd_sparc_AndN_imm,
	                        MATCH_MODE_NEUTRAL);
}

static ir_node *gen_Or(ir_node *node)
{
	return gen_helper_bitop(node,
	                        new_bd_sparc_Or_reg,
	                        new_bd_sparc_Or_imm,
	                        new_bd_sparc_OrN_reg,
	                        new_bd_sparc_OrN_imm,
	                        MATCH_MODE_NEUTRAL);
}

static ir_node *gen_Eor(ir_node *node)
{
	return gen_helper_bitop(node,
	                        new_bd_sparc_Xor_reg,
	                        new_bd_sparc_Xor_imm,
	                        new_bd_sparc_XNor_reg,
	                        new_bd_sparc_XNor_imm,
	                        MATCH_MODE_NEUTRAL);
}

static ir_node *gen_Shl(ir_node *node)
{
	ir_mode *mode = get_irn_mode(node);
	if (get_mode_modulo_shift(mode) != 32)
		panic("modulo_shift!=32 not supported by sparc backend");
	return gen_helper_binop(node, MATCH_NONE, new_bd_sparc_Sll_reg, new_bd_sparc_Sll_imm);
}

static ir_node *gen_Shr(ir_node *node)
{
	ir_mode *mode = get_irn_mode(node);
	if (get_mode_modulo_shift(mode) != 32)
		panic("modulo_shift!=32 not supported by sparc backend");
	return gen_helper_binop(node, MATCH_NONE, new_bd_sparc_Srl_reg, new_bd_sparc_Srl_imm);
}

static ir_node *gen_Shrs(ir_node *node)
{
	ir_mode *mode = get_irn_mode(node);
	if (get_mode_modulo_shift(mode) != 32)
		panic("modulo_shift!=32 not supported by sparc backend");
	return gen_helper_binop(node, MATCH_NONE, new_bd_sparc_Sra_reg, new_bd_sparc_Sra_imm);
}

/**
 * Transforms a Minus node.
 */
static ir_node *gen_Minus(ir_node *node)
{
	ir_mode  *mode = get_irn_mode(node);
	ir_node  *op;
	ir_node  *block;
	ir_node  *new_op;
	ir_node  *zero;
	dbg_info *dbgi;

	if (mode_is_float(mode)) {
		return gen_helper_unfpop(node, mode, new_bd_sparc_fneg_s,
		                         new_bd_sparc_fneg_d, new_bd_sparc_fneg_q);
	}
	block  = be_transform_node(get_nodes_block(node));
	dbgi   = get_irn_dbg_info(node);
	op     = get_Minus_op(node);
	new_op = be_transform_node(op);
	zero   = get_g0(get_irn_irg(node));
	return new_bd_sparc_Sub_reg(dbgi, block, zero, new_op);
}

/**
 * Create an entity for a given (floating point) tarval
 */
static ir_entity *create_float_const_entity(ir_tarval *tv)
{
	const arch_env_t *arch_env = be_get_irg_arch_env(current_ir_graph);
	sparc_isa_t      *isa      = (sparc_isa_t*) arch_env;
	ir_entity        *entity   = (ir_entity*) pmap_get(isa->constants, tv);
	ir_initializer_t *initializer;
	ir_mode          *mode;
	ir_type          *type;
	ir_type          *glob;

	if (entity != NULL)
		return entity;

	mode   = get_tarval_mode(tv);
	type   = get_type_for_mode(mode);
	glob   = get_glob_type();
	entity = new_entity(glob, id_unique("C%u"), type);
	set_entity_visibility(entity, ir_visibility_private);
	add_entity_linkage(entity, IR_LINKAGE_CONSTANT);

	initializer = create_initializer_tarval(tv);
	set_entity_initializer(entity, initializer);

	pmap_insert(isa->constants, tv, entity);
	return entity;
}

static ir_node *gen_float_const(dbg_info *dbgi, ir_node *block, ir_tarval *tv)
{
	ir_entity *entity = create_float_const_entity(tv);
	ir_node   *hi     = new_bd_sparc_SetHi(dbgi, block, entity, 0);
	ir_node   *mem    = get_irg_no_mem(current_ir_graph);
	ir_mode   *mode   = get_tarval_mode(tv);
	ir_node   *new_op
		= create_ldf(dbgi, block, hi, mem, mode, entity, 0, false);
	ir_node   *proj   = new_r_Proj(new_op, mode, pn_sparc_Ldf_res);

	set_irn_pinned(new_op, op_pin_state_floats);
	return proj;
}

static ir_node *gen_Const(ir_node *node)
{
	ir_node   *block = be_transform_node(get_nodes_block(node));
	ir_mode   *mode  = get_irn_mode(node);
	dbg_info  *dbgi  = get_irn_dbg_info(node);
	ir_tarval *tv    = get_Const_tarval(node);
	long       value;

	if (mode_is_float(mode)) {
		return gen_float_const(dbgi, block, tv);
	}

	value = get_tarval_long(tv);
	if (value == 0) {
		return get_g0(get_irn_irg(node));
	} else if (sparc_is_value_imm_encodeable(value)) {
		ir_graph *irg = get_irn_irg(node);
		return new_bd_sparc_Or_imm(dbgi, block, get_g0(irg), NULL, value);
	} else {
		ir_node *hi = new_bd_sparc_SetHi(dbgi, block, NULL, value);
		if ((value & 0x3ff) != 0) {
			return new_bd_sparc_Or_imm(dbgi, block, hi, NULL, value & 0x3ff);
		} else {
			return hi;
		}
	}
}

static ir_node *gen_SwitchJmp(ir_node *node)
{
	dbg_info        *dbgi         = get_irn_dbg_info(node);
	ir_node         *block        = be_transform_node(get_nodes_block(node));
	ir_node         *selector     = get_Cond_selector(node);
	ir_node         *new_selector = be_transform_node(selector);
	long             default_pn   = get_Cond_default_proj(node);
	ir_entity       *entity;
	ir_node         *table_address;
	ir_node         *idx;
	ir_node         *load;
	ir_node         *address;

	/* switch with smaller mode not implemented yet */
	assert(get_mode_size_bits(get_irn_mode(selector)) == 32);

	entity = new_entity(NULL, id_unique("TBL%u"), get_unknown_type());
	set_entity_visibility(entity, ir_visibility_private);
	add_entity_linkage(entity, IR_LINKAGE_CONSTANT);

	/* construct base address */
	table_address = make_address(dbgi, block, entity, 0);
	/* scale index */
	idx = new_bd_sparc_Sll_imm(dbgi, block, new_selector, NULL, 2);
	/* load from jumptable */
	load = new_bd_sparc_Ld_reg(dbgi, block, table_address, idx,
	                           get_irg_no_mem(current_ir_graph),
	                           mode_gp);
	address = new_r_Proj(load, mode_gp, pn_sparc_Ld_res);

	return new_bd_sparc_SwitchJmp(dbgi, block, address, default_pn, entity);
}

static ir_node *gen_Cond(ir_node *node)
{
	ir_node    *selector = get_Cond_selector(node);
	ir_mode    *mode     = get_irn_mode(selector);
	ir_node    *cmp_left;
	ir_mode    *cmp_mode;
	ir_node    *block;
	ir_node    *flag_node;
	ir_relation relation;
	dbg_info   *dbgi;

	/* switch/case jumps */
	if (mode != mode_b) {
		return gen_SwitchJmp(node);
	}

	/* note: after lower_mode_b we are guaranteed to have a Cmp input */
	block       = be_transform_node(get_nodes_block(node));
	dbgi        = get_irn_dbg_info(node);
	cmp_left    = get_Cmp_left(selector);
	cmp_mode    = get_irn_mode(cmp_left);
	flag_node   = be_transform_node(selector);
	relation    = get_Cmp_relation(selector);
	if (mode_is_float(cmp_mode)) {
		return new_bd_sparc_fbfcc(dbgi, block, flag_node, relation);
	} else {
		bool is_unsigned = !mode_is_signed(cmp_mode);
		return new_bd_sparc_Bicc(dbgi, block, flag_node, relation, is_unsigned);
	}
}

/**
 * transform Cmp
 */
static ir_node *gen_Cmp(ir_node *node)
{
	ir_node *op1      = get_Cmp_left(node);
	ir_node *op2      = get_Cmp_right(node);
	ir_mode *cmp_mode = get_irn_mode(op1);
	assert(get_irn_mode(op2) == cmp_mode);

	if (mode_is_float(cmp_mode)) {
		ir_node  *block   = be_transform_node(get_nodes_block(node));
		dbg_info *dbgi    = get_irn_dbg_info(node);
		ir_node  *new_op1 = be_transform_node(op1);
		ir_node  *new_op2 = be_transform_node(op2);
		unsigned  bits    = get_mode_size_bits(cmp_mode);
		if (bits == 32) {
			return new_bd_sparc_fcmp_s(dbgi, block, new_op1, new_op2, cmp_mode);
		} else if (bits == 64) {
			return new_bd_sparc_fcmp_d(dbgi, block, new_op1, new_op2, cmp_mode);
		} else {
			assert(bits == 128);
			return new_bd_sparc_fcmp_q(dbgi, block, new_op1, new_op2, cmp_mode);
		}
	}

	/* when we compare a bitop like and,or,... with 0 then we can directly use
	 * the bitopcc variant.
	 * Currently we only do this when we're the only user of the node...
	 */
	if (is_Const(op2) && is_Const_null(op2) && get_irn_n_edges(op1) == 1) {
		if (is_And(op1)) {
			return gen_helper_bitop(op1,
			                        new_bd_sparc_AndCCZero_reg,
			                        new_bd_sparc_AndCCZero_imm,
			                        new_bd_sparc_AndNCCZero_reg,
			                        new_bd_sparc_AndNCCZero_imm,
			                        MATCH_NONE);
		} else if (is_Or(op1)) {
			return gen_helper_bitop(op1,
			                        new_bd_sparc_OrCCZero_reg,
			                        new_bd_sparc_OrCCZero_imm,
			                        new_bd_sparc_OrNCCZero_reg,
			                        new_bd_sparc_OrNCCZero_imm,
			                        MATCH_NONE);
		} else if (is_Eor(op1)) {
			return gen_helper_bitop(op1,
			                        new_bd_sparc_XorCCZero_reg,
			                        new_bd_sparc_XorCCZero_imm,
			                        new_bd_sparc_XNorCCZero_reg,
			                        new_bd_sparc_XNorCCZero_imm,
			                        MATCH_NONE);
		}
	}

	/* integer compare */
	return gen_helper_binop_args(node, op1, op2, MATCH_NONE,
	                             new_bd_sparc_Cmp_reg, new_bd_sparc_Cmp_imm);
}

/**
 * Transforms a SymConst node.
 */
static ir_node *gen_SymConst(ir_node *node)
{
	ir_entity *entity    = get_SymConst_entity(node);
	dbg_info  *dbgi      = get_irn_dbg_info(node);
	ir_node   *block     = get_nodes_block(node);
	ir_node   *new_block = be_transform_node(block);
	return make_address(dbgi, new_block, entity, 0);
}

static ir_node *create_fftof(dbg_info *dbgi, ir_node *block, ir_node *op,
                             ir_mode *src_mode, ir_mode *dst_mode)
{
	unsigned src_bits = get_mode_size_bits(src_mode);
	unsigned dst_bits = get_mode_size_bits(dst_mode);
	if (src_bits == 32) {
		if (dst_bits == 64) {
			return new_bd_sparc_fftof_s_d(dbgi, block, op, src_mode, dst_mode);
		} else {
			assert(dst_bits == 128);
			return new_bd_sparc_fftof_s_q(dbgi, block, op, src_mode, dst_mode);
		}
	} else if (src_bits == 64) {
		if (dst_bits == 32) {
			return new_bd_sparc_fftof_d_s(dbgi, block, op, src_mode, dst_mode);
		} else {
			assert(dst_bits == 128);
			return new_bd_sparc_fftof_d_q(dbgi, block, op, src_mode, dst_mode);
		}
	} else {
		assert(src_bits == 128);
		if (dst_bits == 32) {
			return new_bd_sparc_fftof_q_s(dbgi, block, op, src_mode, dst_mode);
		} else {
			assert(dst_bits == 64);
			return new_bd_sparc_fftof_q_d(dbgi, block, op, src_mode, dst_mode);
		}
	}
}

static ir_node *create_ftoi(dbg_info *dbgi, ir_node *block, ir_node *op,
                            ir_mode *src_mode)
{
	ir_node  *ftoi;
	unsigned  bits = get_mode_size_bits(src_mode);
	if (bits == 32) {
		ftoi = new_bd_sparc_fftoi_s(dbgi, block, op, src_mode);
	} else if (bits == 64) {
		ftoi = new_bd_sparc_fftoi_d(dbgi, block, op, src_mode);
	} else {
		assert(bits == 128);
		ftoi = new_bd_sparc_fftoi_q(dbgi, block, op, src_mode);
	}

	{
	ir_graph *irg   = get_irn_irg(block);
	ir_node  *sp    = get_irg_frame(irg);
	ir_node  *nomem = get_irg_no_mem(irg);
	ir_node  *stf   = create_stf(dbgi, block, ftoi, sp, nomem, src_mode,
	                             NULL, 0, true);
	ir_node  *ld    = new_bd_sparc_Ld_imm(dbgi, block, sp, stf, mode_gp,
	                                      NULL, 0, true);
	ir_node  *res   = new_r_Proj(ld, mode_gp, pn_sparc_Ld_res);
	set_irn_pinned(stf, op_pin_state_floats);
	set_irn_pinned(ld, op_pin_state_floats);
	return res;
	}
}

static ir_node *create_itof(dbg_info *dbgi, ir_node *block, ir_node *op,
                            ir_mode *dst_mode)
{
	ir_graph *irg   = get_irn_irg(block);
	ir_node  *sp    = get_irg_frame(irg);
	ir_node  *nomem = get_irg_no_mem(irg);
	ir_node  *st    = new_bd_sparc_St_imm(dbgi, block, op, sp, nomem,
	                                      mode_gp, NULL, 0, true);
	ir_node  *ldf   = new_bd_sparc_Ldf_s(dbgi, block, sp, st, mode_fp,
	                                     NULL, 0, true);
	ir_node  *res   = new_r_Proj(ldf, mode_fp, pn_sparc_Ldf_res);
	unsigned  bits  = get_mode_size_bits(dst_mode);
	set_irn_pinned(st, op_pin_state_floats);
	set_irn_pinned(ldf, op_pin_state_floats);

	if (bits == 32) {
		return new_bd_sparc_fitof_s(dbgi, block, res, dst_mode);
	} else if (bits == 64) {
		return new_bd_sparc_fitof_d(dbgi, block, res, dst_mode);
	} else {
		assert(bits == 128);
		return new_bd_sparc_fitof_q(dbgi, block, res, dst_mode);
	}
}

static ir_node *gen_Conv(ir_node *node)
{
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *op       = get_Conv_op(node);
	ir_mode  *src_mode = get_irn_mode(op);
	ir_mode  *dst_mode = get_irn_mode(node);
	dbg_info *dbgi     = get_irn_dbg_info(node);
	ir_node  *new_op;

	int src_bits = get_mode_size_bits(src_mode);
	int dst_bits = get_mode_size_bits(dst_mode);

	if (src_mode == mode_b)
		panic("ConvB not lowered %+F", node);

	if (src_mode == dst_mode)
		return be_transform_node(op);

	if (mode_is_float(src_mode) || mode_is_float(dst_mode)) {
		assert((src_bits <= 64 && dst_bits <= 64) && "quad FP not implemented");

		new_op = be_transform_node(op);
		if (mode_is_float(src_mode)) {
			if (mode_is_float(dst_mode)) {
				/* float -> float conv */
				return create_fftof(dbgi, block, new_op, src_mode, dst_mode);
			} else {
				/* float -> int conv */
				if (!mode_is_signed(dst_mode))
					panic("float to unsigned not implemented yet");
				return create_ftoi(dbgi, block, new_op, src_mode);
			}
		} else {
			/* int -> float conv */
			if (src_bits < 32) {
				new_op = gen_extension(dbgi, block, new_op, src_mode);
			} else if (src_bits == 32 && !mode_is_signed(src_mode)) {
				panic("unsigned to float not lowered!");
			}
			return create_itof(dbgi, block, new_op, dst_mode);
		}
	} else { /* complete in gp registers */
		int min_bits;
		ir_mode *min_mode;

		if (src_bits == dst_bits || dst_mode == mode_b) {
			/* kill unnecessary conv */
			return be_transform_node(op);
		}

		if (src_bits < dst_bits) {
			min_bits = src_bits;
			min_mode = src_mode;
		} else {
			min_bits = dst_bits;
			min_mode = dst_mode;
		}

		if (upper_bits_clean(op, min_mode)) {
			return be_transform_node(op);
		}
		new_op = be_transform_node(op);

		if (mode_is_signed(min_mode)) {
			return gen_sign_extension(dbgi, block, new_op, min_bits);
		} else {
			return gen_zero_extension(dbgi, block, new_op, min_bits);
		}
	}
}

static ir_node *gen_Unknown(ir_node *node)
{
	/* just produce a 0 */
	ir_mode *mode = get_irn_mode(node);
	if (mode_is_float(mode)) {
		ir_node *block = be_transform_node(get_nodes_block(node));
		return gen_float_const(NULL, block, get_mode_null(mode));
	} else if (mode_needs_gp_reg(mode)) {
		ir_graph *irg = get_irn_irg(node);
		return get_g0(irg);
	}

	panic("Unexpected Unknown mode");
}

/**
 * transform the start node to the prolog code
 */
static ir_node *gen_Start(ir_node *node)
{
	ir_graph  *irg           = get_irn_irg(node);
	ir_entity *entity        = get_irg_entity(irg);
	ir_type   *function_type = get_entity_type(entity);
	ir_node   *block         = get_nodes_block(node);
	ir_node   *new_block     = be_transform_node(block);
	dbg_info  *dbgi          = get_irn_dbg_info(node);
	struct obstack *obst     = be_get_be_obst(irg);
	const arch_register_req_t *req;
	size_t     n_outs;
	ir_node   *start;
	size_t     i;
	size_t     o;

	/* start building list of start constraints */
	assert(obstack_object_size(obst) == 0);

	/* calculate number of outputs */
	n_outs = 4; /* memory, g0, g7, sp */
	if (!current_cconv->omit_fp)
		++n_outs; /* framepointer */
	/* function parameters */
	n_outs += current_cconv->n_param_regs;
	/* callee saves */
	if (current_cconv->omit_fp) {
		n_outs += ARRAY_SIZE(omit_fp_callee_saves);
	}

	start = new_bd_sparc_Start(dbgi, new_block, n_outs);

	o = 0;

	/* first output is memory */
	start_mem_offset = o;
	arch_set_irn_register_req_out(start, o, arch_no_register_req);
	++o;

	/* the zero register */
	start_g0_offset = o;
	req = be_create_reg_req(obst, &sparc_registers[REG_G0],
	                        arch_register_req_type_ignore);
	arch_set_irn_register_req_out(start, o, req);
	arch_set_irn_register_out(start, o, &sparc_registers[REG_G0]);
	++o;

	/* g7 is used for TLS data */
	start_g7_offset = o;
	req = be_create_reg_req(obst, &sparc_registers[REG_G7],
	                        arch_register_req_type_ignore);
	arch_set_irn_register_req_out(start, o, req);
	arch_set_irn_register_out(start, o, &sparc_registers[REG_G7]);
	++o;

	/* we need an output for the stackpointer */
	start_sp_offset = o;
	req = be_create_reg_req(obst, sp_reg,
			arch_register_req_type_produces_sp | arch_register_req_type_ignore);
	arch_set_irn_register_req_out(start, o, req);
	arch_set_irn_register_out(start, o, sp_reg);
	++o;

	if (!current_cconv->omit_fp) {
		start_fp_offset = o;
		req = be_create_reg_req(obst, fp_reg, arch_register_req_type_ignore);
		arch_set_irn_register_req_out(start, o, req);
		arch_set_irn_register_out(start, o, fp_reg);
		++o;
	}

	/* function parameters in registers */
	start_params_offset = o;
	for (i = 0; i < get_method_n_params(function_type); ++i) {
		const reg_or_stackslot_t *param = &current_cconv->parameters[i];
		const arch_register_t    *reg0  = param->reg0;
		const arch_register_t    *reg1  = param->reg1;
		if (reg0 != NULL) {
			arch_set_irn_register_req_out(start, o, reg0->single_req);
			arch_set_irn_register_out(start, o, reg0);
			++o;
		}
		if (reg1 != NULL) {
			arch_set_irn_register_req_out(start, o, reg1->single_req);
			arch_set_irn_register_out(start, o, reg1);
			++o;
		}
	}
	/* we need the values of the callee saves (Note: non omit-fp mode has no
	 * callee saves) */
	start_callee_saves_offset = o;
	if (current_cconv->omit_fp) {
		size_t n_callee_saves = ARRAY_SIZE(omit_fp_callee_saves);
		size_t c;
		for (c = 0; c < n_callee_saves; ++c) {
			const arch_register_t *reg = omit_fp_callee_saves[c];
			arch_set_irn_register_req_out(start, o, reg->single_req);
			arch_set_irn_register_out(start, o, reg);
			++o;
		}
	}
	assert(n_outs == o);

	return start;
}

static ir_node *get_initial_sp(ir_graph *irg)
{
	if (start_sp == NULL) {
		ir_node *start = get_irg_start(irg);
		start_sp = new_r_Proj(start, mode_gp, start_sp_offset);
	}
	return start_sp;
}

static ir_node *get_initial_fp(ir_graph *irg)
{
	if (start_fp == NULL) {
		ir_node *start = get_irg_start(irg);
		start_fp = new_r_Proj(start, mode_gp, start_fp_offset);
	}
	return start_fp;
}

static ir_node *get_initial_mem(ir_graph *irg)
{
	if (start_mem == NULL) {
		ir_node *start = get_irg_start(irg);
		start_mem = new_r_Proj(start, mode_M, start_mem_offset);
	}
	return start_mem;
}

static ir_node *get_stack_pointer_for(ir_node *node)
{
	/* get predecessor in stack_order list */
	ir_node *stack_pred = be_get_stack_pred(stackorder, node);
	ir_node *stack;

	if (stack_pred == NULL) {
		/* first stack user in the current block. We can simply use the
		 * initial sp_proj for it */
		ir_graph *irg = get_irn_irg(node);
		return get_initial_sp(irg);
	}

	be_transform_node(stack_pred);
	stack = (ir_node*)pmap_get(node_to_stack, stack_pred);
	if (stack == NULL) {
		return get_stack_pointer_for(stack_pred);
	}

	return stack;
}

/**
 * transform a Return node into epilogue code + return statement
 */
static ir_node *gen_Return(ir_node *node)
{
	ir_node  *block     = get_nodes_block(node);
	ir_graph *irg       = get_irn_irg(node);
	ir_node  *new_block = be_transform_node(block);
	dbg_info *dbgi      = get_irn_dbg_info(node);
	ir_node  *mem       = get_Return_mem(node);
	ir_node  *new_mem   = be_transform_node(mem);
	ir_node  *sp        = get_stack_pointer_for(node);
	size_t    n_res     = get_Return_n_ress(node);
	struct obstack *be_obst = be_get_be_obst(irg);
	ir_node  *bereturn;
	ir_node **in;
	const arch_register_req_t **reqs;
	size_t    i;
	size_t    p;
	size_t    n_ins;

	/* estimate number of return values */
	n_ins = 2 + n_res; /* memory + stackpointer, return values */
	if (current_cconv->omit_fp)
		n_ins += ARRAY_SIZE(omit_fp_callee_saves);

	in   = ALLOCAN(ir_node*, n_ins);
	reqs = OALLOCN(be_obst, const arch_register_req_t*, n_ins);
	p    = 0;

	in[p]   = new_mem;
	reqs[p] = arch_no_register_req;
	++p;

	in[p]   = sp;
	reqs[p] = sp_reg->single_req;
	++p;

	/* result values */
	for (i = 0; i < n_res; ++i) {
		ir_node                  *res_value     = get_Return_res(node, i);
		ir_node                  *new_res_value = be_transform_node(res_value);
		const reg_or_stackslot_t *slot          = &current_cconv->results[i];
		assert(slot->req1 == NULL);
		in[p]   = new_res_value;
		reqs[p] = slot->req0;
		++p;
	}
	/* callee saves */
	if (current_cconv->omit_fp) {
		ir_node  *start          = get_irg_start(irg);
		size_t    n_callee_saves = ARRAY_SIZE(omit_fp_callee_saves);
		for (i = 0; i < n_callee_saves; ++i) {
			const arch_register_t *reg   = omit_fp_callee_saves[i];
			ir_mode               *mode  = reg->reg_class->mode;
			ir_node               *value
					= new_r_Proj(start, mode, i + start_callee_saves_offset);
			in[p]   = value;
			reqs[p] = reg->single_req;
			++p;
		}
	}
	assert(p == n_ins);

	bereturn = new_bd_sparc_Return_reg(dbgi, new_block, n_ins, in);
	arch_set_irn_register_reqs_in(bereturn, reqs);

	return bereturn;
}

static ir_node *bitcast_int_to_float(dbg_info *dbgi, ir_node *block,
                                     ir_node *value0, ir_node *value1)
{
	ir_graph *irg   = current_ir_graph;
	ir_node  *sp    = get_irg_frame(irg);
	ir_node  *nomem = get_irg_no_mem(irg);
	ir_node  *st    = new_bd_sparc_St_imm(dbgi, block, value0, sp, nomem,
	                                      mode_gp, NULL, 0, true);
	ir_mode  *mode;
	ir_node  *ldf;
	ir_node  *mem;
	set_irn_pinned(st, op_pin_state_floats);

	if (value1 != NULL) {
		ir_node *st1 = new_bd_sparc_St_imm(dbgi, block, value1, sp, nomem,
		                                   mode_gp, NULL, 4, true);
		ir_node *in[2] = { st, st1 };
		ir_node *sync  = new_r_Sync(block, 2, in);
		set_irn_pinned(st1, op_pin_state_floats);
		mem  = sync;
		mode = mode_fp2;
	} else {
		mem  = st;
		mode = mode_fp;
	}

	ldf = create_ldf(dbgi, block, sp, mem, mode, NULL, 0, true);
	set_irn_pinned(ldf, op_pin_state_floats);

	return new_r_Proj(ldf, mode, pn_sparc_Ldf_res);
}

static void bitcast_float_to_int(dbg_info *dbgi, ir_node *block,
                                 ir_node *node, ir_mode *float_mode,
                                 ir_node **result)
{
	ir_graph *irg   = current_ir_graph;
	ir_node  *stack = get_irg_frame(irg);
	ir_node  *nomem = get_irg_no_mem(irg);
	ir_node  *stf   = create_stf(dbgi, block, node, stack, nomem, float_mode,
	                             NULL, 0, true);
	int       bits  = get_mode_size_bits(float_mode);
	ir_node  *ld;
	set_irn_pinned(stf, op_pin_state_floats);

	ld = new_bd_sparc_Ld_imm(dbgi, block, stack, stf, mode_gp, NULL, 0, true);
	set_irn_pinned(ld, op_pin_state_floats);
	result[0] = new_r_Proj(ld, mode_gp, pn_sparc_Ld_res);

	if (bits == 64) {
		ir_node *ld2 = new_bd_sparc_Ld_imm(dbgi, block, stack, stf, mode_gp,
		                                   NULL, 4, true);
		set_irn_pinned(ld, op_pin_state_floats);
		result[1] = new_r_Proj(ld2, mode_gp, pn_sparc_Ld_res);

		arch_add_irn_flags(ld, (arch_irn_flags_t)sparc_arch_irn_flag_needs_64bit_spillslot);
		arch_add_irn_flags(ld2, (arch_irn_flags_t)sparc_arch_irn_flag_needs_64bit_spillslot);
	} else {
		assert(bits == 32);
		result[1] = NULL;
	}
}

static ir_node *gen_Call(ir_node *node)
{
	ir_graph        *irg          = get_irn_irg(node);
	ir_node         *callee       = get_Call_ptr(node);
	ir_node         *block        = get_nodes_block(node);
	ir_node         *new_block    = be_transform_node(block);
	ir_node         *mem          = get_Call_mem(node);
	ir_node         *new_mem      = be_transform_node(mem);
	dbg_info        *dbgi         = get_irn_dbg_info(node);
	ir_type         *type         = get_Call_type(node);
	size_t           n_params     = get_Call_n_params(node);
	size_t           n_ress       = get_method_n_ress(type);
	/* max inputs: memory, callee, register arguments */
	ir_node        **sync_ins     = ALLOCAN(ir_node*, n_params);
	struct obstack  *obst         = be_get_be_obst(irg);
	calling_convention_t *cconv
		= sparc_decide_calling_convention(type, NULL);
	size_t           n_param_regs = cconv->n_param_regs;
	/* param-regs + mem + stackpointer + callee */
	unsigned         max_inputs   = 3 + n_param_regs;
	ir_node        **in           = ALLOCAN(ir_node*, max_inputs);
	const arch_register_req_t **in_req
		= OALLOCNZ(obst, const arch_register_req_t*, max_inputs);
	int              in_arity     = 0;
	int              sync_arity   = 0;
	int              n_caller_saves
		= rbitset_popcount(cconv->caller_saves, N_SPARC_REGISTERS);
	ir_entity       *entity       = NULL;
	ir_node         *new_frame    = get_stack_pointer_for(node);
	bool             aggregate_return
		= get_method_calling_convention(type) & cc_compound_ret;
	ir_node         *incsp;
	int              mem_pos;
	ir_node         *res;
	size_t           p;
	size_t           r;
	int              i;
	int              o;
	int              out_arity;

	assert(n_params == get_method_n_params(type));

	/* construct arguments */

	/* memory input */
	in_req[in_arity] = arch_no_register_req;
	mem_pos          = in_arity;
	++in_arity;

	/* stack pointer input */
	/* construct an IncSP -> we have to always be sure that the stack is
	 * aligned even if we don't push arguments on it */
	incsp = be_new_IncSP(sp_reg, new_block, new_frame,
	                     cconv->param_stack_size, 1);
	in_req[in_arity] = sp_reg->single_req;
	in[in_arity]     = incsp;
	++in_arity;

	/* parameters */
	for (p = 0; p < n_params; ++p) {
		ir_node                  *value      = get_Call_param(node, p);
		ir_node                  *new_value  = be_transform_node(value);
		const reg_or_stackslot_t *param      = &cconv->parameters[p];
		ir_type                  *param_type = get_method_param_type(type, p);
		ir_mode                  *mode       = get_type_mode(param_type);
		ir_node                  *new_values[2];
		ir_node                  *str;
		int                       offset;

		if (mode_is_float(mode) && param->reg0 != NULL) {
			unsigned size_bits = get_mode_size_bits(mode);
			assert(size_bits <= 64);
			bitcast_float_to_int(dbgi, new_block, new_value, mode, new_values);
		} else {
			new_values[0] = new_value;
			new_values[1] = NULL;
		}

		/* put value into registers */
		if (param->reg0 != NULL) {
			in[in_arity]     = new_values[0];
			in_req[in_arity] = param->reg0->single_req;
			++in_arity;
			if (new_values[1] == NULL)
				continue;
		}
		if (param->reg1 != NULL) {
			assert(new_values[1] != NULL);
			in[in_arity]     = new_values[1];
			in_req[in_arity] = param->reg1->single_req;
			++in_arity;
			continue;
		}

		/* we need a store if we're here */
		if (new_values[1] != NULL) {
			new_value = new_values[1];
			mode      = mode_gp;
		}

		/* we need to skip over our save area when constructing the call
		 * arguments on stack */
		offset = param->offset + SPARC_MIN_STACKSIZE;

		if (mode_is_float(mode)) {
			str = create_stf(dbgi, new_block, new_value, incsp, new_mem,
			                 mode, NULL, offset, true);
		} else {
			str = new_bd_sparc_St_imm(dbgi, new_block, new_value, incsp,
			                          new_mem, mode, NULL, offset, true);
		}
		set_irn_pinned(str, op_pin_state_floats);
		sync_ins[sync_arity++] = str;
	}

	/* construct memory input */
	if (sync_arity == 0) {
		in[mem_pos] = new_mem;
	} else if (sync_arity == 1) {
		in[mem_pos] = sync_ins[0];
	} else {
		in[mem_pos] = new_rd_Sync(NULL, new_block, sync_arity, sync_ins);
	}

	if (is_SymConst(callee)) {
		entity = get_SymConst_entity(callee);
	} else {
		in[in_arity]     = be_transform_node(callee);
		in_req[in_arity] = sparc_reg_classes[CLASS_sparc_gp].class_req;
		++in_arity;
	}
	assert(in_arity <= (int)max_inputs);

	/* outputs:
	 *  - memory
	 *  - results
	 *  - caller saves
	 */
	out_arity = 1 + cconv->n_reg_results + n_caller_saves;

	/* create call node */
	if (entity != NULL) {
		res = new_bd_sparc_Call_imm(dbgi, new_block, in_arity, in, out_arity,
		                            entity, 0, aggregate_return);
	} else {
		res = new_bd_sparc_Call_reg(dbgi, new_block, in_arity, in, out_arity,
		                            aggregate_return);
	}
	arch_set_irn_register_reqs_in(res, in_req);

	/* create output register reqs */
	o = 0;
	arch_set_irn_register_req_out(res, o++, arch_no_register_req);
	/* add register requirements for the result regs */
	for (r = 0; r < n_ress; ++r) {
		const reg_or_stackslot_t  *result_info = &cconv->results[r];
		const arch_register_req_t *req         = result_info->req0;
		if (req != NULL) {
			arch_set_irn_register_req_out(res, o++, req);
		}
		assert(result_info->req1 == NULL);
	}
	for (i = 0; i < N_SPARC_REGISTERS; ++i) {
		const arch_register_t *reg;
		if (!rbitset_is_set(cconv->caller_saves, i))
			continue;
		reg = &sparc_registers[i];
		arch_set_irn_register_req_out(res, o++, reg->single_req);
	}
	assert(o == out_arity);

	/* copy pinned attribute */
	set_irn_pinned(res, get_irn_pinned(node));

	/* IncSP to destroy the call stackframe */
	incsp = be_new_IncSP(sp_reg, new_block, incsp, -cconv->param_stack_size, 0);
	/* if we are the last IncSP producer in a block then we have to keep
	 * the stack value.
	 * Note: This here keeps all producers which is more than necessary */
	add_irn_dep(incsp, res);
	keep_alive(incsp);

	pmap_insert(node_to_stack, node, incsp);

	sparc_free_calling_convention(cconv);
	return res;
}

static ir_node *gen_Sel(ir_node *node)
{
	dbg_info  *dbgi      = get_irn_dbg_info(node);
	ir_node   *block     = get_nodes_block(node);
	ir_node   *new_block = be_transform_node(block);
	ir_node   *ptr       = get_Sel_ptr(node);
	ir_node   *new_ptr   = be_transform_node(ptr);
	ir_entity *entity    = get_Sel_entity(node);

	/* must be the frame pointer all other sels must have been lowered
	 * already */
	assert(is_Proj(ptr) && is_Start(get_Proj_pred(ptr)));

	return new_bd_sparc_FrameAddr(dbgi, new_block, new_ptr, entity, 0);
}

static ir_node *gen_Alloc(ir_node *node)
{
	dbg_info *dbgi       = get_irn_dbg_info(node);
	ir_node  *block      = get_nodes_block(node);
	ir_node  *new_block  = be_transform_node(block);
	ir_type  *type       = get_Alloc_type(node);
	ir_node  *size       = get_Alloc_count(node);
	ir_node  *stack_pred = get_stack_pointer_for(node);
	ir_node  *subsp;
	if (get_Alloc_where(node) != stack_alloc)
		panic("only stack-alloc supported in sparc backend (at %+F)", node);
	/* lowerer should have transformed all allocas to byte size */
	if (type != get_unknown_type() && get_type_size_bytes(type) != 1)
		panic("Found non-byte alloc in sparc backend (at %+F)", node);

	if (is_Const(size)) {
		ir_tarval *tv    = get_Const_tarval(size);
		long       sizel = get_tarval_long(tv);
		subsp = be_new_IncSP(sp_reg, new_block, stack_pred, sizel, 0);
		set_irn_dbg_info(subsp, dbgi);
	} else {
		ir_node *new_size = be_transform_node(size);
		subsp = new_bd_sparc_SubSP(dbgi, new_block, stack_pred, new_size);
		arch_set_irn_register(subsp, sp_reg);
	}

	/* if we are the last IncSP producer in a block then we have to keep
	 * the stack value.
	 * Note: This here keeps all producers which is more than necessary */
	keep_alive(subsp);

	pmap_insert(node_to_stack, node, subsp);
	/* the "result" is the unmodified sp value */
	return stack_pred;
}

static ir_node *gen_Proj_Alloc(ir_node *node)
{
	ir_node *alloc = get_Proj_pred(node);
	long     pn    = get_Proj_proj(node);

	switch ((pn_Alloc)pn) {
	case pn_Alloc_M: {
		ir_node *alloc_mem = get_Alloc_mem(alloc);
		return be_transform_node(alloc_mem);
	}
	case pn_Alloc_res: {
		ir_node *new_alloc = be_transform_node(alloc);
		return new_alloc;
	}
	case pn_Alloc_X_regular:
	case pn_Alloc_X_except:
		panic("sparc backend: exception output of alloc not supported (at %+F)",
		      node);
	}
	panic("sparc backend: invalid Proj->Alloc");
}

static ir_node *gen_Free(ir_node *node)
{
	dbg_info *dbgi       = get_irn_dbg_info(node);
	ir_node  *block      = get_nodes_block(node);
	ir_node  *new_block  = be_transform_node(block);
	ir_type  *type       = get_Free_type(node);
	ir_node  *size       = get_Free_count(node);
	ir_node  *mem        = get_Free_mem(node);
	ir_node  *new_mem    = be_transform_node(mem);
	ir_node  *stack_pred = get_stack_pointer_for(node);
	ir_node  *addsp;
	if (get_Alloc_where(node) != stack_alloc)
		panic("only stack-alloc supported in sparc backend (at %+F)", node);
	/* lowerer should have transformed all allocas to byte size */
	if (type != get_unknown_type() && get_type_size_bytes(type) != 1)
		panic("Found non-byte alloc in sparc backend (at %+F)", node);

	if (is_Const(size)) {
		ir_tarval *tv    = get_Const_tarval(size);
		long       sizel = get_tarval_long(tv);
		addsp = be_new_IncSP(sp_reg, new_block, stack_pred, -sizel, 0);
		set_irn_dbg_info(addsp, dbgi);
	} else {
		ir_node *new_size = be_transform_node(size);
		addsp = new_bd_sparc_AddSP(dbgi, new_block, stack_pred, new_size);
		arch_set_irn_register(addsp, sp_reg);
	}

	/* if we are the last IncSP producer in a block then we have to keep
	 * the stack value.
	 * Note: This here keeps all producers which is more than necessary */
	keep_alive(addsp);

	pmap_insert(node_to_stack, node, addsp);
	/* the "result" is the unmodified sp value */
	return new_mem;
}

static const arch_register_req_t float1_req = {
	arch_register_req_type_normal,
	&sparc_reg_classes[CLASS_sparc_fp],
	NULL,
	0,
	0,
	1
};
static const arch_register_req_t float2_req = {
	arch_register_req_type_normal | arch_register_req_type_aligned,
	&sparc_reg_classes[CLASS_sparc_fp],
	NULL,
	0,
	0,
	2
};
static const arch_register_req_t float4_req = {
	arch_register_req_type_normal | arch_register_req_type_aligned,
	&sparc_reg_classes[CLASS_sparc_fp],
	NULL,
	0,
	0,
	4
};


static const arch_register_req_t *get_float_req(ir_mode *mode)
{
	unsigned bits = get_mode_size_bits(mode);

	assert(mode_is_float(mode));
	if (bits == 32) {
		return &float1_req;
	} else if (bits == 64) {
		return &float2_req;
	} else {
		assert(bits == 128);
		return &float4_req;
	}
}

/**
 * Transform some Phi nodes
 */
static ir_node *gen_Phi(ir_node *node)
{
	const arch_register_req_t *req;
	ir_node  *block = be_transform_node(get_nodes_block(node));
	ir_graph *irg   = current_ir_graph;
	dbg_info *dbgi  = get_irn_dbg_info(node);
	ir_mode  *mode  = get_irn_mode(node);
	ir_node  *phi;

	if (mode_needs_gp_reg(mode)) {
		/* we shouldn't have any 64bit stuff around anymore */
		assert(get_mode_size_bits(mode) <= 32);
		/* all integer operations are on 32bit registers now */
		mode = mode_gp;
		req  = sparc_reg_classes[CLASS_sparc_gp].class_req;
	} else if (mode_is_float(mode)) {
		mode = mode;
		req  = get_float_req(mode);
	} else {
		req = arch_no_register_req;
	}

	/* phi nodes allow loops, so we use the old arguments for now
	 * and fix this later */
	phi = new_ir_node(dbgi, irg, block, op_Phi, mode, get_irn_arity(node), get_irn_in(node) + 1);
	copy_node_attr(irg, node, phi);
	be_duplicate_deps(node, phi);
	arch_set_irn_register_req_out(phi, 0, req);
	be_enqueue_preds(node);
	return phi;
}

/**
 * Transform a Proj from a Load.
 */
static ir_node *gen_Proj_Load(ir_node *node)
{
	ir_node  *load     = get_Proj_pred(node);
	ir_node  *new_load = be_transform_node(load);
	dbg_info *dbgi     = get_irn_dbg_info(node);
	long      pn       = get_Proj_proj(node);

	/* renumber the proj */
	switch (get_sparc_irn_opcode(new_load)) {
	case iro_sparc_Ld:
		/* handle all gp loads equal: they have the same proj numbers. */
		if (pn == pn_Load_res) {
			return new_rd_Proj(dbgi, new_load, mode_gp, pn_sparc_Ld_res);
		} else if (pn == pn_Load_M) {
			return new_rd_Proj(dbgi, new_load, mode_M, pn_sparc_Ld_M);
		}
		break;
	case iro_sparc_Ldf:
		if (pn == pn_Load_res) {
			const sparc_load_store_attr_t *attr
				= get_sparc_load_store_attr_const(new_load);
			ir_mode *mode = attr->load_store_mode;
			return new_rd_Proj(dbgi, new_load, mode, pn_sparc_Ldf_res);
		} else if (pn == pn_Load_M) {
			return new_rd_Proj(dbgi, new_load, mode_M, pn_sparc_Ld_M);
		}
		break;
	default:
		break;
	}
	panic("Unsupported Proj from Load");
}

static ir_node *gen_Proj_Store(ir_node *node)
{
	ir_node  *store     = get_Proj_pred(node);
	ir_node  *new_store = be_transform_node(store);
	long      pn        = get_Proj_proj(node);

	/* renumber the proj */
	switch (get_sparc_irn_opcode(new_store)) {
	case iro_sparc_St:
		if (pn == pn_Store_M) {
			return new_store;
		}
		break;
	case iro_sparc_Stf:
		if (pn == pn_Store_M) {
			return new_store;
		}
		break;
	default:
		break;
	}
	panic("Unsupported Proj from Store");
}

/**
 * Transform the Projs from a Cmp.
 */
static ir_node *gen_Proj_Cmp(ir_node *node)
{
	(void) node;
	panic("not implemented");
}

/**
 * transform Projs from a Div
 */
static ir_node *gen_Proj_Div(ir_node *node)
{
	ir_node  *pred     = get_Proj_pred(node);
	ir_node  *new_pred = be_transform_node(pred);
	long      pn       = get_Proj_proj(node);
	ir_mode  *res_mode;

	if (is_sparc_SDiv(new_pred) || is_sparc_UDiv(new_pred)) {
		res_mode = mode_gp;
	} else if (is_sparc_fdiv(new_pred)) {
		res_mode = get_Div_resmode(pred);
	} else {
		panic("sparc backend: Div transformed to something unexpected: %+F",
		      new_pred);
	}
	assert((int)pn_sparc_SDiv_res == (int)pn_sparc_UDiv_res);
	assert((int)pn_sparc_SDiv_M   == (int)pn_sparc_UDiv_M);
	assert((int)pn_sparc_SDiv_res == (int)pn_sparc_fdiv_res);
	assert((int)pn_sparc_SDiv_M   == (int)pn_sparc_fdiv_M);
	switch (pn) {
	case pn_Div_res:
		return new_r_Proj(new_pred, res_mode, pn_sparc_SDiv_res);
	case pn_Div_M:
		return new_r_Proj(new_pred, mode_gp, pn_sparc_SDiv_M);
	default:
		break;
	}
	panic("Unsupported Proj from Div");
}

static ir_node *get_frame_base(ir_graph *irg)
{
	if (frame_base == NULL) {
		if (current_cconv->omit_fp) {
			frame_base = get_initial_sp(irg);
		} else {
			frame_base = get_initial_fp(irg);
		}
	}
	return frame_base;
}

static ir_node *gen_Proj_Start(ir_node *node)
{
	ir_node *block     = get_nodes_block(node);
	ir_node *new_block = be_transform_node(block);
	long     pn        = get_Proj_proj(node);
	/* make sure prolog is constructed */
	be_transform_node(get_Proj_pred(node));

	switch ((pn_Start) pn) {
	case pn_Start_X_initial_exec:
		/* exchange ProjX with a jump */
		return new_bd_sparc_Ba(NULL, new_block);
	case pn_Start_M: {
		ir_graph *irg = get_irn_irg(node);
		return get_initial_mem(irg);
	}
	case pn_Start_T_args:
		return new_r_Bad(get_irn_irg(block), mode_T);
	case pn_Start_P_frame_base:
		return get_frame_base(get_irn_irg(block));
	}
	panic("Unexpected start proj: %ld\n", pn);
}

static ir_node *gen_Proj_Proj_Start(ir_node *node)
{
	long      pn        = get_Proj_proj(node);
	ir_node  *block     = get_nodes_block(node);
	ir_graph *irg       = get_irn_irg(node);
	ir_node  *new_block = be_transform_node(block);
	ir_node  *args      = get_Proj_pred(node);
	ir_node  *start     = get_Proj_pred(args);
	ir_node  *new_start = be_transform_node(start);
	const reg_or_stackslot_t *param;

	/* Proj->Proj->Start must be a method argument */
	assert(get_Proj_proj(get_Proj_pred(node)) == pn_Start_T_args);

	param = &current_cconv->parameters[pn];

	if (param->reg0 != NULL) {
		/* argument transmitted in register */
		const arch_register_t *reg      = param->reg0;
		ir_mode               *reg_mode = reg->reg_class->mode;
		long                   new_pn   = param->reg_offset + start_params_offset;
		ir_node               *value    = new_r_Proj(new_start, reg_mode, new_pn);
		bool                   is_float = false;

		{
			ir_entity *entity      = get_irg_entity(irg);
			ir_type   *method_type = get_entity_type(entity);
			if (pn < (long)get_method_n_params(method_type)) {
				ir_type *param_type = get_method_param_type(method_type, pn);
				ir_mode *mode       = get_type_mode(param_type);
				is_float = mode_is_float(mode);
			}
		}

		if (is_float) {
			const arch_register_t *reg1 = param->reg1;
			ir_node *value1 = NULL;

			if (reg1 != NULL) {
				ir_mode *reg1_mode = reg1->reg_class->mode;
				value1 = new_r_Proj(new_start, reg1_mode, new_pn+1);
			} else if (param->entity != NULL) {
				ir_node *fp  = get_initial_fp(irg);
				ir_node *mem = get_initial_mem(irg);
				ir_node *ld  = new_bd_sparc_Ld_imm(NULL, new_block, fp, mem,
				                                   mode_gp, param->entity,
				                                   0, true);
				value1 = new_r_Proj(ld, mode_gp, pn_sparc_Ld_res);
			}

			/* convert integer value to float */
			value = bitcast_int_to_float(NULL, new_block, value, value1);
		}
		return value;
	} else {
		/* argument transmitted on stack */
		ir_node *mem  = get_initial_mem(irg);
		ir_mode *mode = get_type_mode(param->type);
		ir_node *base = get_frame_base(irg);
		ir_node *load;
		ir_node *value;

		if (mode_is_float(mode)) {
			load  = create_ldf(NULL, new_block, base, mem, mode,
			                   param->entity, 0, true);
			value = new_r_Proj(load, mode_fp, pn_sparc_Ldf_res);
		} else {
			load  = new_bd_sparc_Ld_imm(NULL, new_block, base, mem, mode,
			                            param->entity, 0, true);
			value = new_r_Proj(load, mode_gp, pn_sparc_Ld_res);
		}
		set_irn_pinned(load, op_pin_state_floats);

		return value;
	}
}

static ir_node *gen_Proj_Call(ir_node *node)
{
	long     pn        = get_Proj_proj(node);
	ir_node *call      = get_Proj_pred(node);
	ir_node *new_call  = be_transform_node(call);

	switch ((pn_Call) pn) {
	case pn_Call_M:
		return new_r_Proj(new_call, mode_M, 0);
	case pn_Call_X_regular:
	case pn_Call_X_except:
	case pn_Call_T_result:
		break;
	}
	panic("Unexpected Call proj %ld\n", pn);
}

static ir_node *gen_Proj_Proj_Call(ir_node *node)
{
	long                  pn            = get_Proj_proj(node);
	ir_node              *call          = get_Proj_pred(get_Proj_pred(node));
	ir_node              *new_call      = be_transform_node(call);
	ir_type              *function_type = get_Call_type(call);
	calling_convention_t *cconv
		= sparc_decide_calling_convention(function_type, NULL);
	const reg_or_stackslot_t  *res  = &cconv->results[pn];
	ir_mode                   *mode = get_irn_mode(node);
	long                       new_pn = 1 + res->reg_offset;

	assert(res->req0 != NULL && res->req1 == NULL);
	if (mode_needs_gp_reg(mode)) {
		mode = mode_gp;
	}
	sparc_free_calling_convention(cconv);

	return new_r_Proj(new_call, mode, new_pn);
}

/**
 * Transform a Proj node.
 */
static ir_node *gen_Proj(ir_node *node)
{
	ir_node *pred = get_Proj_pred(node);

	switch (get_irn_opcode(pred)) {
	case iro_Alloc:
		return gen_Proj_Alloc(node);
	case iro_Store:
		return gen_Proj_Store(node);
	case iro_Load:
		return gen_Proj_Load(node);
	case iro_Call:
		return gen_Proj_Call(node);
	case iro_Cmp:
		return gen_Proj_Cmp(node);
	case iro_Cond:
		return be_duplicate_node(node);
	case iro_Div:
		return gen_Proj_Div(node);
	case iro_Start:
		return gen_Proj_Start(node);
	case iro_Proj: {
		ir_node *pred_pred = get_Proj_pred(pred);
		if (is_Call(pred_pred)) {
			return gen_Proj_Proj_Call(node);
		} else if (is_Start(pred_pred)) {
			return gen_Proj_Proj_Start(node);
		}
		/* FALLTHROUGH */
	}
	default:
		if (is_sparc_AddCC_t(pred)) {
			return gen_Proj_AddCC_t(node);
		} else if (is_sparc_SubCC_t(pred)) {
			return gen_Proj_SubCC_t(node);
		}
		panic("code selection didn't expect Proj after %+F\n", pred);
	}
}

/**
 * transform a Jmp
 */
static ir_node *gen_Jmp(ir_node *node)
{
	ir_node  *block     = get_nodes_block(node);
	ir_node  *new_block = be_transform_node(block);
	dbg_info *dbgi      = get_irn_dbg_info(node);

	return new_bd_sparc_Ba(dbgi, new_block);
}

/**
 * configure transformation callbacks
 */
static void sparc_register_transformers(void)
{
	be_start_transform_setup();

	be_set_transform_function(op_Add,          gen_Add);
	be_set_transform_function(op_Alloc,        gen_Alloc);
	be_set_transform_function(op_And,          gen_And);
	be_set_transform_function(op_Call,         gen_Call);
	be_set_transform_function(op_Cmp,          gen_Cmp);
	be_set_transform_function(op_Cond,         gen_Cond);
	be_set_transform_function(op_Const,        gen_Const);
	be_set_transform_function(op_Conv,         gen_Conv);
	be_set_transform_function(op_Div,          gen_Div);
	be_set_transform_function(op_Eor,          gen_Eor);
	be_set_transform_function(op_Free,         gen_Free);
	be_set_transform_function(op_Jmp,          gen_Jmp);
	be_set_transform_function(op_Load,         gen_Load);
	be_set_transform_function(op_Minus,        gen_Minus);
	be_set_transform_function(op_Mul,          gen_Mul);
	be_set_transform_function(op_Mulh,         gen_Mulh);
	be_set_transform_function(op_Not,          gen_Not);
	be_set_transform_function(op_Or,           gen_Or);
	be_set_transform_function(op_Phi,          gen_Phi);
	be_set_transform_function(op_Proj,         gen_Proj);
	be_set_transform_function(op_Return,       gen_Return);
	be_set_transform_function(op_Sel,          gen_Sel);
	be_set_transform_function(op_Shl,          gen_Shl);
	be_set_transform_function(op_Shr,          gen_Shr);
	be_set_transform_function(op_Shrs,         gen_Shrs);
	be_set_transform_function(op_Start,        gen_Start);
	be_set_transform_function(op_Store,        gen_Store);
	be_set_transform_function(op_Sub,          gen_Sub);
	be_set_transform_function(op_SymConst,     gen_SymConst);
	be_set_transform_function(op_Unknown,      gen_Unknown);

	be_set_transform_function(op_sparc_AddX_t, gen_AddX_t);
	be_set_transform_function(op_sparc_AddCC_t,gen_AddCC_t);
	be_set_transform_function(op_sparc_Save,   be_duplicate_node);
	be_set_transform_function(op_sparc_SubX_t, gen_SubX_t);
	be_set_transform_function(op_sparc_SubCC_t,gen_SubCC_t);
}

/**
 * Transform a Firm graph into a SPARC graph.
 */
void sparc_transform_graph(ir_graph *irg)
{
	ir_entity *entity = get_irg_entity(irg);
	ir_type   *frame_type;

	sparc_register_transformers();

	node_to_stack = pmap_create();

	mode_gp    = mode_Iu;
	mode_fp    = mode_F;
	mode_fp2   = mode_D;
	mode_flags = mode_Bu;
	//mode_fp4 = ?

	start_mem  = NULL;
	start_g0   = NULL;
	start_g7   = NULL;
	start_sp   = NULL;
	start_fp   = NULL;
	frame_base = NULL;

	stackorder = be_collect_stacknodes(irg);
	current_cconv
		= sparc_decide_calling_convention(get_entity_type(entity), irg);
	if (sparc_variadic_fixups(irg, current_cconv)) {
		sparc_free_calling_convention(current_cconv);
		current_cconv
			= sparc_decide_calling_convention(get_entity_type(entity), irg);
	}
	sparc_create_stacklayout(irg, current_cconv);
	be_add_parameter_entity_stores(irg);

	be_transform_graph(irg, NULL);

	be_free_stackorder(stackorder);
	sparc_free_calling_convention(current_cconv);

	frame_type = get_irg_frame_type(irg);
	if (get_type_state(frame_type) == layout_undefined)
		default_layout_compound_type(frame_type);

	pmap_destroy(node_to_stack);
	node_to_stack = NULL;

	be_add_missing_keeps(irg);

	/* do code placement, to optimize the position of constants */
	place_code(irg);
}

void sparc_init_transform(void)
{
	FIRM_DBG_REGISTER(dbg, "firm.be.sparc.transform");
}
