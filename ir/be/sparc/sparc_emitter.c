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
 * @brief   emit assembler for a backend graph
 * @author  Hannes Rapp, Matthias Braun
 */
#include "config.h"

#include <limits.h>

#include "bitfiddle.h"
#include "xmalloc.h"
#include "tv.h"
#include "iredges.h"
#include "debug.h"
#include "irgwalk.h"
#include "irprintf.h"
#include "irop_t.h"
#include "irargs_t.h"
#include "irprog.h"
#include "irargs_t.h"
#include "irtools.h"
#include "error.h"
#include "raw_bitset.h"
#include "dbginfo.h"
#include "heights.h"
#include "lc_opts.h"

#include "besched.h"
#include "beblocksched.h"
#include "beirg.h"
#include "begnuas.h"
#include "be_dbgout.h"
#include "benode.h"
#include "bestack.h"
#include "bepeephole.h"

#include "sparc_emitter.h"
#include "gen_sparc_emitter.h"
#include "sparc_nodes_attr.h"
#include "sparc_new_nodes.h"
#include "gen_sparc_regalloc_if.h"
#include "sparc_architecture.h"

DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

static ir_heights_t  *heights;
static const ir_node *delay_slot_filler; /**< this node has been choosen to fill
                                              the next delay slot */

static void sparc_emit_node(const ir_node *node);
static bool emitting_delay_slot;
static int raw_permis = 0;
static int fill_nops  = 0;
static int permi_info = 0;

void sparc_emit_indent(void)
{
	be_emit_char('\t');
	if (emitting_delay_slot)
		be_emit_char(' ');
}

void sparc_emit_immediate(const ir_node *node)
{
	const sparc_attr_t *attr   = get_sparc_attr_const(node);
	ir_entity          *entity = attr->immediate_value_entity;

	if (entity == NULL) {
		int32_t value = attr->immediate_value;
		assert(sparc_is_value_imm_encodeable(value));
		be_emit_irprintf("%d", value);
	} else {
		if (get_entity_owner(entity) == get_tls_type()) {
			be_emit_cstring("%tle_lox10(");
		} else {
			be_emit_cstring("%lo(");
		}
		be_gas_emit_entity(entity);
		if (attr->immediate_value != 0) {
			be_emit_irprintf("%+d", attr->immediate_value);
		}
		be_emit_char(')');
	}
}

void sparc_emit_high_immediate(const ir_node *node)
{
	const sparc_attr_t *attr   = get_sparc_attr_const(node);
	ir_entity          *entity = attr->immediate_value_entity;

	if (entity == NULL) {
		uint32_t value = (uint32_t) attr->immediate_value;
		be_emit_irprintf("%%hi(0x%X)", value);
	} else {
		if (get_entity_owner(entity) == get_tls_type()) {
			be_emit_cstring("%tle_hix22(");
		} else {
			be_emit_cstring("%hi(");
		}
		be_gas_emit_entity(entity);
		if (attr->immediate_value != 0) {
			be_emit_irprintf("%+d", attr->immediate_value);
		}
		be_emit_char(')');
	}
}

void sparc_emit_source_register(const ir_node *node, int pos)
{
	const arch_register_t *reg = arch_get_irn_register_in(node, pos);
	be_emit_char('%');
	be_emit_string(arch_register_get_name(reg));
}

void sparc_emit_dest_register(const ir_node *node, int pos)
{
	const arch_register_t *reg = arch_get_irn_register_out(node, pos);
	be_emit_char('%');
	be_emit_string(arch_register_get_name(reg));
}

/**
 * Emits either a imm or register depending on arity of node
 * @param node
 * @param register no (-1 if no register)
 */
void sparc_emit_reg_or_imm(const ir_node *node, int pos)
{
	if (arch_get_irn_flags(node) & ((arch_irn_flags_t)sparc_arch_irn_flag_immediate_form)) {
		// we have a imm input
		sparc_emit_immediate(node);
	} else {
		// we have reg input
		sparc_emit_source_register(node, pos);
	}
}

/**
 * emit SP offset
 */
void sparc_emit_offset(const ir_node *node, int offset_node_pos)
{
	const sparc_load_store_attr_t *attr = get_sparc_load_store_attr_const(node);

	if (attr->is_reg_reg) {
		assert(!attr->is_frame_entity);
		assert(attr->base.immediate_value == 0);
		assert(attr->base.immediate_value_entity == NULL);
		be_emit_char('+');
		sparc_emit_source_register(node, offset_node_pos);
	} else if (attr->is_frame_entity) {
		int32_t offset = attr->base.immediate_value;
		if (offset != 0) {
			assert(sparc_is_value_imm_encodeable(offset));
			be_emit_irprintf("%+ld", offset);
		}
	} else if (attr->base.immediate_value != 0
			|| attr->base.immediate_value_entity != NULL) {
		be_emit_char('+');
		sparc_emit_immediate(node);
	}
}

void sparc_emit_source_reg_and_offset(const ir_node *node, int regpos,
                                      int offpos)
{
	const arch_register_t *reg = arch_get_irn_register_in(node, regpos);
	const sparc_load_store_attr_t *attr;

#ifdef DEBUG_libfirm
	if (reg == &sparc_registers[REG_SP]) {
		attr = get_sparc_load_store_attr_const(node);
		if (!attr->is_reg_reg
		    && attr->base.immediate_value < SPARC_SAVE_AREA_SIZE) {

			ir_fprintf(stderr, "warning: emitting stack pointer relative load/store with offset < %d\n", SPARC_SAVE_AREA_SIZE);
		}
	}
#endif

	sparc_emit_source_register(node, regpos);
	sparc_emit_offset(node, offpos);
}

void sparc_emit_float_load_store_mode(const ir_node *node)
{
	const sparc_load_store_attr_t *attr = get_sparc_load_store_attr_const(node);
	ir_mode *mode = attr->load_store_mode;
	int      bits = get_mode_size_bits(mode);

	assert(mode_is_float(mode));

	switch (bits) {
	case 32:  return;
	case 64:  be_emit_char('d'); return;
	case 128: be_emit_char('q'); return;
	}
	panic("invalid float load/store mode %+F", mode);
}

/**
 *  Emit load mode char
 */
void sparc_emit_load_mode(const ir_node *node)
{
	const sparc_load_store_attr_t *attr = get_sparc_load_store_attr_const(node);
	ir_mode *mode      = attr->load_store_mode;
	int      bits      = get_mode_size_bits(mode);
	bool     is_signed = mode_is_signed(mode);

	if (bits == 16) {
		be_emit_string(is_signed ? "sh" : "uh");
	} else if (bits == 8) {
		be_emit_string(is_signed ? "sb" : "ub");
	} else if (bits == 64) {
		be_emit_char('d');
	} else {
		assert(bits == 32);
	}
}

/**
 * Emit store mode char
 */
void sparc_emit_store_mode(const ir_node *node)
{
	const sparc_load_store_attr_t *attr = get_sparc_load_store_attr_const(node);
	ir_mode *mode      = attr->load_store_mode;
	int      bits      = get_mode_size_bits(mode);

	if (bits == 16) {
		be_emit_string("h");
	} else if (bits == 8) {
		be_emit_string("b");
	} else if (bits == 64) {
		be_emit_char('d');
	} else {
		assert(bits == 32);
	}
}

static void emit_fp_suffix(const ir_mode *mode)
{
	unsigned bits = get_mode_size_bits(mode);
	assert(mode_is_float(mode));

	if (bits == 32) {
		be_emit_char('s');
	} else if (bits == 64) {
		be_emit_char('d');
	} else if (bits == 128) {
		be_emit_char('q');
	} else {
		panic("invalid FP mode");
	}
}

void sparc_emit_fp_conv_source(const ir_node *node)
{
	const sparc_fp_conv_attr_t *attr = get_sparc_fp_conv_attr_const(node);
	emit_fp_suffix(attr->src_mode);
}

