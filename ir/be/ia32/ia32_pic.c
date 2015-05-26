/*
 * This file is part of libFirm.
 * Copyright (C) 2013 University of Karlsruhe.
 */

/**
 * @file
 * @brief       position independent code adjustments
 * @author      Matthias Braun
 */
#include "bearch_ia32_t.h"

#include "adt/pmap.h"
#include "bearch.h"
#include "begnuas.h"
#include "beirg.h"
#include "be_t.h"
#include "entity_t.h"
#include "ident_t.h"
#include "ircons_t.h"
#include "irgwalk.h"
#include "irnode_t.h"

ia32_pic_style_t ia32_pic_style = IA32_PIC_NONE;

/**
 * Create a trampoline entity for the given method.
 */
static ir_entity *create_trampoline(be_main_env_t *be, ir_entity *method)
{
	ir_type *type   = get_entity_type(method);
	ident   *old_id = get_entity_ld_ident(method);
	ir_type *owner  = be->pic_trampolines_type;
	ir_entity *ent;
	if (ia32_pic_style == IA32_PIC_MACH_O) {
		ident *id = new_id_fmt("%s$stup", old_id);
		ent = new_entity(owner, old_id, type);
		set_entity_ld_ident(ent, id);
		set_entity_visibility(ent, ir_visibility_private);
	} else {
		assert(ia32_pic_style == IA32_PIC_ELF_PLT
		    || ia32_pic_style == IA32_PIC_ELF_NO_PLT);
		ident *id = new_id_fmt("%s@PLT", old_id);
		ent = new_entity(owner, old_id, type);
		set_entity_ld_ident(ent, id);
		set_entity_visibility(ent, ir_visibility_external);
	}

	return ent;
}

/**
 * Returns the trampoline entity for the given method.
 */
static ir_entity *get_trampoline(be_main_env_t *env, ir_entity *method)
{
	ir_entity *result = pmap_get(ir_entity, env->ent_trampoline_map, method);
	if (result == NULL) {
		result = create_trampoline(env, method);
		pmap_insert(env->ent_trampoline_map, method, result);
	}

	return result;
}

static ir_entity *create_pic_symbol(const be_main_env_t *be, ir_entity *entity)
{
	if (ia32_pic_style == IA32_PIC_ELF_PLT
	 || ia32_pic_style == IA32_PIC_ELF_NO_PLT) {
		return new_got_entry_entity(entity);
	} else {
		assert(ia32_pic_style == IA32_PIC_MACH_O);
		ident     *old_id = get_entity_ld_ident(entity);
		ident     *id     = new_id_fmt("%s$non_lazy_ptr", old_id);
		ir_type   *e_type = get_entity_type(entity);
		ir_type   *type   = new_type_pointer(e_type);
		ir_type   *parent = be->pic_symbols_type;
		ir_entity *ent    = new_entity(parent, old_id, type);
		set_entity_ld_ident(ent, id);
		set_entity_visibility(ent, ir_visibility_private);
		return ent;
	}
}

static ir_entity *get_pic_symbol(const be_main_env_t *env, ir_entity *entity)
{
	ir_entity *result = pmap_get(ir_entity, env->ent_pic_symbol_map, entity);
	if (result == NULL) {
		result = create_pic_symbol(env, entity);
		pmap_insert(env->ent_pic_symbol_map, entity, result);
	}

	return result;
}

/**
 * Returns non-zero if a given entity can be accessed using a relative address.
 */
static int can_address_relative(const ir_entity *entity)
{
	if (!entity_has_definition(entity)
	 || (get_entity_linkage(entity) & IR_LINKAGE_MERGE))
		return false;
	/* mach-o has constant offsets into the data segments, so everything else
	 * is fine */
	if (ia32_pic_style == IA32_PIC_MACH_O)
		return true;
	/* we can only directly address things in the text section */
	return get_entity_kind(entity) == IR_ENTITY_METHOD;
}

ir_entity *ia32_lconst_pic_adjust(const be_main_env_t *env, ir_entity *entity)
{
	if (can_address_relative(entity))
		return entity;

	return get_pic_symbol(env, entity);
}

/** patches Addresses to work in position independent code */
static void fix_pic_addresses(ir_node *const node, void *const data)
{
	(void)data;

	ir_graph      *const irg = get_irn_irg(node);
	be_main_env_t *const be  = be_get_irg_main_env(irg);
	foreach_irn_in(node, i, pred) {
		if (!is_Address(pred))
			continue;

		ir_node         *res;
		ir_entity *const entity = get_Address_entity(pred);
		dbg_info  *const dbgi   = get_irn_dbg_info(pred);
		if ((ia32_pic_style == IA32_PIC_ELF_PLT
		  || ia32_pic_style == IA32_PIC_MACH_O)
			&& i == n_Call_ptr && is_Call(node)) {
			/* Calls can jump to relative addresses, so we can directly jump to
			 * the (relatively) known call address or the trampoline */
			if (can_address_relative(entity))
				continue;

			ir_entity *const trampoline = get_trampoline(be, entity);
			res = new_rd_Address(dbgi, irg, trampoline);
		} else if (get_entity_type(entity) == get_code_type()) {
			/* Block labels can always be addressed directly. */
			continue;
		} else {
			/* Everything else is accessed relative to EIP. */
			ir_node *const block    = get_nodes_block(pred);
			ir_mode *const mode     = get_irn_mode(pred);
			ir_node *const pic_base = ia32_get_pic_base(irg);

			if (can_address_relative(entity)) {
				/* All ok now for locally constructed stuff. */
				res = new_rd_Add(dbgi, block, pic_base, pred, mode);
				/* Make sure the walker doesn't visit this add again. */
				mark_irn_visited(res);
			} else {
				/* Get entry from pic symbol segment. */
				ir_entity *const pic_symbol  = get_pic_symbol(be, entity);
				ir_node   *const pic_address = new_rd_Address(dbgi, irg, pic_symbol);
				ir_node   *const add         = new_rd_Add(dbgi, block, pic_base, pic_address, mode);
				mark_irn_visited(add);

				/* We need an extra indirection for global data outside our current
				 * module. The loads are always safe and can therefore float and
				 * need no memory input */
				ir_type *const type  = get_entity_type(entity);
				ir_node *const nomem = get_irg_no_mem(irg);
				ir_node *const load  = new_rd_Load(dbgi, block, nomem, add, mode, type, cons_floats);
				res = new_r_Proj(load, mode, pn_Load_res);
			}
		}
		set_irn_n(node, i, res);
	}
}

void ia32_adjust_pic(ir_graph *irg)
{
	if (ia32_pic_style == IA32_PIC_NONE)
		return;
	irg_walk_graph(irg, fix_pic_addresses, NULL, NULL);
}
