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
 * @brief   restarting SSA construction for values.
 * @author  Michael Beck
 */
#include "config.h"

#include "ircons_t.h"
#include "irgraph_t.h"
#include "irnode_t.h"
#include "irgwalk.h"

/** Note: start and finish must use the same kind of walker */
static void (*ssa_cons_walker)(ir_graph *, irg_walk_func *, irg_walk_func *, void *)
	= irg_block_walk_graph;

/**
 * Post-walker: prepare the graph nodes for new SSA construction cycle by
 * allocation new arrays.
 */
static void prepare_blocks(ir_node *block, void *env)
{
	unsigned        n_loc = current_ir_graph->n_loc;
	struct obstack *obst  = current_ir_graph->obst;
	(void)env;
	/* reset mature flag */
	set_Block_matured(block, 0);
	block->attr.block.graph_arr = NEW_ARR_D(ir_node *, obst, n_loc);
	memset(block->attr.block.graph_arr, 0, sizeof(ir_node*) * n_loc);
	set_Block_phis(block, NULL);
}

void ssa_cons_start(ir_graph *irg, int n_loc)
{
	/* for now we support only phase_high graphs */
	assert(irg->phase_state == phase_high);

	/* reset the phase to phase building: some optimization might depend on it */
	set_irg_phase_state(irg, phase_building);

	irg_set_nloc(irg, n_loc);

	/*
	 * Note: we could try to reuse existing frag arrays, but it does not
	 * seems worth to do this.  First, we have to check if they really exists and
	 * then clear them.  We do not expect SSA construction is used often.
	 */
	ssa_cons_walker(irg, NULL, prepare_blocks, NULL);
}

/**
 * mature all immature Blocks.
 */
static void finish_block(ir_node *block, void *env)
{
	(void)env;

	mature_immBlock(block);
}

void ssa_cons_finish(ir_graph *irg)
{
	ssa_cons_walker(irg, NULL, finish_block, NULL);
	irg_finalize_cons(irg);
}