void sparc_emit_fp_conv_destination(const ir_node *node)
{
	const sparc_fp_conv_attr_t *attr = get_sparc_fp_conv_attr_const(node);
	emit_fp_suffix(attr->dest_mode);
}

/**
 * emits the FP mode suffix char
 */
void sparc_emit_fp_mode_suffix(const ir_node *node)
{
	const sparc_fp_attr_t *attr = get_sparc_fp_attr_const(node);
	emit_fp_suffix(attr->fp_mode);
}

static ir_node *get_jump_target(const ir_node *jump)
{
	return (ir_node*)get_irn_link(jump);
}

/**
 * Returns the target label for a control flow node.
 */
static void sparc_emit_cfop_target(const ir_node *node)
{
	ir_node *block = get_jump_target(node);
	be_gas_emit_block_name(block);
}

/**
 * returns true if a sparc_call calls a register and not an immediate
 */
static bool is_sparc_reg_call(const ir_node *node)
{
	const sparc_attr_t *attr = get_sparc_attr_const(node);
	return attr->immediate_value_entity == NULL;
}

static int get_sparc_Call_dest_addr_pos(const ir_node *node)
{
	assert(is_sparc_reg_call(node));
	return get_irn_arity(node)-1;
}

static bool ba_is_fallthrough(const ir_node *node)
{
	ir_node *block      = get_nodes_block(node);
	ir_node *next_block = (ir_node*)get_irn_link(block);
	return get_irn_link(node) == next_block;
}

static bool is_no_instruction(const ir_node *node)
{
	/* copies are nops if src_reg == dest_reg */
	if (be_is_Copy(node) || be_is_CopyKeep(node)) {
		const arch_register_t *src_reg  = arch_get_irn_register_in(node, 0);
		const arch_register_t *dest_reg = arch_get_irn_register_out(node, 0);

		if (src_reg == dest_reg)
			return true;
	}
	if (be_is_IncSP(node) && be_get_IncSP_offset(node) == 0)
		return true;
	/* Ba is not emitted if it is a simple fallthrough */
	if (is_sparc_Ba(node) && ba_is_fallthrough(node))
		return true;

	return be_is_Keep(node) || be_is_Start(node) || is_Phi(node);
}

static bool has_delay_slot(const ir_node *node)
{
	if (is_sparc_Ba(node)) {
		return !ba_is_fallthrough(node);
	}

	return arch_get_irn_flags(node) & sparc_arch_irn_flag_has_delay_slot;
}

/** returns true if the emitter for this sparc node can produce more than one
 * actual sparc instruction.
 * Usually it is a bad sign if we have to add instructions here. We should
 * rather try to get them lowered down. So we can actually put them into
 * delay slots and make them more accessible to the scheduler.
 */
static bool emits_multiple_instructions(const ir_node *node)
{
	if (has_delay_slot(node))
		return true;

	if (is_sparc_Call(node)) {
		return arch_get_irn_flags(node) & sparc_arch_irn_flag_aggregate_return;
	}

	if (fill_nops && (is_sparc_Permi(node) || is_sparc_Permi23(node)))
		return true;

	return is_sparc_SMulh(node) || is_sparc_UMulh(node)
		|| is_sparc_SDiv(node) || is_sparc_UDiv(node)
		|| be_is_MemPerm(node) || be_is_Perm(node);
}

static bool uses_reg(const ir_node *node, const arch_register_t *reg)
{
	int arity = get_irn_arity(node);
	int i;

	for (i = 0; i < arity; ++i) {
		const arch_register_t *in_reg = arch_get_irn_register_in(node, i);
		if (reg == in_reg)
			return true;
	}
	return false;
}

static bool writes_reg(const ir_node *node, const arch_register_t *reg)
{
	unsigned n_outs = arch_get_irn_n_outs(node);
	unsigned o;
	for (o = 0; o < n_outs; ++o) {
		const arch_register_t *out_reg = arch_get_irn_register_out(node, o);
		if (out_reg == reg)
			return true;
	}
	return false;
}

static bool can_move_into_delayslot(const ir_node *node, const ir_node *to)
{
	if (!be_can_move_before(heights, node, to))
		return false;

	if (is_sparc_Call(to)) {
		ir_node *check;
		/** all deps are used after the delay slot so, we're fine */
		if (!is_sparc_reg_call(to))
			return true;

		check = get_irn_n(to, get_sparc_Call_dest_addr_pos(to));
		if (skip_Proj(check) == node)
			return false;

		/* the Call also destroys the value of %o7, but since this is
		 * currently marked as ignore register in the backend, it
		 * should never be used by the instruction in the delay slot. */
		if (uses_reg(node, &sparc_registers[REG_O7]))
			return false;
		return true;
	} else if (is_sparc_Return(to)) {
		/* return uses the value of %o7, all other values are not
		 * immediately used */
		if (writes_reg(node, &sparc_registers[REG_O7]))
			return false;
		return true;
	} else {
		/* the node must not use our computed values */
		int arity = get_irn_arity(to);
		int i;
		for (i = 0; i < arity; ++i) {
			ir_node *in = get_irn_n(to, i);
			if (skip_Proj(in) == node)
				return false;
		}
		return true;
	}
}

/**
 * search for an instruction that can fill the delay slot of @p node
 */
static const ir_node *pick_delay_slot_for(const ir_node *node)
{
	const ir_node *schedpoint = node;
	unsigned       tries      = 0;
	/* currently we don't track which registers are still alive, so we can't
	 * pick any other instructions other than the one directly preceding */
	static const unsigned PICK_DELAY_SLOT_MAX_DISTANCE = 10;

	assert(has_delay_slot(node));

	while (sched_has_prev(schedpoint)) {
		schedpoint = sched_prev(schedpoint);

		if (has_delay_slot(schedpoint))
			break;

		/* skip things which don't really result in instructions */
		if (is_no_instruction(schedpoint))
			continue;

		if (tries++ >= PICK_DELAY_SLOT_MAX_DISTANCE)
			break;

		if (emits_multiple_instructions(schedpoint))
			continue;

		if (!can_move_into_delayslot(schedpoint, node))
			continue;

		/* found something */
		return schedpoint;
	}

	return NULL;
}

/**
 * Emits code for stack space management
 */
static void emit_be_IncSP(const ir_node *irn)
{
	int offset = be_get_IncSP_offset(irn);

	if (offset == 0)
		return;

	/* SPARC stack grows downwards */
	sparc_emit_indent();
	if (offset < 0) {
		be_emit_cstring("sub ");
		offset = -offset;
	} else {
		be_emit_cstring("add ");
	}

	sparc_emit_source_register(irn, 0);
	be_emit_irprintf(", %d", -offset);
	be_emit_cstring(", ");
	sparc_emit_dest_register(irn, 0);
	be_emit_finish_line_gas(irn);
}

/**
 * emits code for mulh
 */
static void emit_sparc_Mulh(const ir_node *irn)
{
	sparc_emit_indent();
	if (is_sparc_UMulh(irn)) {
		be_emit_char('u');
	} else {
		assert(is_sparc_SMulh(irn));
		be_emit_char('s');
	}
	be_emit_cstring("mul ");

	sparc_emit_source_register(irn, 0);
	be_emit_cstring(", ");
	sparc_emit_reg_or_imm(irn, 1);
	be_emit_cstring(", ");
	sparc_emit_dest_register(irn, 0);
	be_emit_finish_line_gas(irn);

	// our result is in the y register now
	// we just copy it to the assigned target reg
	sparc_emit_indent();
	be_emit_cstring("mov %y, ");
	sparc_emit_dest_register(irn, 0);
	be_emit_finish_line_gas(irn);
}

