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
 * @brief   Representation of opcode of intermediate operation.
 * @author  Christian Schaefer, Goetz Lindenmaier, Michael Beck
 */
#include "config.h"

#include <string.h>

#include "irop_t.h"
#include "irnode_t.h"
#include "irhooks.h"
#include "irbackedge_t.h"

#include "iropt_t.h"
#include "irverify_t.h"
#include "reassoc_t.h"

#include "xmalloc.h"
#include "benode.h"

static ir_op **opcodes;
/** the available next opcode */
static unsigned next_iro = iro_MaxOpcode;

static ir_type *default_get_type_attr(const ir_node *node);
static ir_entity *default_get_entity_attr(const ir_node *node);
static unsigned default_hash_node(const ir_node *node);
static void default_copy_attr(ir_graph *irg, const ir_node *old_node,
                              ir_node *new_node);

ir_op *new_ir_op(unsigned code, const char *name, op_pin_state p,
                 irop_flags flags, op_arity opar, int op_index,
                 size_t attr_size)
{
	ir_op *res = XMALLOCZ(ir_op);

	res->code      = code;
	res->name      = new_id_from_chars(name, strlen(name));
	res->pin_state = p;
	res->attr_size = attr_size;
	res->flags     = flags;
	res->opar      = opar;
	res->op_index  = op_index;
	res->tag       = 0;

	memset(&res->ops, 0, sizeof(res->ops));
	res->ops.hash            = default_hash_node;
	res->ops.copy_attr       = default_copy_attr;
	res->ops.get_type_attr   = default_get_type_attr;
	res->ops.get_entity_attr = default_get_entity_attr;

	{
		size_t len = ARR_LEN(opcodes);
		if ((size_t)code >= len) {
			ARR_RESIZE(ir_op*, opcodes, (size_t)code+1);
			memset(&opcodes[len], 0, (code-len+1) * sizeof(opcodes[0]));
		}
		if (opcodes[code] != NULL)
			panic("opcode registered twice");
		opcodes[code] = res;
	}

	hook_new_ir_op(res);
	return res;
}

void free_ir_op(ir_op *code)
{
	hook_free_ir_op(code);

	assert(opcodes[code->code] == code);
	opcodes[code->code] = NULL;

	free(code);
}

unsigned ir_get_n_opcodes(void)
{
	return ARR_LEN(opcodes);
}

ir_op *ir_get_opcode(unsigned code)
{
	assert((size_t)code < ARR_LEN(opcodes));
	return opcodes[code];
}

void ir_clear_opcodes_generic_func(void)
{
	size_t n = ir_get_n_opcodes();
	size_t i;

	for (i = 0; i < n; ++i) {
		ir_op *op = ir_get_opcode(i);
		if (op == NULL)
			continue;
		op->ops.generic  = (op_func)NULL;
		op->ops.generic1 = (op_func)NULL;
		op->ops.generic2 = (op_func)NULL;
	}
}

void ir_op_set_memory_index(ir_op *op, int memory_index)
{
	assert(op->flags & irop_flag_uses_memory);
	op->memory_index = memory_index;
}

void ir_op_set_fragile_indices(ir_op *op, int pn_x_regular, int pn_x_except)
{
	assert(op->flags & irop_flag_fragile);
	op->pn_x_regular = pn_x_regular;
	op->pn_x_except = pn_x_except;
}

const char *get_op_name (const ir_op *op)
{
	return get_id_str(op->name);
}

unsigned (get_op_code)(const ir_op *op)
{
  return get_op_code_(op);
}

ident *(get_op_ident)(const ir_op *op)
{
  return get_op_ident_(op);
}

const char *get_op_pin_state_name(op_pin_state s)
{
	switch (s) {
#define XXX(s) case s: return #s
	XXX(op_pin_state_floats);
	XXX(op_pin_state_pinned);
	XXX(op_pin_state_exc_pinned);
	XXX(op_pin_state_mem_pinned);
#undef XXX
	}
	return "<none>";
}

op_pin_state (get_op_pinned)(const ir_op *op)
{
	return get_op_pinned_(op);
}

