/*
 * This file is part of libFirm.
 * Copyright (C) 2014 University of Karlsruhe.
 */

/**
 * @file
 * @brief   arm graph touchups before emitting
 * @author  Matthias Braun
 */
#include "bearch_arm_t.h"

#include "firm_types.h"
#include "irgwalk.h"

#include "bespillslots.h"
#include "bestack.h"
#include "be_types.h"
#include "beirg.h"
#include "benode.h"
#include "besched.h"

#include "arm_new_nodes.h"
#include "gen_arm_regalloc_if.h"
#include "arm_optimize.h"

static bool is_frame_load(const ir_node *node)
{
	return is_arm_Ldr(node) || is_arm_Ldf(node);
}

static void arm_collect_frame_entity_nodes(ir_node *node, void *data)
{
	if (!is_frame_load(node))
		return;

	const arm_load_store_attr_t *attr = get_arm_load_store_attr_const(node);
	if (!attr->is_frame_entity)
		return;
	const ir_entity *entity = attr->entity;
	if (entity != NULL)
		return;
	const ir_mode *mode = attr->load_store_mode;
	const ir_type *type = get_type_for_mode(mode);

	be_fec_env_t *env = (be_fec_env_t*)data;
	be_load_needs_frame_entity(env, node, type);
}

static void arm_set_frame_entity(ir_node *node, ir_entity *entity,
                                 const ir_type *type)
{
	(void)type;
	arm_load_store_attr_t *attr = get_arm_load_store_attr(node);
	attr->entity = entity;
}

static void introduce_epilog(ir_node *ret)
{
	arch_register_t const *const sp_reg = &arm_registers[REG_SP];
	assert(arch_get_irn_register_req_in(ret, n_arm_Return_sp) == sp_reg->single_req);

	ir_node  *const sp         = get_irn_n(ret, n_arm_Return_sp);
	ir_node  *const block      = get_nodes_block(ret);
	ir_graph *const irg        = get_irn_irg(ret);
	ir_type  *const frame_type = get_irg_frame_type(irg);
	unsigned  const frame_size = get_type_size_bytes(frame_type);
	ir_node  *const incsp      = be_new_IncSP(sp_reg, block, sp, -frame_size, 0);
	set_irn_n(ret, n_arm_Return_sp, incsp);
	sched_add_before(ret, incsp);
}

static void introduce_prolog_epilog(ir_graph *irg)
{
	/* introduce epilog for every return node */
	foreach_irn_in(get_irg_end_block(irg), i, ret) {
		assert(is_arm_Return(ret));
		introduce_epilog(ret);
	}

	const arch_register_t *sp_reg     = &arm_registers[REG_SP];
	ir_node               *start      = get_irg_start(irg);
	ir_node               *block      = get_nodes_block(start);
	ir_node               *initial_sp = be_get_initial_reg_value(irg, sp_reg);
	ir_node               *schedpoint = start;
	ir_type               *frame_type = get_irg_frame_type(irg);
	unsigned               frame_size = get_type_size_bytes(frame_type);

	while (be_is_Keep(sched_next(schedpoint)))
		schedpoint = sched_next(schedpoint);

	ir_node *const incsp = be_new_IncSP(sp_reg, block, initial_sp, frame_size, 0);
	edges_reroute_except(initial_sp, incsp, incsp);
	sched_add_after(schedpoint, incsp);
}

void arm_finish_graph(ir_graph *irg)
{
	be_stack_layout_t *stack_layout = be_get_irg_stack_layout(irg);
	bool               at_begin     = stack_layout->sp_relative;
	be_fec_env_t      *fec_env      = be_new_frame_entity_coalescer(irg);

	irg_walk_graph(irg, NULL, arm_collect_frame_entity_nodes, fec_env);
	be_assign_entities(fec_env, arm_set_frame_entity, at_begin);
	be_free_frame_entity_coalescer(fec_env);

	introduce_prolog_epilog(irg);

	/* fix stack entity offsets */
	be_abi_fix_stack_nodes(irg);
	be_abi_fix_stack_bias(irg);

	/* do peephole optimizations and fix stack offsets */
	arm_peephole_optimization(irg);
}