static void fill_delay_slot(void)
{
	emitting_delay_slot = true;
	if (delay_slot_filler != NULL) {
		sparc_emit_node(delay_slot_filler);
		delay_slot_filler = NULL;
	} else {
		sparc_emit_indent();
		be_emit_cstring("nop\n");
		be_emit_write_line();
	}
	emitting_delay_slot = false;
}

static void emit_sparc_Div(const ir_node *node, bool is_signed)
{
	/* can we get the delay count of the wr instruction somewhere? */
	unsigned wry_delay_count = 3;
	unsigned i;

	sparc_emit_indent();
	be_emit_cstring("wr ");
	sparc_emit_source_register(node, 0);
	be_emit_cstring(", 0, %y");
	be_emit_finish_line_gas(node);

	for (i = 0; i < wry_delay_count; ++i) {
		fill_delay_slot();
	}

	sparc_emit_indent();
	be_emit_irprintf("%s ", is_signed ? "sdiv" : "udiv");
	sparc_emit_source_register(node, 1);
	be_emit_cstring(", ");
	sparc_emit_reg_or_imm(node, 2);
	be_emit_cstring(", ");
	sparc_emit_dest_register(node, 0);
	be_emit_finish_line_gas(node);
}

static void emit_sparc_SDiv(const ir_node *node)
{
	emit_sparc_Div(node, true);
}

static void emit_sparc_UDiv(const ir_node *node)
{
	emit_sparc_Div(node, false);
}

static void emit_sparc_Call(const ir_node *node)
{
	sparc_emit_indent();
	be_emit_cstring("call ");
	if (is_sparc_reg_call(node)) {
		int dest_addr = get_sparc_Call_dest_addr_pos(node);
		sparc_emit_source_register(node, dest_addr);
	} else {
		const sparc_attr_t *attr   = get_sparc_attr_const(node);
		ir_entity          *entity = attr->immediate_value_entity;
	    be_gas_emit_entity(entity);
	    if (attr->immediate_value != 0) {
			be_emit_irprintf("%+d", attr->immediate_value);
		}
		be_emit_cstring(", 0");
	}
	be_emit_finish_line_gas(node);

	fill_delay_slot();

	if (arch_get_irn_flags(node) & sparc_arch_irn_flag_aggregate_return) {
		sparc_emit_indent();
		be_emit_cstring("unimp 8\n");
		be_emit_write_line();
	}
}

static void verify_permi(unsigned *regs)
{
	int i, j;

	/* Generic checks */
	for (i = 0; i < 5; ++i) {
		assert((regs[i] >= 1 && regs[i] <= 31) && "Invalid regno in permi");
	}

	if (regs[0] <= regs[1]) {
		/* Permi5 */
		j = 4;
		for (i = 0; i < 4; ++i) {
			if (regs[i] == regs[i + 1]) {
				j = i;
				break;
			}
		}

		for (i = j; i < 4; ++i) {
			assert(regs[i] == regs[i + 1] && "Equal regnos not at end");
		}
	} else {
		/* Permi23 */
		assert(regs[0] != regs[1] && "No first cycle in Permi23");

		for (i = 2; i < 5; ++i) {
			assert(regs[0] != regs[i] && "Reg in first and second cycle");
			assert(regs[1] != regs[i] && "Reg in first and second cycle");
		}

		assert(regs[2] != regs[3] && "Equal regnos not at end");
	}
}

static unsigned permi5(unsigned *regs)
{
	verify_permi(regs);

	return (regs[0] << 20) | (regs[1] << 15) | (regs[2] << 10) | (regs[3] << 5) | regs[4];
}

static void icore_emit_fill_nops(const ir_node *irn)
{
	if (fill_nops) {
		int num_sparc_insns = 0;
		int saved_insns;
		int i;

		if (is_sparc_Permi(irn)) {
			const int  length   = get_irn_arity(irn);
			const bool is_cycle = get_sparc_permi_attr_const(irn)->is_cycle;

			if (is_cycle)
				num_sparc_insns = (length - 1) * 3;
			else
				num_sparc_insns = length - 1;
		} else if (is_sparc_Permi23(irn)) {
			const int  arity     = get_irn_arity(irn);
			const bool is_cycle2 = get_sparc_permi23_attr_const(irn)->is_cycle2;
			const bool is_cycle3 = get_sparc_permi23_attr_const(irn)->is_cycle3;
			const int  pos3      = is_cycle2 ? 2 : 1;
			const int  size3     = arity - pos3 + (is_cycle3 ? 0 : 1);

			if (is_cycle2)
				num_sparc_insns = 3;
			else
				num_sparc_insns = 1;

			if (is_cycle3)
				num_sparc_insns += (size3 - 1) * 3;
			else
				num_sparc_insns += size3 - 1;
		} else
			assert(!"Invalid node passed to icore_emit_fill_nops");

		saved_insns = num_sparc_insns - 1;
		for (i = 0; i < saved_insns; ++i)
			be_emit_cstring("\tnop\n");
	}
}

static void emit_permi(const ir_node *irn, unsigned *regs)
{
	sparc_emit_indent();

	if (!raw_permis) {
		be_emit_irprintf("permi %u", permi5(regs));
		be_emit_finish_line_gas(irn);
	} else {
		unsigned reg0 = regs[0];
		unsigned opc  = 0;
		unsigned rd   = 0x10 | (reg0 >> 2);
		unsigned xop  = 0;
		unsigned imm  = ((reg0 & 3) << 20) | (regs[1] << 15) | (regs[2] << 10) | (regs[3] << 5) | regs[4];
		unsigned insn = (opc << 30) | (rd << 25) | (xop << 22) | imm;

		be_emit_irprintf(".long %u", insn);
		be_emit_finish_line_gas(irn);
	}

	icore_emit_fill_nops(irn);
}

static void emit_icore_Permi_chain(const ir_node *irn);

static void emit_icore_Permi(const ir_node *irn)
{
	const int MAX_CYCLE_SIZE = 5;
	const arch_register_t *in_regs[MAX_CYCLE_SIZE];
	const arch_register_t *out_regs[MAX_CYCLE_SIZE];
	unsigned regns[MAX_CYCLE_SIZE];
	int i;
	const int arity = get_irn_arity(irn);
	const sparc_permi_attr_t *attr = get_sparc_permi_attr_const(irn);

	if (!attr->is_cycle) {
		emit_icore_Permi_chain(irn);
		return;
	}

	assert(arity >= 2 && arity <= MAX_CYCLE_SIZE);
	for (i = 0; i < arity; ++i) {
		in_regs[i]  = arch_get_irn_register_in(irn, i);
		out_regs[i] = arch_get_irn_register_out(irn, i);
	}

	/*
	 * Sanity check:
	 *
	 * A Permi node with input registers
	 *   r0, r1, r2, r3, r4
	 * encodes the cycle
	 *   r0->r1->r2->r3->r4->r0
	 * which can be seen as a 'right rotation'.
	 *
	 *   r0   r1   r2   r3   r4
	 *   |    |    |    |    |
	 * ,-----------------------.
	 * |         Permi         |
	 * `-----------------------'
	 *   |    |    |    |    |
	 *   r1   r2   r3   r4   r0
	 *
	 * Therefore, the output register of successor node i must be
	 * equal to the input register of predecessor node i + 1.
	 */
#ifndef NDEBUG
	for (i = 0; i < arity; ++i) {
		assert(out_regs[i] == in_regs[(i + 1) % arity] && "Permi node does not represent a cycle of the required form");
	}
#endif

	/*
	 * The current hardware implementation of the permi instruction
	 * describes a 'left rotation', i.e.
	 *   permi r0, r1, r2, r3, r4
	 * encodes
	 *   r0->r4->r3->r2->r1->r0.
	 * Therefore, we reverse the order of the register numbers for
	 * actual code generation.
	 */
	for (i = 0; i < arity; ++i) {
		regns[i] = (unsigned) in_regs[arity - i - 1]->index;
	}

	/*
	 * We have to take care that we generate the correct order for registers
	 * here.
	 *   permi r0, r4, r3, r2, r1   encodes   r0->r1->r2->r3->r4->r0
	 * but
	 *   permi r4, r3, r2, r1, r0   encodes   r0->r1->r0  r2->r3->r4->r2
	 */
	while (regns[0] > regns[1]) {
		/* Rotate until regns[0] <= regns[1] holds. */
		unsigned regn0 = regns[0];
		for (i = 0; i < arity - 1; ++i)
			regns[i] = regns[i + 1];
		regns[arity - 1] = regn0;
	}

	/*
	 * The permi instruction always operates on five registers.  However,
	 * it is allowed to supply the same register number multiple times.
	 * Therefore, e.g., a swap operation on r0 and r1 can be expressed with:
	 *   permi r0, r1, r1, r1, r1
	 *
	 * Hence, we fill the unused elements in regns (if there are any) with
	 * the last register number we inserted.
	 */
	for (i = arity; i < MAX_CYCLE_SIZE; ++i) {
		regns[i] = regns[arity - 1];
	}

	if (permi_info) {
		/* Print some information about the meaning of the following permi. */
		be_emit_irprintf("\t/* Cycle: ");
		for (i = 0; i < arity; ++i) {
			be_emit_irprintf("%s->", in_regs[i]->name);
		}
		be_emit_irprintf("%s */", in_regs[0]->name);
		be_emit_finish_line_gas(NULL);
	}

	emit_permi(irn, regns);
}