void set_op_pinned(ir_op *op, op_pin_state pinned)
{
	if (op == op_Block || op == op_Phi || is_op_cfopcode(op)) return;
	op->pin_state = pinned;
}

unsigned get_next_ir_opcode(void)
{
	return next_iro++;
}

unsigned get_next_ir_opcodes(unsigned num)
{
	unsigned base = next_iro;
	next_iro += num;
	return base;
}

op_func (get_generic_function_ptr)(const ir_op *op)
{
	return get_generic_function_ptr_(op);
}

void (set_generic_function_ptr)(ir_op *op, op_func func)
{
	set_generic_function_ptr_(op, func);
}

ir_op_ops *(get_op_ops)(ir_op *op)
{
	return get_op_ops_(op);
}

irop_flags get_op_flags(const ir_op *op)
{
	return (irop_flags)op->flags;
}

static ir_type *default_get_type_attr(const ir_node *node)
{
	(void)node;
	return get_unknown_type();
}

static ir_entity *default_get_entity_attr(const ir_node *node)
{
	(void)node;
	return NULL;
}

static unsigned default_hash_node(const ir_node *node)
{
	unsigned h;
	int i, irn_arity;

	/* hash table value = 9*(9*(9*(9*(9*arity+in[0])+in[1])+ ...)+mode)+code */
	h = irn_arity = get_irn_arity(node);

	/* consider all in nodes... except the block if not a control flow. */
	for (i = is_cfop(node) ? -1 : 0;  i < irn_arity;  ++i) {
		ir_node *pred = get_irn_n(node, i);
		if (is_irn_cse_neutral(pred))
			h *= 9;
		else
			h = 9*h + hash_ptr(pred);
	}

	/* ...mode,... */
	h = 9*h + hash_ptr(get_irn_mode(node));
	/* ...and code */
	h = 9*h + hash_ptr(get_irn_op(node));

	return h;
}

/**
 * Calculate a hash value of a Const node.
 */
static unsigned hash_Const(const ir_node *node)
{
	unsigned h;

	/* special value for const, as they only differ in their tarval. */
	h = hash_ptr(node->attr.con.tarval);

	return h;
}

/**
 * Calculate a hash value of a SymConst node.
 */
static unsigned hash_SymConst(const ir_node *node)
{
	unsigned h;

	/* all others are pointers */
	h = hash_ptr(node->attr.symc.sym.type_p);

	return h;
}

/** Compares two exception attributes */
static int node_cmp_exception(const ir_node *a, const ir_node *b)
{
	const except_attr *ea = &a->attr.except;
	const except_attr *eb = &b->attr.except;
	return ea->pin_state != eb->pin_state;
}

/** Compares the attributes of two Const nodes. */
static int node_cmp_attr_Const(const ir_node *a, const ir_node *b)
{
	return get_Const_tarval(a) != get_Const_tarval(b);
}

/** Compares the attributes of two Proj nodes. */
static int node_cmp_attr_Proj(const ir_node *a, const ir_node *b)
{
	return a->attr.proj.proj != b->attr.proj.proj;
}

/** Compares the attributes of two Alloc nodes. */
static int node_cmp_attr_Alloc(const ir_node *a, const ir_node *b)
{
	const alloc_attr *pa = &a->attr.alloc;
	const alloc_attr *pb = &b->attr.alloc;
	if (pa->where != pb->where || pa->type != pb->type)
		return 1;
	return node_cmp_exception(a, b);
}

/** Compares the attributes of two Free nodes. */
static int node_cmp_attr_Free(const ir_node *a, const ir_node *b)
{
	const free_attr *pa = &a->attr.free;
	const free_attr *pb = &b->attr.free;
	return (pa->where != pb->where) || (pa->type != pb->type);
}

/** Compares the attributes of two SymConst nodes. */
static int node_cmp_attr_SymConst(const ir_node *a, const ir_node *b)
{
	const symconst_attr *pa = &a->attr.symc;
	const symconst_attr *pb = &b->attr.symc;
	return (pa->kind       != pb->kind)
	    || (pa->sym.type_p != pb->sym.type_p);
}

