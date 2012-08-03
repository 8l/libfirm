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
 * @brief   arm emitter
 * @author  Oliver Richter, Tobias Gneist, Michael Beck
 */
#include "config.h"

#include <limits.h>
#include <stdbool.h>

#include "xmalloc.h"
#include "tv.h"
#include "iredges.h"
#include "debug.h"
#include "irgwalk.h"
#include "irtools.h"
#include "irprintf.h"
#include "irop_t.h"
#include "irprog_t.h"
#include "irargs_t.h"
#include "error.h"
#include "raw_bitset.h"
#include "dbginfo.h"

#include "besched.h"
#include "beblocksched.h"
#include "beirg.h"
#include "begnuas.h"
#include "bedwarf.h"

#include "arm_emitter.h"
#include "arm_optimize.h"
#include "gen_arm_emitter.h"
#include "arm_nodes_attr.h"
#include "arm_new_nodes.h"
#include "arm_map_regs.h"
#include "gen_arm_regalloc_if.h"

#include "benode.h"

DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

static set       *sym_or_tv;
static arm_isa_t *isa;

static void arm_emit_register(const arch_register_t *reg)
{
	be_emit_string(arch_register_get_name(reg));
}

static void arm_emit_source_register(const ir_node *node, int pos)
{
	const arch_register_t *reg = arch_get_irn_register_in(node, pos);
	arm_emit_register(reg);
}

static void arm_emit_dest_register(const ir_node *node, int pos)
{
	const arch_register_t *reg = arch_get_irn_register_out(node, pos);
	arm_emit_register(reg);
}

static void arm_emit_offset(const ir_node *node)
{
	const arm_load_store_attr_t *attr = get_arm_load_store_attr_const(node);
	assert(attr->base.is_load_store);

	be_emit_irprintf("0x%X", attr->offset);
}

/**
 * Emit the arm fpa instruction suffix depending on the mode.
 */
static void arm_emit_fpa_postfix(const ir_mode *mode)
{
	int bits = get_mode_size_bits(mode);
	char c = 'e';

	if (bits == 32)
		c = 's';
	else if (bits == 64)
		c = 'd';
	be_emit_char(c);
}

static void arm_emit_float_load_store_mode(const ir_node *node)
{
	const arm_load_store_attr_t *attr = get_arm_load_store_attr_const(node);
	arm_emit_fpa_postfix(attr->load_store_mode);
}

static void arm_emit_float_arithmetic_mode(const ir_node *node)
{
	const arm_farith_attr_t *attr = get_arm_farith_attr_const(node);
	arm_emit_fpa_postfix(attr->mode);
}

static void arm_emit_symconst(const ir_node *node)
{
	const arm_SymConst_attr_t *symconst = get_arm_SymConst_attr_const(node);
	ir_entity                 *entity   = symconst->entity;

	be_gas_emit_entity(entity);

	/* TODO do something with offset */
}

static void arm_emit_load_mode(const ir_node *node)
{
	const arm_load_store_attr_t *attr = get_arm_load_store_attr_const(node);
	ir_mode *mode      = attr->load_store_mode;
	int      bits      = get_mode_size_bits(mode);
	bool     is_signed = mode_is_signed(mode);
	if (bits == 16) {
		be_emit_string(is_signed ? "sh" : "h");
	} else if (bits == 8) {
		be_emit_string(is_signed ? "sb" : "b");
	} else {
		assert(bits == 32);
	}
}

static void arm_emit_store_mode(const ir_node *node)
{
	const arm_load_store_attr_t *attr = get_arm_load_store_attr_const(node);
	ir_mode *mode      = attr->load_store_mode;
	int      bits      = get_mode_size_bits(mode);
	if (bits == 16) {
		be_emit_cstring("h");
	} else if (bits == 8) {
		be_emit_cstring("b");
	} else {
		assert(bits == 32);
	}
}