static void emit_icore_Permi23(const ir_node *irn)
{
	const int  arity     = get_irn_arity(irn);
	const bool is_cycle2 = get_sparc_permi23_attr_const(irn)->is_cycle2;
	const bool is_cycle3 = get_sparc_permi23_attr_const(irn)->is_cycle3;
	const int  pos3      = is_cycle2 ? 2 : 1;
	const int  size3     = arity - pos3 + (is_cycle3 ? 0 : 1);
	unsigned   regns[5];
	const arch_register_t *in_regs[5];
	const arch_register_t *out_regs[5];
	int i;

	assert(size3 >= 2 && size3 <= 3
	       && "Second op in Permi23 has invalid size");

	for (i = 0; i < arity; ++i) {
		in_regs[i]  = arch_get_irn_register_in(irn, i);
		out_regs[i] = arch_get_irn_register_out(irn, i);
	}

#ifndef NDEBUG
	/* Sanity checks */

	if (is_cycle2) {
		for (i = 0; i < 2; ++i) {
			assert(out_regs[i] == in_regs[(i + 1) % 2]
			       && "First cycle in Permi23 is malformed");
		}
	} else {
		assert(out_regs[0] != in_regs[0]
		       && "First chain in Permi23 is malformed");
	}

	if (is_cycle3) {
		for (i = 0; i < size3; ++i) {
			assert(out_regs[pos3 + i] == in_regs[pos3 + (i + 1) % size3]
			       && "Second cycle in Permi23 is malformed");
		}
	} else {
		if (size3 == 2) {
			assert(out_regs[pos3] != in_regs[pos3]
			       && "Second chain in Permi23 is malformed");
		} else if (size3 == 3) {
			assert(out_regs[pos3] == in_regs[pos3 + 1]
			       && "Second chain in Permi23 is malformed");
		}
	}
#endif

	/*
	 * The current hardware implementation of the permi instruction
	 * describes a 'left rotation', i.e.
	 *   permi23 r0, r1, r2, r3, r4
	 * encodes
	 *   r1->r0->r1   and   r2->r4->r3->r2
	 */
	if (is_cycle2) {
		regns[0] = (unsigned) in_regs[1]->index;
		regns[1] = (unsigned) in_regs[0]->index;
	} else {
		regns[0] = (unsigned) out_regs[0]->index;
		regns[1] = (unsigned) in_regs[0]->index;
	}

	if (is_cycle3) {
		if (size3 == 2) {
			regns[2] = (unsigned) in_regs[pos3 + 1]->index;
			regns[3] = (unsigned) in_regs[pos3]->index;
			regns[4] = regns[3];
		} else {
			regns[2] = (unsigned) in_regs[pos3 + 2]->index;
			regns[3] = (unsigned) in_regs[pos3 + 1]->index;
			regns[4] = (unsigned) in_regs[pos3]->index;
		}
	} else {
		if (size3 == 2) {
			regns[2] = (unsigned) out_regs[pos3]->index;
			regns[3] = (unsigned) in_regs[pos3]->index;
			regns[4] = regns[3];
		} else {
			regns[2] = (unsigned) out_regs[pos3 + 1]->index;
			regns[3] = (unsigned) out_regs[pos3]->index;
			regns[4] = (unsigned) in_regs[pos3]->index;
		}
	}

	/*
	 * We have to take care that we generate the correct order for registers
	 * here.
	 *   permi r0, r4, r3, r2, r1   encodes   r0->r1->r2->r3->r4->r0
	 * but
	 *   permi r4, r3, r2, r1, r0   encodes   r0->r1->r0  r2->r3->r4->r2
	 */
	if (regns[0] <= regns[1]) {
		/* Swap regns[0] and regns[1]. */
		unsigned regn0 = regns[0];
		regns[0] = regns[1];
		regns[1] = regn0;
	}

	if (permi_info) {
		/* Print some information about the meaning of the following permi. */
		if (is_cycle2) {
			be_emit_irprintf("\t/* First cycle: %s->%s->%s */",
				in_regs[0]->name, in_regs[1]->name, in_regs[0]->name);
		} else {
			be_emit_irprintf("\t/* First chain: %s->%s */",
				in_regs[0]->name, out_regs[0]->name);
		}
		be_emit_finish_line_gas(NULL);
		if (is_cycle3) {
			be_emit_irprintf("\t/* Second cycle: ");
			for (i = 0; i < size3; ++i) {
				be_emit_irprintf("%s->", in_regs[pos3 + i]->name);
			}
			be_emit_irprintf("%s */", in_regs[pos3]->name);
		} else {
			be_emit_irprintf("\t/* Second chain: ");
			be_emit_irprintf("%s->%s",
				in_regs[pos3]->name, out_regs[pos3]->name);

			if (size3 == 3) {
				be_emit_irprintf("->%s */", out_regs[pos3 + 1]->name);
			} else {
				be_emit_irprintf(" */");
			}
		}
		be_emit_finish_line_gas(NULL);
	}

	emit_permi(irn, regns);
}