/** Compares the attributes of two Call nodes. */
static int node_cmp_attr_Call(const ir_node *a, const ir_node *b)
{
	const call_attr *pa = &a->attr.call;
	const call_attr *pb = &b->attr.call;
	if (pa->type != pb->type)
		return 1;
	return node_cmp_exception(a, b);
}

/** Compares the attributes of two Sel nodes. */
static int node_cmp_attr_Sel(const ir_node *a, const ir_node *b)
{
	const ir_entity *a_ent = get_Sel_entity(a);
	const ir_entity *b_ent = get_Sel_entity(b);
	return a_ent != b_ent;
}

/** Compares the attributes of two Phi nodes. */
static int node_cmp_attr_Phi(const ir_node *a, const ir_node *b)
{
	(void) b;
	/* do not CSE Phi-nodes without any inputs when building new graphs */
	if (get_irn_arity(a) == 0 &&
		irg_is_constrained(get_irn_irg(a), IR_GRAPH_CONSTRAINT_CONSTRUCTION)) {
	    return 1;
	}
	return 0;
}

/** Compares the attributes of two Cast nodes. */
static int node_cmp_attr_Cast(const ir_node *a, const ir_node *b)
{
	return get_Cast_type(a) != get_Cast_type(b);
}

/** Compares the attributes of two Load nodes. */
static int node_cmp_attr_Load(const ir_node *a, const ir_node *b)
{
	if (get_Load_volatility(a) == volatility_is_volatile ||
	    get_Load_volatility(b) == volatility_is_volatile)
		/* NEVER do CSE on volatile Loads */
		return 1;
	/* do not CSE Loads with different alignment. Be conservative. */
	if (get_Load_unaligned(a) != get_Load_unaligned(b))
		return 1;
	if (get_Load_mode(a) != get_Load_mode(b))
		return 1;
	return node_cmp_exception(a, b);
}

/** Compares the attributes of two Store nodes. */
static int node_cmp_attr_Store(const ir_node *a, const ir_node *b)
{
	/* do not CSE Stores with different alignment. Be conservative. */
	if (get_Store_unaligned(a) != get_Store_unaligned(b))
		return 1;
	/* NEVER do CSE on volatile Stores */
	if (get_Store_volatility(a) == volatility_is_volatile ||
	    get_Store_volatility(b) == volatility_is_volatile)
		return 1;
	return node_cmp_exception(a, b);
}

static int node_cmp_attr_CopyB(const ir_node *a, const ir_node *b)
{
	if (get_CopyB_type(a) != get_CopyB_type(b))
		return 1;

	return node_cmp_exception(a, b);
}

static int node_cmp_attr_Bound(const ir_node *a, const ir_node *b)
{
	return node_cmp_exception(a, b);
}

/** Compares the attributes of two Div nodes. */
static int node_cmp_attr_Div(const ir_node *a, const ir_node *b)
{
	const div_attr *ma = &a->attr.div;
	const div_attr *mb = &b->attr.div;
	if (ma->resmode != mb->resmode || ma->no_remainder  != mb->no_remainder)
		return 1;
	return node_cmp_exception(a, b);
}

/** Compares the attributes of two Mod nodes. */
static int node_cmp_attr_Mod(const ir_node *a, const ir_node *b)
{
	const mod_attr *ma = &a->attr.mod;
	const mod_attr *mb = &b->attr.mod;
	if (ma->resmode != mb->resmode)
		return 1;
	return node_cmp_exception(a, b);
}

static int node_cmp_attr_Cmp(const ir_node *a, const ir_node *b)
{
	const cmp_attr *ma = &a->attr.cmp;
	const cmp_attr *mb = &b->attr.cmp;
	return ma->relation != mb->relation;
}

/** Compares the attributes of two Confirm nodes. */
static int node_cmp_attr_Confirm(const ir_node *a, const ir_node *b)
{
	const confirm_attr *ma = &a->attr.confirm;
	const confirm_attr *mb = &b->attr.confirm;
	return ma->relation != mb->relation;
}

/** Compares the attributes of two Builtin nodes. */
static int node_cmp_attr_Builtin(const ir_node *a, const ir_node *b)
{
	if (get_Builtin_kind(a) != get_Builtin_kind(b))
		return 1;
	if (get_Builtin_type(a) != get_Builtin_type(b))
		return 1;
	return node_cmp_exception(a, b);
}