static void emit_shf_mod_name(arm_shift_modifier_t mod)
{
	switch (mod) {
	case ARM_SHF_ASR_REG:
	case ARM_SHF_ASR_IMM:
		be_emit_cstring("asr");
		return;
	case ARM_SHF_LSL_REG:
	case ARM_SHF_LSL_IMM:
		be_emit_cstring("lsl");
		return;
	case ARM_SHF_LSR_REG:
	case ARM_SHF_LSR_IMM:
		be_emit_cstring("lsr");
		return;
	case ARM_SHF_ROR_REG:
	case ARM_SHF_ROR_IMM:
		be_emit_cstring("ror");
		return;
	default:
		break;
	}
	panic("can't emit this shf_mod_name %d", (int) mod);
}

static void arm_emit_shifter_operand(const ir_node *node)
{
	const arm_shifter_operand_t *attr = get_arm_shifter_operand_attr_const(node);

	switch (attr->shift_modifier) {
	case ARM_SHF_REG:
		arm_emit_source_register(node, get_irn_arity(node) - 1);
		return;
	case ARM_SHF_IMM: {
		unsigned val = attr->immediate_value;
		val = (val >> attr->shift_immediate)
			| (val << (32-attr->shift_immediate));
		val &= 0xFFFFFFFF;
		be_emit_irprintf("#0x%X", val);
		return;
	}
	case ARM_SHF_ASR_IMM:
	case ARM_SHF_LSL_IMM:
	case ARM_SHF_LSR_IMM:
	case ARM_SHF_ROR_IMM:
		arm_emit_source_register(node, get_irn_arity(node) - 1);
		be_emit_cstring(", ");
		emit_shf_mod_name(attr->shift_modifier);
		be_emit_irprintf(" #0x%X", attr->shift_immediate);
		return;

	case ARM_SHF_ASR_REG:
	case ARM_SHF_LSL_REG:
	case ARM_SHF_LSR_REG:
	case ARM_SHF_ROR_REG:
		arm_emit_source_register(node, get_irn_arity(node) - 2);
		be_emit_cstring(", ");
		emit_shf_mod_name(attr->shift_modifier);
		be_emit_cstring(" ");
		arm_emit_source_register(node, get_irn_arity(node) - 1);
		return;

	case ARM_SHF_RRX:
		arm_emit_source_register(node, get_irn_arity(node) - 1);
		panic("RRX shifter emitter TODO");

	case ARM_SHF_INVALID:
		break;
	}
	panic("Invalid shift_modifier while emitting %+F", node);
}

/** An entry in the sym_or_tv set. */
typedef struct sym_or_tv_t {
	union {
		ir_entity  *entity;  /**< An entity. */
		ir_tarval  *tv;      /**< A tarval. */
		const void *generic; /**< For generic compare. */
	} u;
	unsigned label;      /**< the associated label. */
	bool     is_entity;  /**< true if an entity is stored. */
} sym_or_tv_t;

/**
 * Returns a unique label. This number will not be used a second time.
 */
static unsigned get_unique_label(void)
{
	static unsigned id = 0;
	return ++id;
}

static void emit_constant_name(const sym_or_tv_t *entry)
{
	be_emit_irprintf("%sC%u", be_gas_get_private_prefix(), entry->label);
}

/**
 * Returns the target block for a control flow node.
 */
static ir_node *get_cfop_target_block(const ir_node *irn)
{
	return (ir_node*)get_irn_link(irn);
}

/**
 * Emit the target label for a control flow node.
 */
static void arm_emit_cfop_target(const ir_node *irn)
{
	ir_node *block = get_cfop_target_block(irn);

	be_gas_emit_block_name(block);
}

