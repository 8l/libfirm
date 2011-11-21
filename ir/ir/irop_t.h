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
 * @brief    Representation of opcode of intermediate operation -- private header.
 * @author   Christian Schaefer, Goetz Lindenmaier, Michael Beck
 */
#ifndef FIRM_IR_IROP_T_H
#define FIRM_IR_IROP_T_H

#include "irop.h"

#include <stdbool.h>

#include "irtypes.h"
#include "tv.h"

/**
 * Frees a newly created ir operation.
 */
void free_ir_op(ir_op *code);

/** Initialize the irop module. */
void init_op(void);

/** Free memory used by irop module. */
void finish_op(void);

/**
 * Copies simply all attributes stored in the old node to the new node.
 * Assumes both have the same opcode and sufficient size.
 *
 * @param old_node  the old node from which the attributes are read
 * @param new_node  the new node to which the attributes are written
 */
void default_copy_attr(ir_graph *irg, const ir_node *old_node,
                       ir_node *new_node);

/**
 * Returns the attribute size of nodes of this opcode.
 * @note Use not encouraged, internal feature.
 */
static inline size_t get_op_attr_size (const ir_op *op)
{
	return op->attr_size;
}

/**
 * Returns non-zero if op is a control flow opcode,
 * like Start, End, Jmp, Cond, Return, Raise or Bad.
 */
static inline bool is_op_cfopcode(const ir_op *op)
{
	return op->flags & irop_flag_cfopcode;
}

static inline bool is_op_unknown_jump(const ir_op *op)
{
	return op->flags & irop_flag_unknown_jump;
}

/** Returns non-zero if operation is commutative */
static inline bool is_op_commutative(const ir_op *op)
{
	return op->flags & irop_flag_commutative;
}

/** Returns non-zero if operation is fragile */
static inline bool is_op_fragile(const ir_op *op)
{
	return op->flags & irop_flag_fragile;
}

/** Returns non-zero if operation is forking control flow */
static inline bool is_op_forking(const ir_op *op)
{
	return op->flags & irop_flag_forking;
}

/** Returns non-zero if operation is a high-level op */
static inline bool is_op_highlevel(const ir_op *op)
{
	return op->flags & irop_flag_highlevel;
}

/** Returns non-zero if operation is a const-like op */
static inline bool is_op_constlike(const ir_op *op)
{
	return op->flags & irop_flag_constlike;
}

static inline bool is_op_uses_memory(const ir_op *op)
{
	return op->flags & irop_flag_uses_memory;
}

/** Returns non-zero if operation must always be optimized */
static inline bool is_op_always_opt(const ir_op *op)
{
	return op->flags & irop_flag_always_opt;
}

/** Returns non-zero if operation is a keep-like op */
static inline bool is_op_keep(const ir_op *op)
{
	return op->flags & irop_flag_keep;
}

/** Returns non-zero if operation must always be placed in the start block. */
static inline bool is_op_start_block_placed(const ir_op *op)
{
	return op->flags & irop_flag_start_block;
}

/** Returns non-zero if operation is CSE neutral */
static inline bool is_op_cse_neutral(const ir_op *op)
{
	return op->flags & irop_flag_cse_neutral;
}

static inline unsigned get_op_code_(const ir_op *op)
{
	return op->code;
}

static inline ident *get_op_ident_(const ir_op *op)
{
	return op->name;
}

static inline op_pin_state get_op_pinned_(const ir_op *op)
{
	return op->pin_state;
}

static inline void set_generic_function_ptr_(ir_op *op, op_func func)
{
	op->ops.generic = func;
}

static inline op_func get_generic_function_ptr_(const ir_op *op)
{
	return op->ops.generic;
}

static inline const ir_op_ops *get_op_ops_(const ir_op *op)
{
	return &op->ops;
}

static inline void set_op_tag_(ir_op *op, unsigned tag)
{
	op->tag = tag;
}

static inline unsigned get_op_tag_(const ir_op *op)
{
	return op->tag;
}

static inline void set_op_attr_(ir_op *op, void *attr)
{
	op->attr = attr;
}

static inline void *get_op_attr_(const ir_op *op)
{
	return op->attr;
}

#define get_op_code(op)         get_op_code_(op)
#define get_op_ident(op)        get_op_ident_(op)
#define get_op_pinned(op)       get_op_pinned_(op)
#define get_op_ops(op)          get_op_ops_(op)
#define set_op_tag(op, tag)     set_op_tag_((op), (tag))
#define get_op_tag(op)          get_op_tag_(op)
#define set_op_attr(op, attr)   set_op_attr_((op), (attr))
#define get_op_attr(op)         get_op_attr_(op)

#endif