static void emit_icore_Permi_chain(const ir_node *irn)
{
	const int MAX_CYCLE_SIZE = 5;
	const arch_register_t *in_regs[MAX_CYCLE_SIZE - 1];
	const arch_register_t *out_regs[MAX_CYCLE_SIZE - 1];
	unsigned regns[MAX_CYCLE_SIZE];
	int i;
	const int arity = get_irn_arity(irn);
	const int chain_size = arity + 1;

	assert(chain_size >= 2 && chain_size <= MAX_CYCLE_SIZE);

	for (i = 0; i < arity; ++i) {
		in_regs[i]  = arch_get_irn_register_in(irn, i);
		out_regs[i] = arch_get_irn_register_out(irn, i);
	}

	/*
	 * Sanity check:
	 *
	 * A PseudoCycle node with input registers
	 *   r0, r1, r2, r3
	 * must have the following form:
	 *
	 *   r0   r1   r2   r3
	 *   |    |    |    |
	 * ,------------------.
	 * |    PseudoCycle   |
	 * `------------------'
	 *   |    |    |    |
	 *   r1   r2   r3   rX
	 *
	 * Therefore, the output register of successor node i must be
	 * equal to the input register of predecessor node i + 1.
	 * The first input register and the last output register must
	 * not be identical (otherwise it would be a real cycle).
	 */
#ifndef NDEBUG
	for (i = 0; i < arity - 1; ++i) {
		assert(out_regs[i] == in_regs[i + 1] && "PseudoCycle node does not have required form");
	}
	assert(in_regs[0] != out_regs[arity - 1] && "PseudoCycle is actually a real cycle");
#endif

	/*
	 * The current hardware implementation of the permi instruction
	 * describes a 'left rotation', i.e.
	 *   permi r0, r1, r2, r3, r4
	 * encodes
	 *   r0->r4->r3->r2->r1->r0.
	 * Therefore, we reverse the order of the register numbers for
	 * actual code generation.
	 */
	regns[0] = out_regs[arity - 1]->index;
	for (i = 1; i < chain_size; ++i) {
		regns[i] = (unsigned) in_regs[arity - i]->index;
	}

	/*
	 * We have to take care that we generate the correct order for registers
	 * here.
	 *   permi r0, r4, r3, r2, r1   encodes   r0->r1->r2->r3->r4->r0
	 * but
	 *   permi r4, r3, r2, r1, r0   encodes   r0->r1->r0  r2->r3->r4->r2
	 */
	while (regns[0] > regns[1]) {
		/* Rotate until regns[0] <= regns[1] holds. */
		unsigned regn0 = regns[0];
		for (i = 0; i < chain_size - 1; ++i)
			regns[i] = regns[i + 1];
		regns[chain_size - 1] = regn0;
	}

	/*
	 * The permi instruction always operates on five registers.  However,
	 * it is allowed to supply the same register number multiple times.
	 * Therefore, e.g., a swap operation on r0 and r1 can be expressed with:
	 *   permi r0, r1, r1, r1, r1
	 *
	 * Hence, we fill the unused elements in regns (if there are any) with
	 * the last register number we inserted.
	 */
	for (i = chain_size; i < MAX_CYCLE_SIZE; ++i) {
		regns[i] = regns[chain_size - 1];
	}

	if (permi_info) {
		/* Print some information about the meaning of the following permi. */
		be_emit_irprintf("\t/* PseudoCycle with chain: ");
		be_emit_irprintf("%s", in_regs[0]->name);
		for (i = 0; i < arity; ++i) {
			be_emit_irprintf("->%s", out_regs[i]->name);
		}
		be_emit_irprintf("*/");
		be_emit_finish_line_gas(NULL);
	}

	emit_permi(irn, regns);
}

static void emit_be_Perm_xor(const ir_node *irn)
{
	assert(get_irn_arity(irn) == 2 && "Perm nodes must have arity of 2");

	sparc_emit_indent();
	be_emit_cstring("xor ");
	sparc_emit_source_register(irn, 1);
	be_emit_cstring(", ");
	sparc_emit_source_register(irn, 0);
	be_emit_cstring(", ");
	sparc_emit_source_register(irn, 0);
	be_emit_finish_line_gas(NULL);

	sparc_emit_indent();
	be_emit_cstring("xor ");
	sparc_emit_source_register(irn, 1);
	be_emit_cstring(", ");
	sparc_emit_source_register(irn, 0);
	be_emit_cstring(", ");
	sparc_emit_source_register(irn, 1);
	be_emit_finish_line_gas(NULL);

	sparc_emit_indent();
	be_emit_cstring("xor ");
	sparc_emit_source_register(irn, 1);
	be_emit_cstring(", ");
	sparc_emit_source_register(irn, 0);
	be_emit_cstring(", ");
	sparc_emit_source_register(irn, 0);
	be_emit_finish_line_gas(irn);
}

/**
 * Emit code for Perm node
 */
static void emit_be_Perm(const ir_node *irn)
{
	assert(!sparc_cg_config.use_permi && "Regular Perm nodes should not show up if permi is available");
	emit_be_Perm_xor(irn);
}

/* The stack pointer must always be SPARC_STACK_ALIGNMENT bytes aligned, so get
 * the next bigger integer that's evenly divisible by it. */
static unsigned get_aligned_sp_change(const unsigned num_regs)
{
	const unsigned bytes = num_regs * SPARC_REGISTER_SIZE;
	return round_up2(bytes, SPARC_STACK_ALIGNMENT);
}

/* Spill register l0 or both l0 and l1, depending on n_spilled and n_to_spill.*/
static void memperm_emit_spill_registers(const ir_node *node, int n_spilled,
                                         int n_to_spill)
{
	assert(n_spilled < n_to_spill);

	if (n_spilled == 0) {
		/* We always reserve stack space for two registers because during copy
		 * processing we don't know yet if we also need to handle a cycle which
		 * needs two registers.  More complicated code in emit_MemPerm would
		 * prevent wasting SPARC_REGISTER_SIZE bytes of stack space but
		 * it is not worth the worse readability of emit_MemPerm. */

		/* Keep stack pointer aligned. */
		unsigned sp_change = get_aligned_sp_change(2);
		sparc_emit_indent();
		be_emit_irprintf("sub %%sp, %u, %%sp", sp_change);
		be_emit_finish_line_gas(node);

		/* Spill register l0. */
		sparc_emit_indent();
		be_emit_irprintf("st %%l0, [%%sp%+d]", SPARC_MIN_STACKSIZE);
		be_emit_finish_line_gas(node);
	}

	if (n_to_spill == 2) {
		/* Spill register l1. */
		sparc_emit_indent();
		be_emit_irprintf("st %%l1, [%%sp%+d]", SPARC_MIN_STACKSIZE + SPARC_REGISTER_SIZE);
		be_emit_finish_line_gas(node);
	}
}

/* Restore register l0 or both l0 and l1, depending on n_spilled. */
static void memperm_emit_restore_registers(const ir_node *node, int n_spilled)
{
	unsigned sp_change;

	if (n_spilled == 2) {
		/* Restore register l1. */
		sparc_emit_indent();
		be_emit_irprintf("ld [%%sp%+d], %%l1", SPARC_MIN_STACKSIZE + SPARC_REGISTER_SIZE);
		be_emit_finish_line_gas(node);
	}

	/* Restore register l0. */
	sparc_emit_indent();
	be_emit_irprintf("ld [%%sp%+d], %%l0", SPARC_MIN_STACKSIZE);
	be_emit_finish_line_gas(node);

	/* Restore stack pointer. */
	sp_change = get_aligned_sp_change(2);
	sparc_emit_indent();
	be_emit_irprintf("add %%sp, %u, %%sp", sp_change);
	be_emit_finish_line_gas(node);
}

/* Emit code to copy in_ent to out_ent.  Only uses l0. */
static void memperm_emit_copy(const ir_node *node, ir_entity *in_ent,
                              ir_entity *out_ent)
{
	ir_graph          *irg     = get_irn_irg(node);
	be_stack_layout_t *layout  = be_get_irg_stack_layout(irg);
	int                off_in  = be_get_stack_entity_offset(layout, in_ent, 0);
	int                off_out = be_get_stack_entity_offset(layout, out_ent, 0);

	/* Load from input entity. */
	sparc_emit_indent();
	be_emit_irprintf("ld [%%fp%+d], %%l0", off_in);
	be_emit_finish_line_gas(node);

	/* Store to output entity. */
	sparc_emit_indent();
	be_emit_irprintf("st %%l0, [%%fp%+d]", off_out);
	be_emit_finish_line_gas(node);
}