void arm_emitf(const ir_node *node, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	be_emit_char('\t');
	for (;;) {
		const char *start = format;
		while (*format != '%' && *format != '\n'  && *format != '\0')
			++format;
		be_emit_string_len(start, format - start);

		if (*format == '\0')
			break;

		if (*format == '\n') {
			++format;
			be_emit_char('\n');
			be_emit_write_line();
			be_emit_char('\t');
			continue;
		}

		++format;

		switch (*format++) {
		case '%':
			be_emit_char('%');
			break;

		case 'S': {
			if (*format < '0' || '9' <= *format)
				goto unknown;
			unsigned const pos = *format++ - '0';
			arm_emit_source_register(node, pos);
			break;
		}

		case 'D': {
			if (*format < '0' || '9' <= *format)
				goto unknown;
			unsigned const pos = *format++ - '0';
			arm_emit_dest_register(node, pos);
			break;
		}

		case 'I':
			arm_emit_symconst(node);
			break;

		case 'o':
			arm_emit_offset(node);
			break;

		case 'O':
			arm_emit_shifter_operand(node);
			break;

		case 'C': {
			const sym_or_tv_t *name = va_arg(ap, const sym_or_tv_t*);
			emit_constant_name(name);
			break;
		}

		case 'm': {
			ir_mode *mode = va_arg(ap, ir_mode*);
			arm_emit_fpa_postfix(mode);
			break;
		}

		case 'M':
			switch (*format++) {
			case 'L': arm_emit_load_mode(node);             break;
			case 'S': arm_emit_store_mode(node);            break;
			case 'A': arm_emit_float_arithmetic_mode(node); break;
			case 'F': arm_emit_float_load_store_mode(node); break;
			default:
				--format;
				goto unknown;
			}
			break;

		case 'X': {
			int num = va_arg(ap, int);
			be_emit_irprintf("%X", num);
			break;
		}

		case 'u': {
			unsigned num = va_arg(ap, unsigned);
			be_emit_irprintf("%u", num);
			break;
		}

		case 'd': {
			int num = va_arg(ap, int);
			be_emit_irprintf("%d", num);
			break;
		}

		case 's': {
			const char *string = va_arg(ap, const char *);
			be_emit_string(string);
			break;
		}

		case 'r': {
			arch_register_t *reg = va_arg(ap, arch_register_t*);
			arm_emit_register(reg);
			break;
		}

		case 't': {
			const ir_node *n = va_arg(ap, const ir_node*);
			arm_emit_cfop_target(n);
			break;
		}

		default:
unknown:
			panic("unknown format conversion");
		}
	}
	va_end(ap);
	be_emit_finish_line_gas(node);
}

/**
 * Emit a SymConst.
 */
static void emit_arm_SymConst(const ir_node *irn)
{
	const arm_SymConst_attr_t *attr = get_arm_SymConst_attr_const(irn);
	sym_or_tv_t key, *entry;

	key.u.entity  = attr->entity;
	key.is_entity = true;
	key.label     = 0;
	entry = set_insert(sym_or_tv_t, sym_or_tv, &key, sizeof(key), hash_ptr(key.u.generic));
	if (entry->label == 0) {
		/* allocate a label */
		entry->label = get_unique_label();
	}

	/* load the symbol indirect */
	arm_emitf(irn, "ldr %D0, %C", entry);
}

static void emit_arm_FrameAddr(const ir_node *irn)
{
	const arm_SymConst_attr_t *attr = get_arm_SymConst_attr_const(irn);
	arm_emitf(irn, "add %D0, %S0, #0x%X", attr->fp_offset);
}

/**
 * Emit a floating point fpa constant.
 */
static void emit_arm_fConst(const ir_node *irn)
{
	sym_or_tv_t key;

	key.u.tv      = get_fConst_value(irn);
	key.is_entity = false;
	key.label     = 0;
	sym_or_tv_t *entry = set_insert(sym_or_tv_t, sym_or_tv, &key, sizeof(key), hash_ptr(key.u.generic));
	if (entry->label == 0) {
		/* allocate a label */
		entry->label = get_unique_label();
	}

	/* load the tarval indirect */
	ir_mode *mode = get_irn_mode(irn);
	arm_emitf(irn, "ldf%m %D0, %C", mode, entry);
}

/**
 * Returns the next block in a block schedule.
 */
static ir_node *sched_next_block(const ir_node *block)
{
    return (ir_node*)get_irn_link(block);
}

/**
 * Emit a Compare with conditional branch.
 */