/** Compares the attributes of two ASM nodes. */
static int node_cmp_attr_ASM(const ir_node *a, const ir_node *b)
{
	if (get_ASM_text(a) != get_ASM_text(b))
		return 1;

	int n_inputs = get_ASM_n_inputs(a);
	if (n_inputs != get_ASM_n_inputs(b))
		return 1;

	const ir_asm_constraint *in_a = get_ASM_input_constraints(a);
	const ir_asm_constraint *in_b = get_ASM_input_constraints(b);
	for (int i = 0; i < n_inputs; ++i) {
		if (in_a[i].pos != in_b[i].pos
		    || in_a[i].constraint != in_b[i].constraint
		    || in_a[i].mode != in_b[i].mode)
			return 1;
	}

	size_t n_outputs = get_ASM_n_output_constraints(a);
	if (n_outputs != get_ASM_n_output_constraints(b))
		return 1;

	const ir_asm_constraint *out_a = get_ASM_output_constraints(a);
	const ir_asm_constraint *out_b = get_ASM_output_constraints(b);
	for (size_t i = 0; i < n_outputs; ++i) {
		if (out_a[i].pos != out_b[i].pos
		    || out_a[i].constraint != out_b[i].constraint
		    || out_a[i].mode != out_b[i].mode)
			return 1;
	}

	size_t n_clobbers = get_ASM_n_clobbers(a);
	if (n_clobbers != get_ASM_n_clobbers(b))
		return 1;

	ident **cla = get_ASM_clobbers(a);
	ident **clb = get_ASM_clobbers(b);
	for (size_t i = 0; i < n_clobbers; ++i) {
		if (cla[i] != clb[i])
			return 1;
	}

	return node_cmp_exception(a, b);
}

/** Compares the inexistent attributes of two Dummy nodes. */
static int node_cmp_attr_Dummy(const ir_node *a, const ir_node *b)
{
	(void) a;
	(void) b;
	/* Dummy nodes never equal by definition */
	return 1;
}

static int node_cmp_attr_InstOf(const ir_node *a, const ir_node *b)
{
	if (get_InstOf_type(a) != get_InstOf_type(b))
		return 1;
	return node_cmp_exception(a, b);
}

static void default_copy_attr(ir_graph *irg, const ir_node *old_node,
                              ir_node *new_node)
{
	(void) irg;

	assert(get_irn_op(old_node) == get_irn_op(new_node));
	memcpy(&new_node->attr, &old_node->attr, get_op_attr_size(get_irn_op(old_node)));
}

/**
 * Copies all Call attributes stored in the old node to the new node.
 */
static void call_copy_attr(ir_graph *irg, const ir_node *old_node,
                           ir_node *new_node)
{
	default_copy_attr(irg, old_node, new_node);
	remove_Call_callee_arr(new_node);
}

/**
 * Copies all Block attributes stored in the old node to the new node.
 */
static void block_copy_attr(ir_graph *irg, const ir_node *old_node,
                            ir_node *new_node)
{
	default_copy_attr(irg, old_node, new_node);
	new_node->attr.block.irg.irg       = irg;
	new_node->attr.block.phis          = NULL;
	new_node->attr.block.backedge      = new_backedge_arr(irg->obst, get_irn_arity(new_node));
	new_node->attr.block.block_visited = 0;
	memset(&new_node->attr.block.dom, 0, sizeof(new_node->attr.block.dom));
	memset(&new_node->attr.block.pdom, 0, sizeof(new_node->attr.block.pdom));
	/* It should be safe to copy the entity here, as it has no back-link to the old block.
	 * It serves just as a label number, so copying a labeled block results in an exact copy.
	 * This is at least what we need for DCE to work. */
	new_node->attr.block.entity         = old_node->attr.block.entity;
	new_node->attr.block.phis           = NULL;
}

/**
 * Copies all phi attributes stored in old node to the new node
 */
static void phi_copy_attr(ir_graph *irg, const ir_node *old_node,
                          ir_node *new_node)
{
	default_copy_attr(irg, old_node, new_node);
	new_node->attr.phi.next       = NULL;
	new_node->attr.phi.u.backedge = new_backedge_arr(irg->obst, get_irn_arity(new_node));
}