/* Emit code to swap ent1 and ent2.  Uses l0 and l1. */
static void memperm_emit_swap(const ir_node *node, ir_entity *ent1,
                              ir_entity *ent2)
{
	ir_graph          *irg     = get_irn_irg(node);
	be_stack_layout_t *layout  = be_get_irg_stack_layout(irg);
	int                off1    = be_get_stack_entity_offset(layout, ent1, 0);
	int                off2    = be_get_stack_entity_offset(layout, ent2, 0);

	/* Load from first input entity. */
	sparc_emit_indent();
	be_emit_irprintf("ld [%%fp%+d], %%l0", off1);
	be_emit_finish_line_gas(node);

	/* Load from second input entity. */
	sparc_emit_indent();
	be_emit_irprintf("ld [%%fp%+d], %%l1", off2);
	be_emit_finish_line_gas(node);

	/* Store first value to second output entity. */
	sparc_emit_indent();
	be_emit_irprintf("st %%l0, [%%fp%+d]", off2);
	be_emit_finish_line_gas(node);

	/* Store second value to first output entity. */
	sparc_emit_indent();
	be_emit_irprintf("st %%l1, [%%fp%+d]", off1);
	be_emit_finish_line_gas(node);
}

/* Find the index of ent in ents or return -1 if not found. */
static int get_index(ir_entity **ents, int n, ir_entity *ent)
{
	int i;

	for (i = 0; i < n; ++i)
		if (ents[i] == ent)
			return i;

	return -1;
}

/*
 * Emit code for a MemPerm node.
 *
 * Analyze MemPerm for copy chains and cyclic swaps and resolve them using
 * loads and stores.
 * This function is conceptually very similar to permute_values in
 * beprefalloc.c.
 */
static void emit_be_MemPerm(const ir_node *node)
{
	int         memperm_arity = be_get_MemPerm_entity_arity(node);
	/* Upper limit for the number of participating entities is twice the
	 * arity, e.g., for a simple copying MemPerm node with one input/output. */
	int         max_size      = 2 * memperm_arity;
	ir_entity **entities      = ALLOCANZ(ir_entity *, max_size);
	/* sourceof contains the input entity for each entity.  If an entity is
	 * never used as an output, its entry in sourceof is a fix point. */
	int        *sourceof      = ALLOCANZ(int,         max_size);
	/* n_users counts how many output entities use this entity as their input.*/
	int        *n_users       = ALLOCANZ(int,         max_size);
	/* n_spilled records the number of spilled registers, either 1 or 2. */
	int         n_spilled     = 0;
	int         i, n, oidx;

	/* This implementation currently only works with frame pointers. */
	ir_graph          *irg    = get_irn_irg(node);
	be_stack_layout_t *layout = be_get_irg_stack_layout(irg);
	assert(!layout->sp_relative && "MemPerms currently do not work without frame pointers");

	for (i = 0; i < max_size; ++i) {
		sourceof[i] = i;
	}

	for (i = n = 0; i < memperm_arity; ++i) {
		ir_entity *out  = be_get_MemPerm_out_entity(node, i);
		ir_entity *in   = be_get_MemPerm_in_entity(node, i);
		int              oidx; /* Out index */
		int              iidx; /* In index */

		/* Insert into entities to be able to operate on unique indices. */
		if (get_index(entities, n, out) == -1)
			entities[n++] = out;
		if (get_index(entities, n, in) == -1)
			entities[n++] = in;

		oidx = get_index(entities, n, out);
		iidx = get_index(entities, n, in);

		sourceof[oidx] = iidx; /* Remember the source. */
		++n_users[iidx]; /* Increment number of users of this entity. */
	}

	/* First do all the copies. */
	for (oidx = 0; oidx < n; /* empty */) {
		int iidx = sourceof[oidx];

		/* Nothing to do for fix points.
		 * Also, if entities[oidx] is used as an input by another copy, we
		 * can't overwrite entities[oidx] yet.*/
		if (iidx == oidx || n_users[oidx] > 0) {
			++oidx;
			continue;
		}

		/* We found the end of a 'chain', so do the copy. */
		if (n_spilled == 0) {
			memperm_emit_spill_registers(node, n_spilled, /*n_to_spill=*/1);
			n_spilled = 1;
		}
		memperm_emit_copy(node, entities[iidx], entities[oidx]);

		/* Mark as done. */
		sourceof[oidx] = oidx;

		assert(n_users[iidx] > 0);
		/* Decrementing the number of users might enable us to do another
		 * copy. */
		--n_users[iidx];

		if (iidx < oidx && n_users[iidx] == 0) {
			oidx = iidx;
		} else {
			++oidx;
		}
	}

	/* The rest are cycles. */
	for (oidx = 0; oidx < n; /* empty */) {
		int iidx = sourceof[oidx];
		int tidx;

		/* Nothing to do for fix points. */
		if (iidx == oidx) {
			++oidx;
			continue;
		}

		assert(n_users[iidx] == 1);

		/* Swap the two values to resolve the cycle. */
		if (n_spilled < 2) {
			memperm_emit_spill_registers(node, n_spilled, /*n_to_spill=*/2);
			n_spilled = 2;
		}
		memperm_emit_swap(node, entities[iidx], entities[oidx]);

		tidx = sourceof[iidx];
		/* Mark as done. */
		sourceof[iidx] = iidx;

		/* The source of oidx is now the old source of iidx, because we swapped
		 * the two entities. */
		sourceof[oidx] = tidx;
	}

#ifdef DEBUG_libfirm
	/* Only fix points should remain. */
	for (i = 0; i < max_size; ++i) {
		assert(sourceof[i] == i);
	}
#endif

	assert(n_spilled > 0 && "Useless MemPerm node");

	memperm_emit_restore_registers(node, n_spilled);
}

static void emit_sparc_Return(const ir_node *node)
{
	ir_graph  *irg    = get_irn_irg(node);
	ir_entity *entity = get_irg_entity(irg);
	ir_type   *type   = get_entity_type(entity);

	const char *destreg = "%o7";

	/* hack: we don't explicitely model register changes because of the
	 * restore node. So we have to do it manually here */
	if (delay_slot_filler != NULL &&
			(is_sparc_Restore(delay_slot_filler)
			 || is_sparc_RestoreZero(delay_slot_filler))) {
		destreg = "%i7";
	}
	sparc_emit_indent();
	be_emit_cstring("jmp ");
	be_emit_string(destreg);
	if (get_method_calling_convention(type) & cc_compound_ret) {
		be_emit_cstring("+12");
	} else {
		be_emit_cstring("+8");
	}
	be_emit_finish_line_gas(node);
	fill_delay_slot();
}

static const arch_register_t *map_i_to_o_reg(const arch_register_t *reg)
{
	unsigned idx = reg->global_index;
	if (idx < REG_I0 || idx > REG_I7)
		return reg;
	idx += REG_O0 - REG_I0;
	assert(REG_O0 <= idx && idx <= REG_O7);
	return &sparc_registers[idx];
}

static void emit_sparc_Restore(const ir_node *node)
{
	const arch_register_t *destreg
		= arch_get_irn_register_out(node, pn_sparc_Restore_res);
	sparc_emit_indent();
	be_emit_cstring("restore ");
	sparc_emit_source_register(node, 1);
	be_emit_cstring(", ");
	sparc_emit_reg_or_imm(node, 2);
	be_emit_cstring(", ");
	destreg = map_i_to_o_reg(destreg);
	be_emit_char('%');
	be_emit_string(arch_register_get_name(destreg));
	be_emit_finish_line_gas(node);
}

static void emit_sparc_FrameAddr(const ir_node *node)
{
	const sparc_attr_t *attr   = get_sparc_attr_const(node);
	int32_t             offset = attr->immediate_value;

	sparc_emit_indent();
	if (offset < 0) {
		be_emit_cstring("add ");
		sparc_emit_source_register(node, 0);
		be_emit_cstring(", ");
		assert(sparc_is_value_imm_encodeable(offset));
		be_emit_irprintf("%ld", offset);
	} else {
		be_emit_cstring("sub ");
		sparc_emit_source_register(node, 0);
		be_emit_cstring(", ");
		assert(sparc_is_value_imm_encodeable(-offset));
		be_emit_irprintf("%ld", -offset);
	}

	be_emit_cstring(", ");
	sparc_emit_dest_register(node, 0);
	be_emit_finish_line_gas(node);
}