static void emit_arm_B(const ir_node *irn)
{
	const ir_node *proj_true  = NULL;
	const ir_node *proj_false = NULL;
	const ir_node *block;
	const ir_node *next_block;
	ir_node *op1 = get_irn_n(irn, 0);
	const char *suffix;
	ir_relation relation = get_arm_CondJmp_relation(irn);
	const arm_cmp_attr_t *cmp_attr = get_arm_cmp_attr_const(op1);
	bool is_signed = !cmp_attr->is_unsigned;

	assert(is_arm_Cmp(op1) || is_arm_Tst(op1));

	foreach_out_edge(irn, edge) {
		ir_node *proj = get_edge_src_irn(edge);
		long nr = get_Proj_proj(proj);
		if (nr == pn_Cond_true) {
			proj_true = proj;
		} else {
			proj_false = proj;
		}
	}

	if (cmp_attr->ins_permuted) {
		relation = get_inversed_relation(relation);
	}

	/* for now, the code works for scheduled and non-schedules blocks */
	block = get_nodes_block(irn);

	/* we have a block schedule */
	next_block = sched_next_block(block);

	assert(relation != ir_relation_false);
	assert(relation != ir_relation_true);

	if (get_cfop_target_block(proj_true) == next_block) {
		/* exchange both proj's so the second one can be omitted */
		const ir_node *t = proj_true;

		proj_true  = proj_false;
		proj_false = t;
		relation   = get_negated_relation(relation);
	}

	switch (relation & (ir_relation_less_equal_greater)) {
		case ir_relation_equal:         suffix = "eq"; break;
		case ir_relation_less:          suffix = is_signed ? "lt" : "lo"; break;
		case ir_relation_less_equal:    suffix = is_signed ? "le" : "ls"; break;
		case ir_relation_greater:       suffix = is_signed ? "gt" : "hi"; break;
		case ir_relation_greater_equal: suffix = is_signed ? "ge" : "hs"; break;
		case ir_relation_less_greater:  suffix = "ne"; break;
		case ir_relation_less_equal_greater: suffix = "al"; break;
		default: panic("Cmp has unsupported relation");
	}

	/* emit the true proj */
	arm_emitf(irn, "b%s %t", suffix, proj_true);

	if (get_cfop_target_block(proj_false) == next_block) {
		if (be_options.verbose_asm) {
			arm_emitf(irn, "/* fallthrough to %t */", proj_false);
		}
	} else {
		arm_emitf(irn, "b %t", proj_false);
	}
}

/** Sort register in ascending order. */
static int reg_cmp(const void *a, const void *b)
{
	const arch_register_t * const *ra = (const arch_register_t**)a;
	const arch_register_t * const *rb = (const arch_register_t**)b;

	return *ra < *rb ? -1 : (*ra != *rb);
}

/**
 * Create the CopyB instruction sequence.
 */
static void emit_arm_CopyB(const ir_node *irn)
{
	const arm_CopyB_attr_t *attr = get_arm_CopyB_attr_const(irn);
	unsigned size = attr->size;
	const arch_register_t *tmpregs[4];

	/* collect the temporary registers and sort them, we need ascending order */
	tmpregs[0] = arch_get_irn_register_in(irn, 2);
	tmpregs[1] = arch_get_irn_register_in(irn, 3);
	tmpregs[2] = arch_get_irn_register_in(irn, 4);
	tmpregs[3] = &arm_registers[REG_R12];

	/* Note: R12 is always the last register because the RA did not assign higher ones */
	qsort((void *)tmpregs, 3, sizeof(tmpregs[0]), reg_cmp);

	if (be_options.verbose_asm) {
		arm_emitf(irn, "/* MemCopy (%S1)->(%S0) [%u bytes], Uses %r, %r, %r and %r */",
		          size, tmpregs[0], tmpregs[1], tmpregs[2], tmpregs[3]);
	}

	assert(size > 0 && "CopyB needs size > 0" );

	if (size & 3) {
		fprintf(stderr, "strange hack enabled: copy more bytes than needed!");
		size += 4;
	}

	size >>= 2;
	switch (size & 3) {
	case 0:
		break;
	case 1:
		arm_emitf(irn, "ldr %r, [%S1, #0]", tmpregs[3]);
		arm_emitf(irn, "str %r, [%S0, #0]", tmpregs[3]);
		break;
	case 2:
		arm_emitf(irn, "ldmia %S1!, {%r, %r}", tmpregs[0], tmpregs[1]);
		arm_emitf(irn, "stmia %S0!, {%r, %r}", tmpregs[0], tmpregs[1]);
		break;
	case 3:
		arm_emitf(irn, "ldmia %S1!, {%r, %r, %r}", tmpregs[0], tmpregs[1], tmpregs[2]);
		arm_emitf(irn, "stmia %S0!, {%r, %r, %r}", tmpregs[0], tmpregs[1], tmpregs[2]);
		break;
	}
	size >>= 2;
	while (size) {
		arm_emitf(irn, "ldmia %S1!, {%r, %r, %r}", tmpregs[0], tmpregs[1], tmpregs[2], tmpregs[3]);
		arm_emitf(irn, "stmia %S0!, {%r, %r, %r}", tmpregs[0], tmpregs[1], tmpregs[2], tmpregs[3]);
		--size;
	}
}