/**
 * Copies all ASM attributes stored in old node to the new node
 */
static void ASM_copy_attr(ir_graph *irg, const ir_node *old_node,
                          ir_node *new_node)
{
	default_copy_attr(irg, old_node, new_node);
	new_node->attr.assem.input_constraints  = DUP_ARR_D(ir_asm_constraint, irg->obst, old_node->attr.assem.input_constraints);
	new_node->attr.assem.output_constraints = DUP_ARR_D(ir_asm_constraint, irg->obst, old_node->attr.assem.output_constraints);
	new_node->attr.assem.clobbers = DUP_ARR_D(ident*, irg->obst, old_node->attr.assem.clobbers);
}

static void switch_copy_attr(ir_graph *irg, const ir_node *old_node,
                             ir_node *new_node)
{
	const ir_switch_table *table = get_Switch_table(old_node);
	new_node->attr.switcha.table = ir_switch_table_duplicate(irg, table);
	new_node->attr.switcha.n_outs = old_node->attr.switcha.n_outs;
}

static void register_node_cmp_func(ir_op *op, node_cmp_attr_func func)
{
	op->ops.node_cmp_attr = func;
}

static void register_node_hash_func(ir_op *op, hash_func func)
{
	op->ops.hash = func;
}

static void register_node_copy_attr_func(ir_op *op, copy_attr_func func)
{
	op->ops.copy_attr = func;
}

static void generated_init_op(void);
static void generated_finish_op(void);

void firm_init_op(void)
{
	opcodes = NEW_ARR_F(ir_op*, 0);
	generated_init_op();
	be_init_op();

	register_node_cmp_func(op_ASM,      node_cmp_attr_ASM);
	register_node_cmp_func(op_Alloc,    node_cmp_attr_Alloc);
	register_node_cmp_func(op_Bound,    node_cmp_attr_Bound);
	register_node_cmp_func(op_Builtin,  node_cmp_attr_Builtin);
	register_node_cmp_func(op_Call,     node_cmp_attr_Call);
	register_node_cmp_func(op_Cast,     node_cmp_attr_Cast);
	register_node_cmp_func(op_Cmp,      node_cmp_attr_Cmp);
	register_node_cmp_func(op_Confirm,  node_cmp_attr_Confirm);
	register_node_cmp_func(op_Const,    node_cmp_attr_Const);
	register_node_cmp_func(op_CopyB,    node_cmp_attr_CopyB);
	register_node_cmp_func(op_Div,      node_cmp_attr_Div);
	register_node_cmp_func(op_Dummy,    node_cmp_attr_Dummy);
	register_node_cmp_func(op_Free,     node_cmp_attr_Free);
	register_node_cmp_func(op_InstOf,   node_cmp_attr_InstOf);
	register_node_cmp_func(op_Load,     node_cmp_attr_Load);
	register_node_cmp_func(op_Mod,      node_cmp_attr_Mod);
	register_node_cmp_func(op_Phi,      node_cmp_attr_Phi);
	register_node_cmp_func(op_Proj,     node_cmp_attr_Proj);
	register_node_cmp_func(op_Sel,      node_cmp_attr_Sel);
	register_node_cmp_func(op_Store,    node_cmp_attr_Store);
	register_node_cmp_func(op_SymConst, node_cmp_attr_SymConst);

	register_node_hash_func(op_Const,    hash_Const);
	register_node_hash_func(op_SymConst, hash_SymConst);

	register_node_copy_attr_func(op_Call,   call_copy_attr);
	register_node_copy_attr_func(op_Block,  block_copy_attr);
	register_node_copy_attr_func(op_Phi,    phi_copy_attr);
	register_node_copy_attr_func(op_ASM,    ASM_copy_attr);
	register_node_copy_attr_func(op_Switch, switch_copy_attr);

	ir_register_opt_node_ops();
	ir_register_reassoc_node_ops();
	ir_register_verify_node_ops();
}

void firm_finish_op(void)
{
	be_finish_op();
	generated_finish_op();
	DEL_ARR_F(opcodes);
	opcodes = NULL;
}

#include "gen_irop.c.inl"