static const char *get_icc_unsigned(ir_relation relation)
{
	switch (relation & (ir_relation_less_equal_greater)) {
	case ir_relation_false:              return "bn";
	case ir_relation_equal:              return "be";
	case ir_relation_less:               return "blu";
	case ir_relation_less_equal:         return "bleu";
	case ir_relation_greater:            return "bgu";
	case ir_relation_greater_equal:      return "bgeu";
	case ir_relation_less_greater:       return "bne";
	case ir_relation_less_equal_greater: return "ba";
	default: panic("Cmp has unsupported relation");
	}
}

static const char *get_icc_signed(ir_relation relation)
{
	switch (relation & (ir_relation_less_equal_greater)) {
	case ir_relation_false:              return "bn";
	case ir_relation_equal:              return "be";
	case ir_relation_less:               return "bl";
	case ir_relation_less_equal:         return "ble";
	case ir_relation_greater:            return "bg";
	case ir_relation_greater_equal:      return "bge";
	case ir_relation_less_greater:       return "bne";
	case ir_relation_less_equal_greater: return "ba";
	default: panic("Cmp has unsupported relation");
	}
}

static const char *get_fcc(ir_relation relation)
{
	switch (relation) {
	case ir_relation_false:                   return "fbn";
	case ir_relation_equal:                   return "fbe";
	case ir_relation_less:                    return "fbl";
	case ir_relation_less_equal:              return "fble";
	case ir_relation_greater:                 return "fbg";
	case ir_relation_greater_equal:           return "fbge";
	case ir_relation_less_greater:            return "fblg";
	case ir_relation_less_equal_greater:      return "fbo";
	case ir_relation_unordered:               return "fbu";
	case ir_relation_unordered_equal:         return "fbue";
	case ir_relation_unordered_less:          return "fbul";
	case ir_relation_unordered_less_equal:    return "fbule";
	case ir_relation_unordered_greater:       return "fbug";
	case ir_relation_unordered_greater_equal: return "fbuge";
	case ir_relation_unordered_less_greater:  return "fbne";
	case ir_relation_true:                    return "fba";
	}
	panic("invalid relation");
}

typedef const char* (*get_cc_func)(ir_relation relation);

static void emit_sparc_branch(const ir_node *node, get_cc_func get_cc)
{
	const sparc_jmp_cond_attr_t *attr = get_sparc_jmp_cond_attr_const(node);
	ir_relation      relation    = attr->relation;
	const ir_node   *proj_true   = NULL;
	const ir_node   *proj_false  = NULL;
	const ir_edge_t *edge;
	const ir_node   *block;
	const ir_node   *next_block;

	foreach_out_edge(node, edge) {
		ir_node *proj = get_edge_src_irn(edge);
		long nr = get_Proj_proj(proj);
		if (nr == pn_Cond_true) {
			proj_true = proj;
		} else {
			proj_false = proj;
		}
	}

	/* for now, the code works for scheduled and non-schedules blocks */
	block = get_nodes_block(node);

	/* we have a block schedule */
	next_block = (ir_node*)get_irn_link(block);

	if (get_irn_link(proj_true) == next_block) {
		/* exchange both proj's so the second one can be omitted */
		const ir_node *t = proj_true;

		proj_true  = proj_false;
		proj_false = t;
		relation   = get_negated_relation(relation);
	}

	/* emit the true proj */
	sparc_emit_indent();
	be_emit_string(get_cc(relation));
	be_emit_char(' ');
	sparc_emit_cfop_target(proj_true);
	be_emit_finish_line_gas(proj_true);

	fill_delay_slot();

	sparc_emit_indent();
	if (get_irn_link(proj_false) == next_block) {
		be_emit_cstring("/* fallthrough to ");
		sparc_emit_cfop_target(proj_false);
		be_emit_cstring(" */");
		be_emit_finish_line_gas(proj_false);
	} else {
		be_emit_cstring("ba ");
		sparc_emit_cfop_target(proj_false);
		be_emit_finish_line_gas(proj_false);
		fill_delay_slot();
	}
}

static void emit_sparc_Bicc(const ir_node *node)
{
	const sparc_jmp_cond_attr_t *attr = get_sparc_jmp_cond_attr_const(node);
	bool             is_unsigned = attr->is_unsigned;
	emit_sparc_branch(node, is_unsigned ? get_icc_unsigned : get_icc_signed);
}

static void emit_sparc_fbfcc(const ir_node *node)
{
	/* if the flags producing node was immediately in front of us, emit
	 * a nop */
	ir_node *flags = get_irn_n(node, n_sparc_fbfcc_flags);
	ir_node *prev  = sched_prev(node);
	if (is_Block(prev)) {
		/* TODO: when the flags come from another block, then we have to do
		 * more complicated tests to see wether the flag producing node is
		 * potentially in front of us (could happen for fallthroughs) */
		panic("TODO: fbfcc flags come from other block");
	}
	if (skip_Proj(flags) == prev) {
		sparc_emit_indent();
		be_emit_cstring("nop\n");
	}
	emit_sparc_branch(node, get_fcc);
}

static void emit_sparc_Ba(const ir_node *node)
{
	sparc_emit_indent();
	if (ba_is_fallthrough(node)) {
		be_emit_cstring("/* fallthrough to ");
		sparc_emit_cfop_target(node);
		be_emit_cstring(" */");
		be_emit_finish_line_gas(node);
	} else {
		be_emit_cstring("ba ");
		sparc_emit_cfop_target(node);
		be_emit_finish_line_gas(node);
		fill_delay_slot();
	}
}

static void emit_sparc_SwitchJmp(const ir_node *node)
{
	const sparc_switch_jmp_attr_t *attr = get_sparc_switch_jmp_attr_const(node);

	sparc_emit_indent();
	be_emit_cstring("jmp ");
	sparc_emit_source_register(node, 0);
	be_emit_finish_line_gas(node);
	fill_delay_slot();

	be_emit_jump_table(node, attr->table, attr->table_entity, get_jump_target);
}

static void emit_fmov(const ir_node *node, const arch_register_t *src_reg,
                      const arch_register_t *dst_reg)
{
	sparc_emit_indent();
	be_emit_cstring("fmovs %");
	be_emit_string(arch_register_get_name(src_reg));
	be_emit_cstring(", %");
	be_emit_string(arch_register_get_name(dst_reg));
	be_emit_finish_line_gas(node);
}

static const arch_register_t *get_next_fp_reg(const arch_register_t *reg)
{
	unsigned idx = reg->global_index;
	assert(reg == &sparc_registers[idx]);
	idx++;
	assert(idx - REG_F0 < N_sparc_fp_REGS);
	return &sparc_registers[idx];
}

static void emit_be_Copy(const ir_node *node)
{
	ir_mode               *mode    = get_irn_mode(node);
	const arch_register_t *src_reg = arch_get_irn_register_in(node, 0);
	const arch_register_t *dst_reg = arch_get_irn_register_out(node, 0);

	if (src_reg == dst_reg)
		return;

	if (mode_is_float(mode)) {
		unsigned bits = get_mode_size_bits(mode);
		int      n    = bits > 32 ? bits > 64 ? 3 : 1 : 0;
		int      i;
		emit_fmov(node, src_reg, dst_reg);
		for (i = 0; i < n; ++i) {
			src_reg = get_next_fp_reg(src_reg);
			dst_reg = get_next_fp_reg(dst_reg);
			emit_fmov(node, src_reg, dst_reg);
		}
	} else if (mode_is_data(mode)) {
		sparc_emit_indent();
		be_emit_cstring("mov ");
		sparc_emit_source_register(node, 0);
		be_emit_cstring(", ");
		sparc_emit_dest_register(node, 0);
		be_emit_finish_line_gas(node);
	} else {
		panic("emit_be_Copy: invalid mode");
	}
}

