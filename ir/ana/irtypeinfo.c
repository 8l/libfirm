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
 * @brief     Data structure to hold type information for nodes.
 * @author    Goetz Lindenmaier
 * @date      28.8.2003
 * @brief
 *  Data structure to hold type information for nodes.
 *
 *  This module defines a field "type" of type "type *" for each ir node.
 *  It defines a flag for irgraphs to mark whether the type info of the
 *  graph is valid.  Further it defines an auxiliary type "initial_type".
 *
 *  The module defines a map that contains pairs (irnode, type).  If an irnode
 *  is not in the map it is assumed to be initialized, i.e., the initialization
 *  requires no compute time.  As firm nodes can not be freed and reallocated
 *  pointers for nodes are unique (until a call of dead_node_elimination).
 */
#include "config.h"

#include "irtypeinfo.h"

#include <stddef.h>

#include "irgraph_t.h"
#include "irprog_t.h"
#include "irnode_t.h"
#include "pmap.h"

static pmap *type_node_map = NULL;


ir_type *initial_type = NULL;

void init_irtypeinfo(void)
{
	size_t i, n;

	if (initial_type == NULL)
		initial_type = new_type_class(new_id_from_str("initial_type"));

	/* We need a new, empty map. */
	if (type_node_map != NULL)
		pmap_destroy(type_node_map);
	type_node_map = pmap_create();

	for (i = 0, n = get_irp_n_irgs(); i < n; ++i)
		set_irg_typeinfo_state(get_irp_irg(i), ir_typeinfo_none);
}

void free_irtypeinfo(void)
{
	size_t i, n;

	if (initial_type != NULL) {
		free_type(initial_type);
		initial_type = NULL;
	}

	if (type_node_map != NULL) {
		pmap_destroy(type_node_map);
		type_node_map = NULL;
	}

	for (i = 0, n = get_irp_n_irgs(); i < n; ++i)
		set_irg_typeinfo_state(get_irp_irg(i), ir_typeinfo_none);
}


void set_irg_typeinfo_state(ir_graph *irg, ir_typeinfo_state s)
{
	assert(is_ir_graph(irg));
	irg->typeinfo_state = s;
	if ((irg->typeinfo_state == ir_typeinfo_consistent) &&
	    (irp->typeinfo_state == ir_typeinfo_consistent) &&
	    (s                   != ir_typeinfo_consistent)   )
		irp->typeinfo_state = ir_typeinfo_inconsistent;
}

ir_typeinfo_state get_irg_typeinfo_state(const ir_graph *irg)
{
	assert(is_ir_graph(irg));
	return irg->typeinfo_state;
}


ir_typeinfo_state get_irp_typeinfo_state(void)
{
	return irp->typeinfo_state;
}
void set_irp_typeinfo_state(ir_typeinfo_state s)
{
	irp->typeinfo_state = s;
}
void set_irp_typeinfo_inconsistent(void)
{
	if (irp->typeinfo_state == ir_typeinfo_consistent)
		irp->typeinfo_state = ir_typeinfo_inconsistent;
}


ir_type *get_irn_typeinfo_type(const ir_node *n)
{
	ir_type *res;
	assert(get_irg_typeinfo_state(get_irn_irg(n)) != ir_typeinfo_none);

	res = pmap_get(ir_type, type_node_map, n);
	if (res == NULL) {
		res = initial_type;
	}

	return res;
}

void set_irn_typeinfo_type(ir_node *n, ir_type *tp)
{
	assert(get_irg_typeinfo_state(get_irn_irg(n)) != ir_typeinfo_none);

	pmap_insert(type_node_map, (void *)n, (void *)tp);
}
