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
 * @brief       Provides several statistic functions for the backend.
 * @author      Christian Wuerdig, Matthias Braun
 */
#include "config.h"

#include <time.h>

#include "irnode_t.h"
#include "irprintf.h"
#include "irgwalk.h"
#include "irhooks.h"
#include "execfreq.h"
#include "firmstat_t.h"
#include "irtools.h"
#include "statev.h"
#include "error.h"

#include "bearch.h"
#include "beirg.h"
#include "bestat.h"
#include "belive_t.h"
#include "besched.h"
#include "benode.h"



typedef struct pressure_walker_env_t pressure_walker_env_t;
struct pressure_walker_env_t {
	ir_graph *irg;
	be_lv_t  *lv;
	double    insn_count;
	double    regpressure;
	size_t    max_pressure;
	const arch_register_class_t *cls;
};

static void check_reg_pressure_class(pressure_walker_env_t *env,
                                     ir_node *block,
                                     const arch_register_class_t *cls)
{
	ir_graph    *irg  = env->irg;
	ir_nodeset_t live_nodes;
	size_t       max_live;

	ir_nodeset_init(&live_nodes);
	be_liveness_end_of_block(env->lv, cls, block, &live_nodes);
	max_live = ir_nodeset_size(&live_nodes);
	env->regpressure += max_live;

	sched_foreach_reverse(block, irn) {
		size_t cnt;

		if (is_Phi(irn))
			break;

		be_liveness_transfer(cls, irn, &live_nodes);
		cnt      = ir_nodeset_size(&live_nodes);
		max_live = cnt < max_live ? max_live : cnt;
		env->regpressure += cnt;
		env->insn_count++;
	}

	if (max_live > env->max_pressure)
		env->max_pressure = max_live;

	stat_be_block_regpressure(irg, block, max_live, cls->name);
	ir_nodeset_destroy(&live_nodes);
}

static void stat_reg_pressure_block(ir_node *block, void *data)
{
	pressure_walker_env_t *env = (pressure_walker_env_t*)data;

	check_reg_pressure_class(env, block, env->cls);
}

void be_do_stat_reg_pressure(ir_graph *irg, const arch_register_class_t *cls)
{
	pressure_walker_env_t  env;
	double                 average_pressure;

	env.irg          = irg;
	env.insn_count   = 0;
	env.max_pressure = 0;
	env.regpressure  = 0;
	be_assure_live_sets(irg);
	env.lv           = be_get_irg_liveness(irg);
	env.cls          = cls;

	/* Collect register pressure information for each block */
	irg_block_walk_graph(irg, stat_reg_pressure_block, NULL, &env);

	average_pressure = env.regpressure / env.insn_count;
	stat_ev_emit("bechordal_average_register_pressure", average_pressure);
	stat_ev_emit("bechordal_maximum_register_pressure", env.max_pressure);
}




typedef struct estimate_irg_costs_env_t {
	double costs;
} estimate_irg_costs_env_t;

static void estimate_block_costs(ir_node *block, void *data)
{
	estimate_irg_costs_env_t *env = (estimate_irg_costs_env_t*)data;
	double costs = 0.0;

	sched_foreach(block, node) {
		costs += arch_get_op_estimated_cost(node);
	}

	env->costs += costs * get_block_execfreq(block);
}

double be_estimate_irg_costs(ir_graph *irg)
{
	estimate_irg_costs_env_t env;
	env.costs = 0.0;

	irg_block_walk_graph(irg, estimate_block_costs, NULL, &env);

	return env.costs;
}



static void node_stat_walker(ir_node *irn, void *data)
{
	be_node_stats_t *const stats = (be_node_stats_t*)data;

	/* if the node is a normal phi */
	if (is_Phi(irn)) {
		if (get_irn_mode(irn) == mode_M) {
			(*stats)[BE_STAT_MEM_PHIS]++;
		} else {
			(*stats)[BE_STAT_PHIS]++;
		}
	} else if (be_is_Perm(irn)) {
		(*stats)[BE_STAT_PERMS]++;
	} else if (be_is_Copy(irn)) {
		(*stats)[BE_STAT_COPIES]++;
	}
}

void be_collect_node_stats(be_node_stats_t *new_stats, ir_graph *irg)
{
	memset(new_stats, 0, sizeof(*new_stats));
	irg_walk_graph(irg, NULL, node_stat_walker, new_stats);
}

void be_subtract_node_stats(be_node_stats_t *stats, be_node_stats_t *sub)
{
	int i;
	for (i = 0; i < BE_STAT_COUNT; ++i) {
		(*stats)[i] -= (*sub)[i];
	}
}

void be_copy_node_stats(be_node_stats_t *dest, be_node_stats_t *src)
{
	memcpy(dest, src, sizeof(be_node_stats_t));
}

static const char *get_stat_name(enum be_stat_tag_t tag)
{
	switch (tag) {
	case BE_STAT_PHIS:     return "phis";
	case BE_STAT_MEM_PHIS: return "mem_phis";
	case BE_STAT_COPIES:   return "copies";
	case BE_STAT_PERMS:    return "perms";
	default:               panic("unknown stat tag found");
	}
}

void be_emit_node_stats(be_node_stats_t *stats, const char *prefix)
{
	static char   buf[256];
	be_stat_tag_t i;

	for (i = BE_STAT_FIRST; i < BE_STAT_COUNT; ++i) {
		snprintf(buf, sizeof(buf), "%s%s", prefix, get_stat_name(i));
		stat_ev_dbl(buf, (*stats)[i]);
	}
}



static void insn_count_walker(ir_node *irn, void *data)
{
	unsigned long *cnt = (unsigned long*)data;

	switch (get_irn_opcode(irn)) {
	case iro_Proj:
	case iro_Phi:
	case beo_Start:
	case iro_End:
		break;
	default:
		(*cnt)++;
	}
}

unsigned long be_count_insns(ir_graph *irg)
{
	unsigned long cnt = 0;
	irg_walk_graph(irg, insn_count_walker, NULL, &cnt);
	return cnt;
}

static void block_count_walker(ir_node *node, void *data)
{
	unsigned long *cnt = (unsigned long*)data;
	if (node == get_irg_end_block(get_irn_irg(node)))
		return;
	(*cnt)++;
}

unsigned long be_count_blocks(ir_graph *irg)
{
	unsigned long cnt = 0;
	irg_block_walk_graph(irg, block_count_walker, NULL, &cnt);
	return cnt;
}