static void emit_nothing(const ir_node *irn)
{
	(void) irn;
}

typedef void (*emit_func) (const ir_node *);

static inline void set_emitter(ir_op *op, emit_func sparc_emit_node)
{
	op->ops.generic = (op_func)sparc_emit_node;
}

/**
 * Enters the emitter functions for handled nodes into the generic
 * pointer of an opcode.
 */
static void sparc_register_emitters(void)
{
	/* first clear the generic function pointer for all ops */
	ir_clear_opcodes_generic_func();
	/* register all emitter functions defined in spec */
	sparc_register_spec_emitters();

	/* custom emitter */
	set_emitter(op_be_Copy,         emit_be_Copy);
	set_emitter(op_be_CopyKeep,     emit_be_Copy);
	set_emitter(op_be_IncSP,        emit_be_IncSP);
	set_emitter(op_be_MemPerm,      emit_be_MemPerm);
	set_emitter(op_be_Perm,         emit_be_Perm);
	set_emitter(op_sparc_Ba,        emit_sparc_Ba);
	set_emitter(op_sparc_Bicc,      emit_sparc_Bicc);
	set_emitter(op_sparc_Call,      emit_sparc_Call);
	set_emitter(op_sparc_fbfcc,     emit_sparc_fbfcc);
	set_emitter(op_sparc_FrameAddr, emit_sparc_FrameAddr);
	set_emitter(op_sparc_SMulh,     emit_sparc_Mulh);
	set_emitter(op_sparc_UMulh,     emit_sparc_Mulh);
	set_emitter(op_sparc_Restore,   emit_sparc_Restore);
	set_emitter(op_sparc_Return,    emit_sparc_Return);
	set_emitter(op_sparc_SDiv,      emit_sparc_SDiv);
	set_emitter(op_sparc_SwitchJmp, emit_sparc_SwitchJmp);
	set_emitter(op_sparc_UDiv,      emit_sparc_UDiv);
	set_emitter(op_sparc_Permi,     emit_icore_Permi);
	set_emitter(op_sparc_Permi23,   emit_icore_Permi23);

	/* no need to emit anything for the following nodes */
	set_emitter(op_be_Keep,     emit_nothing);
	set_emitter(op_sparc_Start, emit_nothing);
	set_emitter(op_Phi,         emit_nothing);
}

/**
 * Emits code for a node.
 */
static void sparc_emit_node(const ir_node *node)
{
	ir_op *op = get_irn_op(node);

	if (op->ops.generic) {
		emit_func func = (emit_func) op->ops.generic;
		be_dbg_set_dbg_info(get_irn_dbg_info(node));
		(*func) (node);
	} else {
		panic("No emit handler for node %+F (graph %+F)\n", node,
		      get_irn_irg(node));
	}
}

static ir_node *find_next_delay_slot(ir_node *from)
{
	ir_node *schedpoint = from;
	while (!has_delay_slot(schedpoint)) {
		if (!sched_has_next(schedpoint))
			return NULL;
		schedpoint = sched_next(schedpoint);
	}
	return schedpoint;
}

static bool block_needs_label(const ir_node *block, const ir_node *sched_prev)
{
	int n_cfgpreds;

	if (get_Block_entity(block) != NULL)
		return true;

	n_cfgpreds = get_Block_n_cfgpreds(block);
	if (n_cfgpreds == 0) {
		return false;
	} else if (n_cfgpreds > 1) {
		return true;
	} else {
		ir_node *cfgpred       = get_Block_cfgpred(block, 0);
		ir_node *cfgpred_block = get_nodes_block(cfgpred);
		if (is_Proj(cfgpred) && is_sparc_SwitchJmp(get_Proj_pred(cfgpred)))
			return true;
		return sched_prev != cfgpred_block || get_irn_link(cfgpred) != block;
	}
}

/**
 * Walks over the nodes in a block connected by scheduling edges
 * and emits code for each node.
 */
static void sparc_emit_block(ir_node *block, ir_node *prev)
{
	ir_node *node;
	ir_node *next_delay_slot;

	assert(is_Block(block));

	if (block_needs_label(block, prev)) {
		be_gas_emit_block_name(block);
		be_emit_cstring(":\n");
		be_emit_write_line();
	}

	next_delay_slot = find_next_delay_slot(sched_first(block));
	if (next_delay_slot != NULL)
		delay_slot_filler = pick_delay_slot_for(next_delay_slot);

	sched_foreach(block, node) {
		if (node == delay_slot_filler) {
			continue;
		}

		sparc_emit_node(node);

		if (node == next_delay_slot) {
			assert(delay_slot_filler == NULL);
			next_delay_slot = find_next_delay_slot(sched_next(node));
			if (next_delay_slot != NULL)
				delay_slot_filler = pick_delay_slot_for(next_delay_slot);
		}
	}
}

/**
 * Emits code for function start.
 */
static void sparc_emit_func_prolog(ir_graph *irg)
{
	ir_entity *entity = get_irg_entity(irg);
	be_gas_emit_function_prolog(entity, 4);
}

/**
 * Emits code for function end
 */
static void sparc_emit_func_epilog(ir_graph *irg)
{
	ir_entity *entity = get_irg_entity(irg);
	be_gas_emit_function_epilog(entity);
}

static void sparc_gen_labels(ir_node *block, void *env)
{
	ir_node *pred;
	int n = get_Block_n_cfgpreds(block);
	(void) env;

	for (n--; n >= 0; n--) {
		pred = get_Block_cfgpred(block, n);
		set_irn_link(pred, block); // link the pred of a block (which is a jmp)
	}
}

void sparc_emit_routine(ir_graph *irg)
{
	ir_node **block_schedule;
	size_t    i;
	size_t    n;

	heights = heights_new(irg);

	/* register all emitter functions */
	sparc_register_emitters();

	/* create the block schedule. For now, we don't need it earlier. */
	block_schedule = be_create_block_schedule(irg);

	sparc_emit_func_prolog(irg);
	irg_block_walk_graph(irg, sparc_gen_labels, NULL, NULL);

	/* inject block scheduling links & emit code of each block */
	n = ARR_LEN(block_schedule);
	for (i = 0; i < n; ++i) {
		ir_node *block      = block_schedule[i];
		ir_node *next_block = i+1 < n ? block_schedule[i+1] : NULL;
		set_irn_link(block, next_block);
	}

	for (i = 0; i < n; ++i) {
		ir_node *block = block_schedule[i];
		ir_node *prev  = i>=1 ? block_schedule[i-1] : NULL;
		if (block == get_irg_end_block(irg))
			continue;
		sparc_emit_block(block, prev);
	}

	/* emit function epilog */
	sparc_emit_func_epilog(irg);

	heights_free(heights);
}

static const lc_opt_table_entry_t sparc_emitter_options[] = {
	LC_OPT_ENT_BOOL("raw_permis", "emit raw bytes for permi instruction", &raw_permis),
	LC_OPT_ENT_BOOL("fill_nops", "emit fill nops", &fill_nops),
	LC_OPT_ENT_BOOL("permi_info", "emit permi info", &permi_info),
	LC_OPT_LAST
};

void sparc_init_emitter(void)
{
	lc_opt_entry_t *be_grp;
	lc_opt_entry_t *sparc_grp;
	lc_opt_entry_t *icore_grp;

	be_grp    = lc_opt_get_grp(firm_opt_get_root(), "be");
	sparc_grp = lc_opt_get_grp(be_grp, "sparc");
	icore_grp = lc_opt_get_grp(sparc_grp, "icore");

	lc_opt_add_table(icore_grp, sparc_emitter_options);

	FIRM_DBG_REGISTER(dbg, "firm.be.sparc.emit");
}