static void emit_arm_SwitchJmp(const ir_node *irn)
{
	const arm_SwitchJmp_attr_t *attr = get_arm_SwitchJmp_attr_const(irn);
	arm_emitf(irn, "ldrls pc, [pc, %S0, asl #2]");

	be_emit_jump_table(irn, attr->table, NULL, get_cfop_target_block);
}

/** Emit an IncSP node */
static void emit_be_IncSP(const ir_node *irn)
{
	int offs = -be_get_IncSP_offset(irn);

	if (offs == 0)
		return;

	const char *op = "add";
	if (offs < 0) {
		op   = "sub";
		offs = -offs;
	}
	arm_emitf(irn, "%s %D0, %S0, #0x%X", op, offs);
}

static void emit_be_Copy(const ir_node *irn)
{
	ir_mode *mode = get_irn_mode(irn);

	if (arch_get_irn_register_in(irn, 0) == arch_get_irn_register_out(irn, 0)) {
		/* omitted Copy */
		return;
	}

	if (mode_is_float(mode)) {
		if (USE_FPA(isa)) {
			arm_emitf(irn, "mvf %D0, %S0");
		} else {
			panic("move not supported for this mode");
		}
	} else if (mode_is_data(mode)) {
		arm_emitf(irn, "mov %D0, %S0");
	} else {
		panic("move not supported for this mode");
	}
}

static void emit_be_Perm(const ir_node *irn)
{
	arm_emitf(irn,
		"eor %S0, %S0, %S1\n"
		"eor %S1, %S0, %S1\n"
		"eor %S0, %S0, %S1");
}

static void emit_be_MemPerm(const ir_node *node)
{
	int i;
	int memperm_arity;
	int sp_change = 0;

	/* TODO: this implementation is slower than necessary.
	   The longterm goal is however to avoid the memperm node completely */

	memperm_arity = be_get_MemPerm_entity_arity(node);
	if (memperm_arity > 12)
		panic("memperm with more than 12 inputs not supported yet");

	for (i = 0; i < memperm_arity; ++i) {
		/* spill register */
		arm_emitf(node, "str r%d, [sp, #-4]!", i);
		sp_change += 4;
		/* load from entity */
		ir_entity *entity = be_get_MemPerm_in_entity(node, i);
		int        offset = get_entity_offset(entity) + sp_change;
		arm_emitf(node, "ldr r%d, [sp, #%d]", i, offset);
	}

	for (i = memperm_arity-1; i >= 0; --i) {
		/* store to new entity */
		ir_entity *entity = be_get_MemPerm_out_entity(node, i);
		int        offset = get_entity_offset(entity) + sp_change;
		arm_emitf(node, "str r%d, [sp, #%d]", i, offset);
		/* restore register */
		arm_emitf(node, "ldr r%d, [sp], #4", i);
		sp_change -= 4;
	}
	assert(sp_change == 0);
}

static void emit_be_Start(const ir_node *node)
{
	ir_graph *irg        = get_irn_irg(node);
	ir_type  *frame_type = get_irg_frame_type(irg);
	unsigned  size       = get_type_size_bytes(frame_type);

	/* allocate stackframe */
	if (size > 0) {
		arm_emitf(node, "sub sp, sp, #0x%X", size);
	}
}

static void emit_be_Return(const ir_node *node)
{
	ir_graph *irg        = get_irn_irg(node);
	ir_type  *frame_type = get_irg_frame_type(irg);
	unsigned  size       = get_type_size_bytes(frame_type);

	/* deallocate stackframe */
	if (size > 0) {
		arm_emitf(node, "add sp, sp, #0x%X", size);
	}
	arm_emitf(node, "mov pc, lr");
}


static void emit_arm_Jmp(const ir_node *node)
{
	ir_node *block, *next_block;

	/* for now, the code works for scheduled and non-schedules blocks */
	block = get_nodes_block(node);

	/* we have a block schedule */
	next_block = sched_next_block(block);
	if (get_cfop_target_block(node) != next_block) {
		arm_emitf(node, "b %t", node);
	} else {
		if (be_options.verbose_asm) {
			arm_emitf(node, "/* fallthrough to %t */", node);
		}
	}
}

static void emit_nothing(const ir_node *irn)
{
	(void) irn;
}

/**
 * The type of a emitter function.
 */
typedef void (emit_func)(const ir_node *irn);

/**
 * Set a node emitter. Make it a bit more type safe.
 */
static inline void set_emitter(ir_op *op, emit_func arm_emit_node)
{
	op->ops.generic = (op_func)arm_emit_node;
}

/**
 * Enters the emitter functions for handled nodes into the generic
 * pointer of an opcode.
 */
static void arm_register_emitters(void)
{
	/* first clear the generic function pointer for all ops */
	ir_clear_opcodes_generic_func();

	/* register all emitter functions defined in spec */
	arm_register_spec_emitters();

	/* custom emitter */
	set_emitter(op_arm_B,         emit_arm_B);
	set_emitter(op_arm_CopyB,     emit_arm_CopyB);
	set_emitter(op_arm_fConst,    emit_arm_fConst);
	set_emitter(op_arm_FrameAddr, emit_arm_FrameAddr);
	set_emitter(op_arm_Jmp,       emit_arm_Jmp);
	set_emitter(op_arm_SwitchJmp, emit_arm_SwitchJmp);
	set_emitter(op_arm_SymConst,  emit_arm_SymConst);
	set_emitter(op_be_Copy,       emit_be_Copy);
	set_emitter(op_be_CopyKeep,   emit_be_Copy);
	set_emitter(op_be_IncSP,      emit_be_IncSP);
	set_emitter(op_be_MemPerm,    emit_be_MemPerm);
	set_emitter(op_be_Perm,       emit_be_Perm);
	set_emitter(op_be_Return,     emit_be_Return);
	set_emitter(op_be_Start,      emit_be_Start);

	/* no need to emit anything for the following nodes */
	set_emitter(op_Phi,           emit_nothing);
	set_emitter(op_be_Keep,       emit_nothing);
}

/**
 * Emits code for a node.
 */
static void arm_emit_node(const ir_node *irn)
{
	ir_op *op = get_irn_op(irn);

	if (op->ops.generic) {
		emit_func *emit = (emit_func *)op->ops.generic;
		be_dwarf_location(get_irn_dbg_info(irn));
		(*emit)(irn);
	} else {
		panic("Error: No emit handler for node %+F (graph %+F)\n",
		      irn, get_irn_irg(irn));
	}
}

/**
 * emit the block label if needed.
 */
static void arm_emit_block_header(ir_node *block, ir_node *prev)
{
	bool need_label = false;
	int  n_cfgpreds;

	n_cfgpreds = get_Block_n_cfgpreds(block);
	if (n_cfgpreds == 1) {
		ir_node *pred       = get_Block_cfgpred(block, 0);
		ir_node *pred_block = get_nodes_block(pred);

		/* we don't need labels for fallthrough blocks, however switch-jmps
		 * are no fallthroughs */
		if (pred_block == prev &&
				!(is_Proj(pred) && is_arm_SwitchJmp(get_Proj_pred(pred)))) {
			need_label = false;
		} else {
			need_label = true;
		}
	} else {
		need_label = true;
	}

	be_gas_begin_block(block, need_label);
}

/**
 * Walks over the nodes in a block connected by scheduling edges
 * and emits code for each node.
 */
static void arm_gen_block(ir_node *block, ir_node *prev_block)
{
	arm_emit_block_header(block, prev_block);
	be_dwarf_location(get_irn_dbg_info(block));
	sched_foreach(block, irn) {
		arm_emit_node(irn);
	}
}

/**
 * Block-walker:
 * Sets labels for control flow nodes (jump target)
 */
static void arm_gen_labels(ir_node *block, void *env)
{
	ir_node *pred;
	int n = get_Block_n_cfgpreds(block);
	(void)env;

	for (n--; n >= 0; n--) {
		pred = get_Block_cfgpred(block, n);
		set_irn_link(pred, block);
	}
}

/**
 * Compare two entries of the symbol or tarval set.
 */
static int cmp_sym_or_tv(const void *elt, const void *key, size_t size)
{
	const sym_or_tv_t *p1 = (const sym_or_tv_t*)elt;
	const sym_or_tv_t *p2 = (const sym_or_tv_t*)key;
	(void) size;

	/* as an identifier NEVER can point to a tarval, it's enough
	   to compare it this way */
	return p1->u.generic != p2->u.generic;
}

void arm_gen_routine(ir_graph *irg)
{
	ir_node          *last_block = NULL;
	ir_entity        *entity     = get_irg_entity(irg);
	const arch_env_t *arch_env   = be_get_irg_arch_env(irg);
	ir_node          **blk_sched;
	size_t           i, n;

	isa = (arm_isa_t*) arch_env;
	sym_or_tv = new_set(cmp_sym_or_tv, 8);

	be_gas_elf_type_char = '%';

	arm_register_emitters();

	/* create the block schedule */
	blk_sched = be_create_block_schedule(irg);

	be_gas_emit_function_prolog(entity, 4, NULL);

	irg_block_walk_graph(irg, arm_gen_labels, NULL, NULL);

	n = ARR_LEN(blk_sched);
	for (i = 0; i < n;) {
		ir_node *block, *next_bl;

		block   = blk_sched[i];
		++i;
		next_bl = i < n ? blk_sched[i] : NULL;

		/* set here the link. the emitter expects to find the next block here */
		set_irn_link(block, next_bl);
		arm_gen_block(block, last_block);
		last_block = block;
	}

	/* emit SymConst values */
	if (set_count(sym_or_tv) > 0) {
		be_emit_cstring("\t.align 2\n");

		foreach_set(sym_or_tv, sym_or_tv_t, entry) {
			emit_constant_name(entry);
			be_emit_cstring(":\n");
			be_emit_write_line();

			if (entry->is_entity) {
				be_emit_cstring("\t.word\t");
				be_gas_emit_entity(entry->u.entity);
				be_emit_char('\n');
				be_emit_write_line();
			} else {
				ir_tarval *tv = entry->u.tv;
				int vi;
				int size = get_mode_size_bytes(get_tarval_mode(tv));

				/* beware: ARM fpa uses big endian format */
				for (vi = ((size + 3) & ~3) - 4; vi >= 0; vi -= 4) {
					/* get 32 bits */
					unsigned v;
					v =            get_tarval_sub_bits(tv, vi+3);
					v = (v << 8) | get_tarval_sub_bits(tv, vi+2);
					v = (v << 8) | get_tarval_sub_bits(tv, vi+1);
					v = (v << 8) | get_tarval_sub_bits(tv, vi+0);
					be_emit_irprintf("\t.word\t%u\n", v);
					be_emit_write_line();
				}
			}
		}
		be_emit_char('\n');
		be_emit_write_line();
	}
	del_set(sym_or_tv);

	be_gas_emit_function_epilog(entity);
}

void arm_init_emitter(void)
{
	FIRM_DBG_REGISTER(dbg, "firm.be.arm.emit");
}
