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
 * @brief   Write vcg representation of firm to file.
 * @author  Martin Trapp, Christian Schaefer, Goetz Lindenmaier, Hubert Schmidt,
 *          Matthias Braun
 */
#include "config.h"

#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <errno.h>

#include "list.h"
#include "pset.h"

#include "irnode_t.h"
#include "irgraph_t.h"
#include "irprog_t.h"
#include "entity_t.h"
#include "irop.h"

#include "irdump_t.h"
#include "irpass_t.h"

#include "irgwalk.h"
#include "tv_t.h"
#include "irouts.h"
#include "iredges.h"
#include "irdom.h"
#include "irloop_t.h"
#include "callgraph.h"
#include "irhooks.h"
#include "dbginfo_t.h"
#include "irtools.h"
#include "irprintf.h"

#include "irverify.h"

#include "error.h"
#include "array.h"
#include "pmap.h"
#include "obst.h"
#include "pset.h"
#include "util.h"

typedef struct pns_lookup {
	long       nr;      /**< the proj number */
	const char *name;   /**< the name of the Proj */
} pns_lookup_t;

typedef struct proj_lookup {
	unsigned           code;      /**< the opcode of the Proj predecessor */
	unsigned           num_data;  /**< number of data entries */
	const pns_lookup_t *data;     /**< the data */
} proj_lookup_t;

#include "gen_irdump.c.inl"

/** Dump only irgs with names that start with this prefix. */
static ident *dump_file_filter_id = NULL;

static ir_dump_flags_t flags =
	ir_dump_flag_blocks_as_subgraphs |
	ir_dump_flag_keepalive_edges |
	ir_dump_flag_ld_names |
	ir_dump_flag_back_edges |
	ir_dump_flag_consts_local |
	ir_dump_flag_analysed_types |
	ir_dump_flag_entities_in_hierarchy |
	ir_dump_flag_number_label;

static ird_color_t overrule_nodecolor = ird_color_default_node;

/** The vcg node attribute hook. */
static dump_node_vcgattr_func dump_node_vcgattr_hook = NULL;
/** The vcg edge attribute hook. */
static dump_edge_vcgattr_func dump_edge_vcgattr_hook = NULL;
/** The vcg dump block edge hook */
static dump_node_edge_func dump_block_edge_hook = NULL;
/** The vcg dump node edge hook. */
static dump_node_edge_func dump_node_edge_hook = NULL;

void set_dump_node_edge_hook(dump_node_edge_func func)
{
	dump_node_edge_hook = func;
}

dump_node_edge_func get_dump_node_edge_hook(void)
{
	return dump_node_edge_hook;
}

void set_dump_block_edge_hook(dump_node_edge_func func)
{
	dump_block_edge_hook = func;
}

dump_node_edge_func get_dump_block_edge_hook(void)
{
	return dump_node_edge_hook;
}

void set_dump_node_vcgattr_hook(dump_node_vcgattr_func hook)
{
	dump_node_vcgattr_hook = hook;
}

void set_dump_edge_vcgattr_hook(dump_edge_vcgattr_func hook)
{
	dump_edge_vcgattr_hook = hook;
}

void ir_set_dump_flags(ir_dump_flags_t new_flags)
{
	flags = new_flags;
}

void ir_add_dump_flags(ir_dump_flags_t new_flags)
{
	flags |= new_flags;
}

void ir_remove_dump_flags(ir_dump_flags_t to_remove)
{
	flags &= ~to_remove;
}

ir_dump_flags_t ir_get_dump_flags(void)
{
	return flags;
}

/** Returns 0 if dump_out_edge_flag or dump_loop_information_flag
 * are set, else returns dump_const_local_flag.
 */
static bool get_opt_dump_const_local(void)
{
	return (flags & ir_dump_flag_out_edges)
		|| (flags & ir_dump_flag_loops)
		|| (flags & ir_dump_flag_consts_local)
		|| (flags & ir_dump_flag_iredges);
}

static char *dump_filter;

void ir_set_dump_filter(const char *new_filter)
{
	xfree(dump_filter);
	dump_filter = xstrdup(new_filter);
}

const char *ir_get_dump_filter(void)
{
	return dump_filter;
}

int ir_should_dump(const char *name)
{
	const char *f, *n;

	if (dump_filter == NULL || dump_filter[0] == '\0')
		return 1;

	for (n = name, f = dump_filter; *f != '\0' && *n != '\0';
			++n, ++f) {
		if (*n != *f)
			return 0;
	}
	return 1;
}

/* -------------- some extended helper functions ----------------- */

const char *get_mode_name_ex(const ir_mode *mode, int *bad)
{
	if (is_mode(mode))
		return get_mode_name(mode);
	if (bad != NULL)
		*bad |= 1;
	return "<ERROR>";
}

#define CUSTOM_COLOR_BASE    100
static const char *color_names[ird_color_count];
static const char *color_rgb[ird_color_count];
static struct obstack color_obst;

/** define a custom color. */
static void custom_color(int num, const char *rgb_def)
{
	assert(num < ird_color_count);
	obstack_printf(&color_obst, "%d", CUSTOM_COLOR_BASE + num);
	obstack_1grow(&color_obst, '\0');

	color_rgb[num]   = rgb_def;
	color_names[num] = (const char*)obstack_finish(&color_obst);
}

/** Define a named color. */
static void named_color(int num, const char *name)
{
	assert(num < ird_color_count);
	color_rgb[num]   = NULL;
	color_names[num] = name;
}

/** Initializes the used colors. */
static void init_colors(void)
{
	static bool initialized = 0;
	if (initialized)
		return;

	obstack_init(&color_obst);

	custom_color(ird_color_prog_background,       "204 204 204");
	custom_color(ird_color_block_background,      "255 255 0");
	custom_color(ird_color_dead_block_background, "190 150 150");
	named_color(ird_color_block_inout,            "lightblue");
	named_color(ird_color_default_node,           "white");
	custom_color(ird_color_memory,                "153 153 255");
	custom_color(ird_color_controlflow,           "255 153 153");
	custom_color(ird_color_const,                 "204 255 255");
	custom_color(ird_color_proj,                  "255 255 153");
	custom_color(ird_color_uses_memory,           "153 153 255");
	custom_color(ird_color_phi,                   "105 255 105");
	custom_color(ird_color_anchor,                "100 100 255");
	named_color(ird_color_error,                  "red");
	custom_color(ird_color_entity,                "204 204 255");

	initialized = 1;
}

/**
 * Prints the VCG color to a file.
 */
static void print_vcg_color(FILE *out, ird_color_t color)
{
	assert(color < ird_color_count);
	fprintf(out, "color:%s", color_names[color]);
}

void print_nodeid(FILE *F, const ir_node *node)
{
	fprintf(F, "\"n%ld\"", get_irn_node_nr(node));
}

void print_irgid(FILE *F, const ir_graph *irg)
{
	fprintf(F, "\"g%ld\"", get_irg_graph_nr(irg));
}

void print_typeid(FILE *F, const ir_type *type)
{
	fprintf(F, "\"t%ld\"", get_type_nr(type));
}

void print_entityid(FILE *F, const ir_entity *entity)
{
	fprintf(F, "\"e%ld\"", get_entity_nr(entity));
}

static void print_itemid(FILE *F, const ir_type *tp, size_t itemnr)
{
	fprintf(F, "\"i%ldT%zu\"", get_type_nr(tp), itemnr);
}

/**
 * Prints the edge kind of a given IR node.
 *
 * Projs should be dumped near their predecessor, so they get "nearedge".
 */
static void print_node_edge_kind(FILE *out, const ir_node *node)
{
	if (is_Proj(node)) {
		fprintf(out, "nearedge: ");
	} else {
		fprintf(out, "edge: ");
	}
}

/**
 * Prints the edge from a type S to a type T with additional info fmt, ...
 * to the file F.
 */
static void print_type_type_edge(FILE *F, const ir_type *S, const ir_type *T, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(F, "edge: { sourcename: ");
	print_typeid(F, S);
	fprintf(F, " targetname: ");
	print_typeid(F, T);
	ir_vfprintf(F, fmt, ap);
	fprintf(F,"}\n");
	va_end(ap);
}

/**
 * Prints the edge from a type tp to an entity ent with additional info fmt, ...
 * to the file F.
 */
static void print_type_ent_edge(FILE *F, const ir_type *tp, const ir_entity *ent, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(F, "edge: { sourcename: ");
	print_typeid(F, tp);
	fprintf(F, " targetname: ");
	print_entityid(F, ent);
	ir_vfprintf(F, fmt, ap);
	fprintf(F, "}\n");
	va_end(ap);
}

/**
 * Prints the edge from an entity ent1 to an entity ent2 with additional info fmt, ...
 * to the file F.
 */
static void print_ent_ent_edge(FILE *F, const ir_entity *ent1, const ir_entity *ent2, int backedge, ird_color_t color, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (backedge)
		fprintf(F, "backedge: { sourcename: ");
	else
		fprintf(F, "edge: { sourcename: ");
	print_entityid(F, ent1);
	fprintf(F, " targetname: ");
	print_entityid(F, ent2);
	ir_vfprintf(F, fmt, ap);
	fprintf(F, " ");
	if (color != ird_color_none)
		print_vcg_color(F, color);
	fprintf(F, "}\n");
	va_end(ap);
}

/**
 * Prints the edge from an entity ent to a type tp with additional info fmt, ...
 * to the file F.
 */
static void print_ent_type_edge(FILE *F, const ir_entity *ent, const ir_type *tp, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(F, "edge: { sourcename: ");
	print_entityid(F, ent);
	fprintf(F, " targetname: ");
	print_typeid(F, tp);
	ir_vfprintf(F, fmt, ap);
	fprintf(F,"}\n");
	va_end(ap);
}

/**
 * Prints the edge from a node irn to a type tp with additional info fmt, ...
 * to the file F.
 */
static void print_node_type_edge(FILE *F, const ir_node *irn, ir_type *tp, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(F, "edge: { sourcename: ");
	print_nodeid(F, irn);
	fprintf(F, " targetname: ");
	print_typeid(F, tp);
	ir_vfprintf(F, fmt, ap);
	fprintf(F,"}\n");
	va_end(ap);
}

/**
 * Prints the edge from a node irn to an entity ent with additional info fmt, ...
 * to the file F.
 */
static void print_node_ent_edge(FILE *F, const ir_node *irn, const ir_entity *ent, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(F, "edge: { sourcename: ");
	print_nodeid(F, irn);
	fprintf(F, " targetname: ");
	print_entityid(F, ent);
	ir_vfprintf(F, fmt, ap);
	fprintf(F,"}\n");
	va_end(ap);
}

/**
 * Prints the edge from a type tp to an enumeration item item with additional info fmt, ...
 * to the file F.
 */
static void print_enum_item_edge(FILE *F, const ir_type *tp, size_t item, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(F, "edge: { sourcename: ");
	print_typeid(F, tp);
	fprintf(F, " targetname: ");
	print_itemid(F, tp, item);
	ir_vfprintf(F, fmt, ap);
	fprintf(F,"}\n");
	va_end(ap);
}

/*-----------------------------------------------------------------*/
/* global and ahead declarations                                   */
/*-----------------------------------------------------------------*/

static void dump_loop_nodes_into_graph(FILE *F, ir_graph *irg);

/*-----------------------------------------------------------------*/
/* Helper functions.                                                */
/*-----------------------------------------------------------------*/

/**
 * This map is used as a private link attr to be able to call dumper
 * anywhere without destroying link fields.
 */
static pmap *irdump_link_map = NULL;

/** NOT A STANDARD LIBFIRM INIT METHOD
 *
 * We do not want to integrate dumping into libfirm, i.e., if the dumpers
 * are off, we want to have as few interferences as possible.  Therefore the
 * initialization is performed lazily and not called from within init_firm.
 *
 * Creates the link attribute map. */
static void init_irdump(void)
{
	/* We need a new, empty map. */
	if (irdump_link_map) pmap_destroy(irdump_link_map);
	irdump_link_map = pmap_create();
	if (!dump_file_filter_id)
		dump_file_filter_id = new_id_from_str("");
}

/**
 * Returns the private link field.
 */
static void *ird_get_irn_link(const ir_node *n)
{
	if (irdump_link_map == NULL)
		return NULL;

	return pmap_get(void, irdump_link_map, n);
}

/**
 * Sets the private link field.
 */
static void ird_set_irn_link(const ir_node *n, void *x)
{
	if (irdump_link_map == NULL)
		init_irdump();
	pmap_insert(irdump_link_map, n, x);
}

/**
 * Gets the private link field of an irg.
 */
static void *ird_get_irg_link(const ir_graph *irg)
{
	if (irdump_link_map == NULL)
		return NULL;

	return pmap_get(void, irdump_link_map, irg);
}

/**
 * Sets the private link field of an irg.
 */
static void ird_set_irg_link(const ir_graph *irg, void *x)
{
	if (irdump_link_map == NULL)
		init_irdump();
	pmap_insert(irdump_link_map, irg, x);
}

/**
 * Walker, clears the private link field.
 */
static void clear_link(ir_node *node, void *env)
{
	(void) env;
	ird_set_irn_link(node, NULL);
}

/**
 * If the entity has a ld_name, returns it if the dump_ld_name is set,
 * else returns the name of the entity.
 */
static const char *get_ent_dump_name_(const ir_entity *ent, bool dump_ld_name)
{
	if (ent == NULL)
		return "<NULL entity>";
	if (dump_ld_name) {
		/* Don't use get_entity_ld_ident (ent) as it computes the mangled name! */
		if (ent->ld_name != NULL)
			return get_id_str(ent->ld_name);
	}
	return get_id_str(ent->name);
}

const char *get_ent_dump_name(const ir_entity *ent)
{
	return get_ent_dump_name_(ent, flags & ir_dump_flag_ld_names);
}

const char *get_irg_dump_name(const ir_graph *irg)
{
	/* Don't use get_entity_ld_ident (ent) as it computes the mangled name! */
	return get_ent_dump_name_(get_irg_entity(irg), true);
}

/**
 * Returns non-zero if a node is in floating state.
 */
static int node_floats(const ir_node *n)
{
	ir_graph *irg = get_irn_irg(n);
	return ((get_irn_pinned(n) == op_pin_state_floats) &&
	        (get_irg_pinned(irg) == op_pin_state_floats));
}

/**
 *  Walker that visits the anchors
 */
static void ird_walk_graph(ir_graph *irg, irg_walk_func *pre, irg_walk_func *post, void *env)
{
	if ((flags & ir_dump_flag_all_anchors)
			|| ((flags & ir_dump_flag_iredges) && edges_activated(irg))) {
		irg_walk_anchors(irg, pre, post, env);
	} else {
		irg_walk_graph(irg, pre, post, env);
	}
}

/**
 * Walker, allocates an array for all blocks and puts their non-floating
 * nodes into this array.
 */
static void collect_node(ir_node *node, void *env)
{
	(void) env;
	if (is_Block(node)
	    || node_floats(node)
	    || (get_op_flags(get_irn_op(node)) & irop_flag_dump_noblock)) {
		ir_node ** arr = (ir_node **) ird_get_irg_link(get_irn_irg(node));
		if (!arr) arr = NEW_ARR_F(ir_node *, 0);
		ARR_APP1(ir_node *, arr, node);
		ird_set_irg_link(get_irn_irg(node), arr);    /* arr is an l-value, APP_ARR might change it! */
	} else {
		ir_node * block = get_nodes_block(node);

		if (is_Bad(block)) {
			/* this node is in a Bad block, so we must place it into the graph's list */
			ir_node ** arr = (ir_node **) ird_get_irg_link(get_irn_irg(node));
			if (!arr) arr = NEW_ARR_F(ir_node *, 0);
			ARR_APP1(ir_node *, arr, node);
			ird_set_irg_link(get_irn_irg(node), arr);    /* arr is an l-value, APP_ARR might change it! */
		} else {
			ird_set_irn_link(node, ird_get_irn_link(block));
			ird_set_irn_link(block, node);
		}
	}
}

/** Construct lists to walk ir block-wise.
 *
 * Collects all blocks, nodes not op_pin_state_pinned,
 * Bad, NoMem and Unknown into a flexible array in link field of
 * irg they belong to.  Sets the irg link field to NULL in all
 * graphs not visited.
 * Free the list with DEL_ARR_F().
 */
static ir_node **construct_block_lists(ir_graph *irg)
{
	size_t   i;
	int      walk_flag = ir_resources_reserved(irg) & IR_RESOURCE_IRN_VISITED;

	if (walk_flag) {
		ir_free_resources(irg, IR_RESOURCE_IRN_VISITED);
	}

	for (i = get_irp_n_irgs(); i > 0;)
		ird_set_irg_link(get_irp_irg(--i), NULL);

	ird_walk_graph(irg, clear_link, collect_node, irg);

	if (walk_flag) {
		ir_reserve_resources(irg, IR_RESOURCE_IRN_VISITED);
	}

	return (ir_node**)ird_get_irg_link(irg);
}

void dump_node_opcode(FILE *F, const ir_node *n)
{
	const ir_op_ops *ops = get_op_ops(get_irn_op(n));

	/* call the dump_node operation if available */
	if (ops->dump_node) {
		ops->dump_node(F, n, dump_node_opcode_txt);
		return;
	}

	/* implementation for default nodes */
	switch (get_irn_opcode(n)) {
	case iro_SymConst:
		switch (get_SymConst_kind(n)) {
		case symconst_addr_ent:
			fprintf(F, "SymC &%s", get_entity_name(get_SymConst_entity(n)));
			break;
		case symconst_ofs_ent:
			fprintf(F, "SymC %s offset", get_entity_name(get_SymConst_entity(n)));
			break;
		case symconst_type_size:
			ir_fprintf(F, "SymC %+F size", get_SymConst_type(n));
			break;
		case symconst_type_align:
			ir_fprintf(F, "SymC %+F align", get_SymConst_type(n));
			break;
		case symconst_enum_const:
			fprintf(F, "SymC %s enum", get_enumeration_const_name(get_SymConst_enum(n)));
			break;
		}
		break;

	case iro_Load:
		if (get_Load_unaligned(n) == align_non_aligned)
			fprintf(F, "ua");
		fprintf(F, "%s[%s]", get_irn_opname(n), get_mode_name_ex(get_Load_mode(n), NULL));
		break;
	case iro_Store:
		if (get_Store_unaligned(n) == align_non_aligned)
			fprintf(F, "ua");
		fprintf(F, "%s", get_irn_opname(n));
		break;
	case iro_Block:
		if (n == get_irg_start_block(get_irn_irg(n)))
			fputs("Start ", F);
		if (n == get_irg_end_block(get_irn_irg(n)))
			fputs("End ", F);
		fprintf(F, "%s%s", get_irn_opname(n),
			(flags & ir_dump_flag_show_marks) ? (get_Block_mark(n) ? "*" : "") : "");
		break;
	case iro_Div:
		fprintf(F, "%s", get_irn_opname(n));
		if (get_Div_no_remainder(n))
			fprintf(F, "RL");
		fprintf(F, "[%s]", get_mode_name_ex(get_Div_resmode(n), NULL));
		break;
	case iro_Mod:
		fprintf(F, "%s[%s]", get_irn_opname(n), get_mode_name_ex(get_Mod_resmode(n), NULL));
		break;
	case iro_Builtin:
		fprintf(F, "%s[%s]", get_irn_opname(n), get_builtin_kind_name(get_Builtin_kind(n)));
		break;

	default:
		fprintf(F, "%s", get_irn_opname(n));
	}
}

/**
 * Dump the mode of a node n to a file F.
 * Ignore modes that are "always known".
 */
static void dump_node_mode(FILE *F, const ir_node *n)
{
	const ir_op_ops *ops = get_op_ops(get_irn_op(n));
	unsigned         iro;
	ir_mode         *mode;

	/* call the dump_node operation if available */
	if (ops->dump_node) {
		ops->dump_node(F, n, dump_node_mode_txt);
		return;
	}

	/* default implementation */
	iro = get_irn_opcode(n);
	switch (iro) {
	case iro_SymConst:
	case iro_Sel:
	case iro_End:
	case iro_Return:
	case iro_Free:
	case iro_Sync:
	case iro_Jmp:
	case iro_NoMem:
		break;
	default:
		mode = get_irn_mode(n);

		if (mode != NULL && mode != mode_BB && mode != mode_ANY && mode != mode_BAD &&
			(mode != mode_T || iro == iro_Proj))
			fprintf(F, "%s", get_mode_name_ex(mode, NULL));
	}
}

/**
 * Dump the type of a node n to a file F if it's known.
 */
static int dump_node_typeinfo(FILE *F, const ir_node *n)
{
	ir_graph *irg = get_irn_irg(n);
	int bad = 0;

	if (ir_get_dump_flags() & ir_dump_flag_analysed_types) {
		if (get_irg_typeinfo_state(irg) == ir_typeinfo_consistent  ||
			get_irg_typeinfo_state(irg) == ir_typeinfo_inconsistent) {
			ir_type *tp = get_irn_typeinfo_type(n);
			if (tp != get_none_type()) {
				ir_fprintf(F, "[%+F]", tp);
			} else {
				fprintf(F, "[] ");
			}
		}
	}
	return bad;
}

/**
 * Dump additional node attributes of some nodes to a file F.
 */
static void dump_node_nodeattr(FILE *F, const ir_node *n)
{
	ir_node *pred;
	unsigned code;
	long proj_nr;
	const ir_op_ops *ops = get_op_ops(get_irn_op(n));

	/* call the dump_node operation if available */
	if (ops->dump_node) {
		ops->dump_node(F, n, dump_node_nodeattr_txt);
		return;
	}

	switch (get_irn_opcode(n)) {
	case iro_Const:
		ir_fprintf(F, "%T ", get_Const_tarval(n));
		break;

	case iro_Proj:
		pred    = get_Proj_pred(n);
		proj_nr = get_Proj_proj(n);
		code    = get_irn_opcode(pred);

		if (code == iro_Proj && get_irn_opcode(get_Proj_pred(pred)) == iro_Start)
			fprintf(F, "Arg %ld ", proj_nr);
		else if (code == iro_Cond && get_irn_mode(get_Cond_selector(pred)) != mode_b)
			fprintf(F, "%ld ", proj_nr);
		else {
			unsigned i, j, f = 0;

			for (i = 0; i < ARRAY_SIZE(proj_lut); ++i) {
				if (code == proj_lut[i].code) {
					for (j = 0; j < proj_lut[i].num_data; ++j) {
						if (proj_nr == proj_lut[i].data[j].nr) {
							fprintf(F, "%s ", proj_lut[i].data[j].name);
							f = 1;
							break;
						}
					}
					break;
				}
			}
			if (! f)
				fprintf(F, "%ld ", proj_nr);
			if (code == iro_Cond && get_Cond_jmp_pred(pred) != COND_JMP_PRED_NONE) {
				if (proj_nr == pn_Cond_false && get_Cond_jmp_pred(pred) == COND_JMP_PRED_FALSE)
					fprintf(F, "PRED ");
				if (proj_nr == pn_Cond_true && get_Cond_jmp_pred(pred) == COND_JMP_PRED_TRUE)
					fprintf(F, "PRED ");
			}
		}
		break;
	case iro_Sel:
		fprintf(F, "%s ", get_ent_dump_name(get_Sel_entity(n)));
		break;
	case iro_Cast:
		ir_fprintf(F, "(%+F)", get_Cast_type(n));
		break;
	case iro_Cmp:
		fprintf(F, "%s ", get_relation_string(get_Cmp_relation(n)));
		break;
	case iro_Confirm:
		fprintf(F, "%s ", get_relation_string(get_Confirm_relation(n)));
		break;
	case iro_CopyB:
		ir_fprintf(F, "(%+F)", get_CopyB_type(n));
		break;

	default:
		break;
	}
}

void dump_node_label(FILE *F, const ir_node *n)
{
	dump_node_opcode(F, n);
	fputs(" ", F);
	dump_node_mode(F, n);
	fprintf(F, " ");
	dump_node_typeinfo(F, n);
	dump_node_nodeattr(F, n);
	if (flags & ir_dump_flag_number_label) {
		fprintf(F, "%ld", get_irn_node_nr(n));
	}
	if (flags & ir_dump_flag_idx_label) {
		fprintf(F, ":%u", get_irn_idx(n));
	}
}

/**
 * Dumps the attributes of a node n into the file F.
 * Currently this is only the color of a node.
 */
static void dump_node_vcgattr(FILE *F, const ir_node *node, const ir_node *local, bool bad)
{
	ir_mode *mode;
	const ir_node *n;

	if (bad) {
		print_vcg_color(F, ird_color_error);
		return;
	}

	if (dump_node_vcgattr_hook != NULL) {
		dump_node_vcgattr_hook(F, node, local);
		return;
	}

	n = local ? local : node;

	if (overrule_nodecolor != ird_color_default_node) {
		print_vcg_color(F, overrule_nodecolor);
		return;
	}

	mode = get_irn_mode(n);
	if (mode == mode_M) {
		print_vcg_color(F, ird_color_memory);
		return;
	}
	if (mode == mode_X) {
		print_vcg_color(F, ird_color_controlflow);
		return;
	}

	switch (get_irn_opcode(n)) {
	case iro_Start:
	case iro_End:
		print_vcg_color(F, ird_color_anchor);
		break;
	case iro_Bad:
		print_vcg_color(F, ird_color_error);
		break;
	case iro_Block:
		print_vcg_color(F, ird_color_block_background);
		break;
	case iro_Phi:
		print_vcg_color(F, ird_color_phi);
		break;
	case iro_Pin:
		print_vcg_color(F, ird_color_memory);
		break;
	case iro_SymConst:
	case iro_Const:
		print_vcg_color(F, ird_color_const);
		break;
	case iro_Proj:
		print_vcg_color(F, ird_color_proj);
		break;
	default: {
		ir_op *op = get_irn_op(node);

		if (is_op_constlike(op)) {
			print_vcg_color(F, ird_color_const);
		} else if (is_op_uses_memory(op)) {
			print_vcg_color(F, ird_color_uses_memory);
		} else if (is_op_cfopcode(op) || is_op_forking(op)) {
			print_vcg_color(F, ird_color_controlflow);
		}
	}
	}
}

void *dump_add_node_info_callback(dump_node_info_cb_t *cb, void *data)
{
	hook_entry_t *info = XMALLOCZ(hook_entry_t);

	info->hook._hook_node_info = cb;
	info->context              = data;
	register_hook(hook_node_info, info);

	return info;
}

void dump_remove_node_info_callback(void *handle)
{
	hook_entry_t *info = (hook_entry_t*)handle;
	unregister_hook(hook_node_info, info);
	xfree(info);
}

/**
 * Dump the node information of a node n to a file F.
 */
static void dump_node_info(FILE *F, const ir_node *n)
{
	const ir_op_ops *ops = get_op_ops(get_irn_op(n));

	fprintf(F, " info1: \"");
	dump_irnode_to_file(F, n);
	/* call the dump_node operation if available */
	if (ops->dump_node)
		ops->dump_node(F, n, dump_node_info_txt);

	/* allow additional info to be added */
	hook_node_info(F, n);
	fprintf(F, "\"\n");
}

static int is_constlike_node(const ir_node *node)
{
	const ir_op *op = get_irn_op(node);
	return is_op_constlike(op);
}

static void print_constid(FILE *F, const ir_node *user, const ir_node *node)
{
	fprintf(F, "\"n%ldb%ld\"", get_irn_node_nr(user), get_irn_node_nr(node));
}

static void print_constblkid(FILE *F, const ir_node *node, const ir_node *block)
{
	fprintf(F, "\"n%ldb%ld\"", get_irn_node_nr(node), get_irn_node_nr(block));
}


/** outputs the predecessors of n, that are constants, local.  I.e.,
   generates a copy of the constant predecessors for each node called with. */
static void dump_const_node_local(FILE *F, const ir_node *n)
{
	ir_graph *irg = get_irn_irg(n);
	int i;
	if (!get_opt_dump_const_local()) return;

	/* Use visited flag to avoid outputting nodes twice.
	initialize it first. */
	for (i = 0; i < get_irn_arity(n); i++) {
		ir_node *con = get_irn_n(n, i);
		if (is_constlike_node(con)) {
			set_irn_visited(con, get_irg_visited(irg) - 1);
		}
	}

	for (i = 0; i < get_irn_arity(n); i++) {
		ir_node *con = get_irn_n(n, i);
		if (is_constlike_node(con) && !irn_visited_else_mark(con)) {
			/* Generate a new name for the node by appending the names of
			n and const. */
			fprintf(F, "node: {title: ");
			print_constid(F, n, con);
			fprintf(F, " label: \"");
			dump_node_label(F, con);
			fprintf(F, "\" ");
			dump_node_info(F, con);
			dump_node_vcgattr(F, n, con, 0);
			fprintf(F, "}\n");
		}
	}
}

/** If the block of an edge is a const_like node, dump it local with an edge */
static void dump_const_block_local(FILE *F, const ir_node *n)
{
	ir_node *blk;

	if (!get_opt_dump_const_local()) return;

	blk = get_nodes_block(n);
	if (is_constlike_node(blk)) {
		/* Generate a new name for the node by appending the names of
		n and blk. */
		fprintf(F, "node: {title: ");
		print_constblkid(F, n, blk);
		fprintf(F, " label: \"");
		dump_node_label(F, blk);
		fprintf(F, "\" ");
		dump_node_info(F, blk);
		dump_node_vcgattr(F, n, blk, 0);
		fprintf(F, "}\n");

		fprintf(F, "edge: { sourcename: ");
		print_nodeid(F, n);
		fprintf(F, " targetname: ");
		print_constblkid(F, n, blk);
		fprintf(F, " ");

		if (dump_edge_vcgattr_hook != NULL
		    && dump_edge_vcgattr_hook(F, n, -1)) {
			fprintf(F, "}\n");
			return;
		}

		fprintf(F, BLOCK_EDGE_ATTR "}\n");
	}
}

/**
 * prints the error message of a node to a file F as info2.
 */
static void print_node_error(FILE *F, const char *err_msg)
{
	if (! err_msg)
		return;

	fprintf(F, " info2: \"%s\"", err_msg);
}

/**
 * prints debug messages of a node to file F as info3.
 */
static void print_dbg_info(FILE *F, dbg_info *dbg)
{
	char buf[1024];

	ir_dbg_info_snprint(buf, sizeof(buf), dbg);
	if (buf[0] != 0) {
		fprintf(F, " info3: \"%s\"\n", buf);
	}
}

static void print_type_dbg_info(FILE *F, type_dbg_info *dbg)
{
	(void) F;
	(void) dbg;
	/* TODO */
}

/**
 * Dump a node
 */
void dump_node(FILE *F, const ir_node *n)
{
	int bad = 0;
	const char *p;
	ir_graph   *irg;

	if (get_opt_dump_const_local() && is_constlike_node(n))
		return;

	/* dump this node */
	fputs("node: {title: ", F);
	print_nodeid(F, n);

	fputs(" label: \"", F);
	irg = get_irn_irg(n);
	bad = ! irn_verify_irg_dump(n, irg, &p);
	dump_node_label(F, n);
	fputs("\" ", F);

	dump_node_info(F, n);
	print_node_error(F, p);
	print_dbg_info(F, get_irn_dbg_info(n));
	dump_node_vcgattr(F, n, NULL, bad);
	fputs("}\n", F);
	dump_const_node_local(F, n);

}

/** dump the edge to the block this node belongs to */
static void dump_ir_block_edge(FILE *F, const ir_node *n)
{
	if (get_opt_dump_const_local() && is_constlike_node(n)) return;
	if (!is_Block(n)) {
		ir_node *block = get_nodes_block(n);

		if (get_opt_dump_const_local() && is_constlike_node(block)) {
			dump_const_block_local(F, n);
		} else {
			fprintf(F, "edge: { sourcename: ");
			print_nodeid(F, n);
			fprintf(F, " targetname: ");
			print_nodeid(F, block);
			fprintf(F, " ");

			if (dump_edge_vcgattr_hook && dump_edge_vcgattr_hook(F, n, -1)) {
				fprintf(F, "}\n");
				return;
			}

			fprintf(F, BLOCK_EDGE_ATTR "}\n");
		}
	}
}

static void print_data_edge_vcgattr(FILE *F, const ir_node *from, int to)
{
	/*
	 * do not use get_nodes_block() here, will fail
	 * if the irg is not pinned.
	 */
	if (get_nodes_block(from) == get_nodes_block(get_irn_n(from, to)))
		fprintf(F, INTRA_DATA_EDGE_ATTR);
	else
		fprintf(F, INTER_DATA_EDGE_ATTR);
}

static void print_mem_edge_vcgattr(FILE *F, const ir_node *from, int to)
{
	/*
	 * do not use get_nodes_block() here, will fail
	 * if the irg is not pinned.
	 */
	if (get_nodes_block(from) == get_nodes_block(get_irn_n(from, to)))
		fprintf(F, INTRA_MEM_EDGE_ATTR);
	else
		fprintf(F, INTER_MEM_EDGE_ATTR);
}

/** Print the vcg attributes for the edge from node "from" to its "to"th input */
static void print_edge_vcgattr(FILE *F, const ir_node *from, int to)
{
	assert(from);

	if (dump_edge_vcgattr_hook)
		if (dump_edge_vcgattr_hook(F, from, to))
			return;

	if ((flags & ir_dump_flag_back_edges) && is_backedge(from, to))
		fprintf(F, BACK_EDGE_ATTR);

	switch (get_irn_opcode(from)) {
	case iro_Block:
		fprintf(F, CF_EDGE_ATTR);
		break;
	case iro_Start:  break;
	case iro_End:
		if (to >= 0) {
			if (get_irn_mode(get_End_keepalive(from, to)) == mode_BB)
				fprintf(F, KEEP_ALIVE_CF_EDGE_ATTR);
			else
				fprintf(F, KEEP_ALIVE_DF_EDGE_ATTR);
		}
		break;
	default:
		if (is_Proj(from)) {
			if (get_irn_mode(from) == mode_M)
				print_mem_edge_vcgattr(F, from, to);
			else if (get_irn_mode(from) == mode_X)
				fprintf(F, CF_EDGE_ATTR);
			else
				print_data_edge_vcgattr(F, from, to);
		}
		else if (get_irn_mode(get_irn_n(from, to)) == mode_M)
			print_mem_edge_vcgattr(F, from, to);
		else if (get_irn_mode(get_irn_n(from, to)) == mode_X)
			fprintf(F, CF_EDGE_ATTR);
		else
			print_data_edge_vcgattr(F, from, to);
	}
}

/** dump edges to our inputs */
static void dump_ir_data_edges(FILE *F, const ir_node *n)
{
	int i, num;

	if (dump_node_edge_hook)
		dump_node_edge_hook(F, n);

	if (!(flags & ir_dump_flag_keepalive_edges) && is_End(n)) {
		/* the End node has only keep-alive edges */
		return;
	}

	/* dump the dependency edges. */
	num = get_irn_deps(n);
	for (i = 0; i < num; ++i) {
		ir_node *dep = get_irn_dep(n, i);

		if (dep) {
			print_node_edge_kind(F, n);
			fprintf(F, "{sourcename: ");
			print_nodeid(F, n);
			fprintf(F, " targetname: ");
			if ((get_opt_dump_const_local()) && is_constlike_node(dep)) {
				print_constid(F, n, dep);
			} else {
				print_nodeid(F, dep);
			}
			fprintf(F, " label: \"%d\" ", i);
			fprintf(F, " color: darkgreen}\n");
		}
	}

	num = get_irn_arity(n);
	for (i = 0; i < num; i++) {
		ir_node *pred = get_irn_n(n, i);
		assert(pred);

		if ((flags & ir_dump_flag_back_edges) && is_backedge(n, i)) {
			fprintf(F, "backedge: {sourcename: ");
		} else {
			print_node_edge_kind(F, n);
			fprintf(F, "{sourcename: ");
		}
		print_nodeid(F, n);
		fprintf(F, " targetname: ");
		if ((get_opt_dump_const_local()) && is_constlike_node(pred)) {
			print_constid(F, n, pred);
		} else {
			print_nodeid(F, pred);
		}
		fprintf(F, " label: \"%d\" ", i);
		print_edge_vcgattr(F, n, i);
		fprintf(F, "}\n");
	}
}

/**
 * Dump the ir_edges
 */
static void dump_ir_edges(ir_node *node, void *env)
{
	int   i = 0;
	FILE *F = (FILE*)env;

	foreach_out_edge(node, edge) {
		ir_node *succ = get_edge_src_irn(edge);

		print_node_edge_kind(F, succ);
		fprintf(F, "{sourcename: ");
		print_nodeid(F, node);
		fprintf(F, " targetname: ");
		print_nodeid(F, succ);

		fprintf(F, " label: \"%d\" ", i);
		fprintf(F, OUT_EDGE_ATTR);
		fprintf(F, "}\n");
		++i;
	}
}


/** Dumps a node and its edges but not the block edge  */
static void dump_node_wo_blockedge(FILE *F, const ir_node *n)
{
	dump_node(F, n);
	dump_ir_data_edges(F, n);
}

/** Dumps a node and its edges. */
static void dump_node_with_edges(ir_node *n, void *env)
{
	FILE *F = (FILE*)env;
	dump_node_wo_blockedge(F, n);
	if (!node_floats(n))
		dump_ir_block_edge(F, n);
}

/** Dumps a const-like node. */
static void dump_const_node(ir_node *n, void *env)
{
	FILE *F = (FILE*)env;
	if (is_Block(n)) return;
	dump_node_wo_blockedge(F, n);
}

/***********************************************************************/
/* the following routines dump the nodes/irgs bracketed to graphs.     */
/***********************************************************************/

/** Dumps a constant expression as entity initializer, array bound ... */
static void dump_const_expression(FILE *F, ir_node *value)
{
	ir_graph *irg = get_const_code_irg();
	ir_dump_flags_t old_flags = ir_get_dump_flags();
	ir_remove_dump_flags(ir_dump_flag_consts_local);

	irg_walk(value, dump_const_node, NULL, F);
	/* Decrease visited flag so that we walk with the same flag for the next
	   expression.  This guarantees that we don't dump the same node twice,
	   as for const expressions cse is performed to save memory. */
	set_irg_visited(irg, get_irg_visited(irg) -1);

	ir_set_dump_flags(old_flags);
}

/** Dump a block as graph containing its nodes.
 *
 *  Expects to find nodes belonging to the block as list in its
 *  link field.
 *  Dumps the edges of all nodes including itself. */
static void dump_whole_block(FILE *F, const ir_node *block)
{
	ir_node *node;

	assert(is_Block(block));

	fprintf(F, "graph: { title: ");
	print_nodeid(F, block);
	fprintf(F, " label: \"");
	dump_node_label(F, block);

	fprintf(F, "\" status:clustered ");
	/* colorize blocks */
	ird_color_t const color =
		!get_Block_matured(block) ? ird_color_error :
		ird_color_block_background;
	print_vcg_color(F, color);
	fprintf(F, "\n");

	/* yComp can show attributes for blocks, XVCG parses but ignores them */
	dump_node_info(F, block);
	print_dbg_info(F, get_irn_dbg_info(block));

	/* dump the blocks edges */
	dump_ir_data_edges(F, block);

	if (dump_block_edge_hook)
		dump_block_edge_hook(F, block);

	/* dump the nodes that go into the block */
	for (node = (ir_node*)ird_get_irn_link(block); node; node = (ir_node*)ird_get_irn_link(node)) {
		dump_node(F, node);
		dump_ir_data_edges(F, node);
	}

	/* Close the vcg information for the block */
	fprintf(F, "}\n");
	dump_const_node_local(F, block);
	fprintf(F, "\n");
}

/** dumps a graph block-wise. Expects all blockless nodes in arr in irgs link.
 *  The outermost nodes: blocks and nodes not op_pin_state_pinned, Bad, Unknown. */
static void dump_block_graph(FILE *F, ir_graph *irg)
{
	size_t i, n;
	ir_node **arr = (ir_node**)ird_get_irg_link(irg);

	for (i = 0, n = ARR_LEN(arr); i < n; ++i) {
		ir_node *node = arr[i];
		if (is_Block(node)) {
		/* Dumps the block and all the nodes in the block, which are to
			be found in Block->link. */
			dump_whole_block(F, node);
		} else {
			/* Nodes that are not in a Block. */
			dump_node(F, node);
			if (!node_floats(node) && is_Bad(get_nodes_block(node))) {
				dump_const_block_local(F, node);
			}
			dump_ir_data_edges(F, node);
		}
		if ((flags & ir_dump_flag_iredges) && edges_activated(irg))
			dump_ir_edges(node, F);
	}

	if ((flags & ir_dump_flag_loops)
	     && irg_has_properties(irg, IR_GRAPH_PROPERTY_CONSISTENT_LOOPINFO))
		dump_loop_nodes_into_graph(F, irg);
}

/**
 * Dump the info for an irg.
 * Parsed by XVCG but not shown. use yComp.
 */
static void dump_graph_info(FILE *F, ir_graph *irg)
{
	fprintf(F, "info1: \"");
	dump_entity_to_file(F, get_irg_entity(irg));
	fprintf(F, "\n");

	/* dump graph state */
	fprintf(F, "constraints:");
	if (irg_is_constrained(irg, IR_GRAPH_CONSTRAINT_ARCH_DEP))
		fprintf(F, " arch_dep");
	if (irg_is_constrained(irg, IR_GRAPH_CONSTRAINT_MODEB_LOWERED))
		fprintf(F, " modeb_lowered");
	if (irg_is_constrained(irg, IR_GRAPH_CONSTRAINT_NORMALISATION2))
		fprintf(F, " normalisation2");
	if (irg_is_constrained(irg, IR_GRAPH_CONSTRAINT_OPTIMIZE_UNREACHABLE_CODE))
		fprintf(F, " optimize_unreachable_code");
	fprintf(F, "\n");

	fprintf(F, "properties:");
	if (irg_has_properties(irg, IR_GRAPH_PROPERTY_NO_CRITICAL_EDGES))
		fprintf(F, " no_critical_edges");
	if (irg_has_properties(irg, IR_GRAPH_PROPERTY_NO_BADS))
		fprintf(F, " no_bads");
	if (irg_has_properties(irg, IR_GRAPH_PROPERTY_NO_UNREACHABLE_CODE))
		fprintf(F, " no_unreachable_code");
	if (irg_has_properties(irg, IR_GRAPH_PROPERTY_ONE_RETURN))
		fprintf(F, " one_return");
	if (irg_has_properties(irg, IR_GRAPH_PROPERTY_CONSISTENT_DOMINANCE))
		fprintf(F, " consistent_dominance");
	if (irg_has_properties(irg, IR_GRAPH_PROPERTY_CONSISTENT_POSTDOMINANCE))
		fprintf(F, " consistent_postdominance");
	if (irg_has_properties(irg, IR_GRAPH_PROPERTY_CONSISTENT_OUT_EDGES))
		fprintf(F, " consistent_out_edges");
	if (irg_has_properties(irg, IR_GRAPH_PROPERTY_CONSISTENT_OUTS))
		fprintf(F, " consistent_outs");
	if (irg_has_properties(irg, IR_GRAPH_PROPERTY_CONSISTENT_LOOPINFO))
		fprintf(F, " consistent_loopinfo");
	if (irg_has_properties(irg, IR_GRAPH_PROPERTY_CONSISTENT_ENTITY_USAGE))
		fprintf(F, " consistent_entity_usage");
	fprintf(F, "\"\n");
}

/** Dumps an irg as a graph clustered by block nodes.
 *  If interprocedural view edges can point to nodes out of this graph.
 */
static void dump_graph_from_list(FILE *F, ir_graph *irg)
{
	ir_entity *ent = get_irg_entity(irg);

	fprintf(F, "graph: { title: ");
	print_irgid(F, irg);
	fprintf(F, " label: \"%s\" status:clustered color:%s\n",
	  get_ent_dump_name(ent), color_names[ird_color_prog_background]);

	dump_graph_info(F, irg);
	print_dbg_info(F, get_entity_dbg_info(ent));

	dump_block_graph(F, irg);

	/* Close the vcg information for the irg */
	fprintf(F, "}\n\n");
}

/*******************************************************************/
/* Basic type and entity nodes and edges.                          */
/*******************************************************************/

/** dumps the edges between nodes and their type or entity attributes. */
static void dump_node2type_edges(ir_node *n, void *env)
{
	FILE *F = (FILE*)env;
	assert(n);

	switch (get_irn_opcode(n)) {
	case iro_Const :
		/* @@@ some consts have an entity */
		break;
	case iro_SymConst:
		if (SYMCONST_HAS_TYPE(get_SymConst_kind(n)))
			print_node_type_edge(F,n,get_SymConst_type(n),NODE2TYPE_EDGE_ATTR);
		break;
	case iro_Sel:
		print_node_ent_edge(F,n,get_Sel_entity(n),NODE2TYPE_EDGE_ATTR);
		break;
	case iro_Call:
		print_node_type_edge(F,n,get_Call_type(n),NODE2TYPE_EDGE_ATTR);
		break;
	case iro_Alloc:
		print_node_type_edge(F,n,get_Alloc_type(n),NODE2TYPE_EDGE_ATTR);
		break;
	case iro_Free:
		print_node_type_edge(F,n,get_Free_type(n),NODE2TYPE_EDGE_ATTR);
		break;
	case iro_Cast:
		print_node_type_edge(F,n,get_Cast_type(n),NODE2TYPE_EDGE_ATTR);
		break;
	default:
		break;
	}
}

static void print_typespecific_vcgattr(FILE *F, ir_type *tp)
{
	switch (get_type_tpop_code(tp)) {
	case tpo_class:
		fprintf(F, " " TYPE_CLASS_NODE_ATTR);
		break;
	case tpo_struct:
		fprintf(F, " " TYPE_METH_NODE_ATTR);
		break;
	case tpo_method:
		break;
	case tpo_union:
		break;
	case tpo_array:
		break;
	case tpo_enumeration:
		break;
	case tpo_pointer:
		break;
	case tpo_primitive:
		break;
	default:
		break;
	}
}

void dump_type_node(FILE *F, ir_type *tp)
{
	fprintf(F, "node: {title: ");
	print_typeid(F, tp);
	fprintf(F, " label: \"");
	if (tp->dbi != NULL) {
		char buf[1024];
		ir_print_type(buf, sizeof(buf), tp);
		fprintf(F, "%s '%s'", get_type_tpop_name(tp), buf);
	} else {
		ir_fprintf(F, "%+F", tp);
	}
	fputs("\" info1: \"", F);
	dump_type_to_file(F, tp);
	fprintf(F, "\"\n");
	print_type_dbg_info(F, get_type_dbg_info(tp));
	print_typespecific_vcgattr(F, tp);
	fprintf(F, "}\n");
}

static void dump_entity_node(FILE *F, ir_entity *ent)
{
	fprintf(F, "node: {title: ");
	print_entityid(F, ent);
	fprintf(F, " label: ");
	fprintf(F, "\"%s\" ", get_ent_dump_name(ent));

	print_vcg_color(F, ird_color_entity);
	fprintf(F, "\n info1: \"");

	dump_entity_to_file(F, ent);

	fprintf(F, "\"\n");
	print_dbg_info(F, get_entity_dbg_info(ent));
	fprintf(F, "}\n");
}

static void dump_enum_item(FILE *F, ir_type *tp, size_t pos)
{
	char buf[1024];
	ir_enum_const *ec = get_enumeration_const(tp, pos);
	ident         *id = get_enumeration_const_nameid(ec);
	ir_tarval     *tv = get_enumeration_value(ec);

	if (tv)
		tarval_snprintf(buf, sizeof(buf), tv);
	else
		strncpy(buf, "<not set>", sizeof(buf));
	fprintf(F, "node: {title: ");
	print_itemid(F, tp, pos);
	fprintf(F, " label: ");
	fprintf(F, "\"enum item %s\" " ENUM_ITEM_NODE_ATTR, get_id_str(id));
	fprintf(F, "\n info1: \"value: %s\"}\n", buf);
}

/**
 * Dumps a new style initializer.
 */
static void dump_entity_initializer(FILE *F, const ir_entity *ent)
{
	/* TODO */
	(void) F;
	(void) ent;
}

/**
 * type-walker: Dumps a type or entity and its edges.
 */
static void dump_type_info(type_or_ent tore, void *env)
{
	FILE *F = (FILE*)env;
	size_t i = 0;  /* to shutup gcc */

	/* dump this type or entity */

	switch (get_kind(tore.ent)) {
	case k_entity: {
		ir_entity *ent = tore.ent;
		/* The node */
		dump_entity_node(F, ent);
		/* The Edges */
		/* skip this to reduce graph.  Member edge of type is parallel to this edge. *
		fprintf(F, "edge: { sourcename: \"%p\" targetname: \"%p\" "
		ENT_OWN_EDGE_ATTR "}\n", ent, get_entity_owner(ent));*/
		print_ent_type_edge(F,ent, get_entity_type(ent), ENT_TYPE_EDGE_ATTR);
		if (is_Class_type(get_entity_owner(ent))) {
			for (i = get_entity_n_overwrites(ent); i > 0;)
				print_ent_ent_edge(F, ent, get_entity_overwrites(ent, --i), 0, ird_color_none, ENT_OVERWRITES_EDGE_ATTR);
		}
		/* attached subgraphs */
		if (! (flags & ir_dump_flag_no_entity_values)) {
			if (ent->initializer != NULL) {
				/* new style initializers */
				dump_entity_initializer(F, ent);
			}
		}
		break;
	}
	case k_type: {
		ir_type *tp = tore.typ;
		dump_type_node(F, tp);
		/* and now the edges */
		switch (get_type_tpop_code(tp)) {
		case tpo_class:
			for (i = get_class_n_supertypes(tp); i > 0;) {
				--i;
				print_type_type_edge(F, tp, get_class_supertype(tp, i), TYPE_SUPER_EDGE_ATTR);
			}
			for (i = get_class_n_members(tp); i > 0;) {
				--i;
				print_type_ent_edge(F, tp, get_class_member(tp, i), TYPE_MEMBER_EDGE_ATTR);
			}
			break;
		case tpo_struct:
			for (i = get_struct_n_members(tp); i > 0;) {
				--i;
				print_type_ent_edge(F, tp, get_struct_member(tp, i), TYPE_MEMBER_EDGE_ATTR);
			}
			break;
		case tpo_method:
			for (i = get_method_n_params(tp); i > 0;) {
				--i;
				print_type_type_edge(F, tp, get_method_param_type(tp, i), METH_PAR_EDGE_ATTR,i);
			}
			for (i = get_method_n_ress(tp); i > 0;) {
				 --i;
				print_type_type_edge(F, tp, get_method_res_type(tp, i), METH_RES_EDGE_ATTR,i);
			}
			break;
		case tpo_union:
			for (i = get_union_n_members(tp); i > 0;) {
				 --i;
				print_type_ent_edge(F, tp, get_union_member(tp, i), UNION_EDGE_ATTR);
			}
			break;
		case tpo_array:
			print_type_type_edge(F, tp, get_array_element_type(tp), ARR_ELT_TYPE_EDGE_ATTR);
			print_type_ent_edge(F, tp, get_array_element_entity(tp), ARR_ENT_EDGE_ATTR);
			for (i = get_array_n_dimensions(tp); i > 0;) {
				ir_node *upper, *lower;

				 --i;
				upper = get_array_upper_bound(tp, i);
				lower = get_array_lower_bound(tp, i);
				print_node_type_edge(F, upper, tp, "label: \"upper %zu\"", get_array_order(tp, i));
				print_node_type_edge(F, lower, tp, "label: \"lower %zu\"", get_array_order(tp, i));
				dump_const_expression(F, upper);
				dump_const_expression(F, lower);
			}
			break;
		case tpo_enumeration:
			for (i = get_enumeration_n_enums(tp); i > 0;) {
				 --i;
				dump_enum_item(F, tp, i);
				print_enum_item_edge(F, tp, i, "label: \"item %zu\"", i);
			}
			break;
		case tpo_pointer:
			print_type_type_edge(F, tp, get_pointer_points_to_type(tp), PTR_PTS_TO_EDGE_ATTR);
			break;
		case tpo_unknown:
		case tpo_code:
		case tpo_uninitialized:
		case tpo_none:
		case tpo_primitive:
			break;
		}
		break; /* case k_type */
	}
	default:
		printf(" *** irdump,  dump_type_info(l.%i), faulty type.\n", __LINE__);
	}
}

/** For dumping class hierarchies.
 * Dumps a class type node and a superclass edge.
 */
static void dump_class_hierarchy_node(type_or_ent tore, void *ctx)
{
	FILE *F = (FILE*)ctx;
	size_t i = 0;  /* to shutup gcc */

	/* dump this type or entity */
	switch (get_kind(tore.ent)) {
	case k_entity: {
		ir_entity *ent = tore.ent;
		if (get_entity_owner(ent) == get_glob_type()) break;
		if (!is_Method_type(get_entity_type(ent)))
			break;  /* GL */
		if (flags & ir_dump_flag_entities_in_hierarchy
				&& is_Class_type(get_entity_owner(ent))) {
			/* The node */
			dump_entity_node(F, ent);
			/* The edges */
			print_type_ent_edge(F, get_entity_owner(ent), ent, TYPE_MEMBER_EDGE_ATTR);
			for (i = get_entity_n_overwrites(ent); i > 0;) {
				 --i;
				print_ent_ent_edge(F, get_entity_overwrites(ent, i), ent, 0, ird_color_none, ENT_OVERWRITES_EDGE_ATTR);
			}
		}
		break;
	}
	case k_type: {
		ir_type *tp = tore.typ;
		if (tp == get_glob_type())
			break;
		switch (get_type_tpop_code(tp)) {
		case tpo_class:
			dump_type_node(F, tp);
			/* and now the edges */
			for (i = get_class_n_supertypes(tp); i > 0;) {
				 --i;
				print_type_type_edge(F,tp,get_class_supertype(tp, i),TYPE_SUPER_EDGE_ATTR);
			}
			break;
		default: break;
		}
		break; /* case k_type */
	}
	default:
		printf(" *** irdump,  dump_class_hierarchy_node(l.%i), faulty type.\n", __LINE__);
	}
}

/*******************************************************************/
/* dump analysis information that is expressed in graph terms.     */
/*******************************************************************/

/* dump out edges */
static void dump_out_edge(ir_node *n, void *env)
{
	FILE *F = (FILE*)env;
	int i;
	for (i = get_irn_n_outs(n) - 1; i >= 0; --i) {
		ir_node *succ = get_irn_out(n, i);
		assert(succ);
		print_node_edge_kind(F, succ);
		fprintf(F, "{sourcename: ");
		print_nodeid(F, n);
		fprintf(F, " targetname: ");
		print_nodeid(F, succ);
		fprintf(F, " color: red linestyle: dashed");
		fprintf(F, "}\n");
	}
}

static void dump_loop_label(FILE *F, const ir_loop *loop)
{
	fprintf(F, "loop %u", get_loop_depth(loop));
}

static void dump_loop_info(FILE *F, const ir_loop *loop)
{
	fprintf(F, " info1: \"");
	fprintf(F, " loop nr: %ld", get_loop_loop_nr(loop));
#ifdef DEBUG_libfirm
	fprintf(F, "\n The loop was analyzed %ld times.", (long int) PTR_TO_INT(get_loop_link(loop)));
#endif
	fprintf(F, "\"");
}

void print_loopid(FILE *F, const ir_loop *loop)
{
	fprintf(F, "\"l%ld\"", get_loop_loop_nr(loop));
}

static void dump_loop_node(FILE *F, const ir_loop *loop)
{
	fprintf(F, "node: {title: ");
	print_loopid(F, loop);
	fprintf(F, " label: \"");
	dump_loop_label(F, loop);
	fprintf(F, "\" ");
	dump_loop_info(F, loop);
	fprintf(F, "}\n");
}

static void dump_loop_node_edge(FILE *F, const ir_loop *loop, size_t i)
{
	assert(loop);
	fprintf(F, "edge: {sourcename: ");
	print_loopid(F, loop);
	fprintf(F, " targetname: ");
	print_nodeid(F, get_loop_element(loop, i).node);
	fprintf(F, " color: green");
	fprintf(F, "}\n");
}

static void dump_loop_son_edge(FILE *F, const ir_loop *loop, size_t i)
{
	assert(loop);
	fprintf(F, "edge: {sourcename: ");
	print_loopid(F, loop);
	fprintf(F, " targetname: ");
	print_loopid(F, get_loop_element(loop, i).son);
	ir_fprintf(F, " color: darkgreen label: \"%zu\"}\n", i);
}

static void dump_loops(FILE *F, const ir_loop *loop)
{
	size_t i;
	size_t n_elements = get_loop_n_elements(loop);
	/* dump this loop node */
	dump_loop_node(F, loop);

	/* dump edges to nodes in loop -- only if it is a real loop */
	if (get_loop_depth(loop) != 0) {
		for (i = n_elements; i > 0;) {
			loop_element element;
			--i;
			element = get_loop_element(loop, i);
			if (*element.kind != k_ir_node)
				continue;
			dump_loop_node_edge(F, loop, i);
		}
	}
	for (i = n_elements; i > 0;) {
		loop_element element;
		--i;
		element = get_loop_element(loop, i);
		if (*element.kind != k_ir_loop)
			continue;
		dump_loops(F, element.son);
		dump_loop_son_edge(F, loop, i);
	}
}

static void dump_loop_nodes_into_graph(FILE *F, ir_graph *irg)
{
	ir_loop *loop = get_irg_loop(irg);
	if (loop != NULL) {
		dump_loops(F, loop);
	}
}

void dump_vcg_header_colors(FILE *F)
{
	int i;
	init_colors();
	for (i = 0; i < ird_color_count; ++i) {
		if (color_rgb[i] != NULL) {
			fprintf(F, "colorentry %s: %s\n", color_names[i], color_rgb[i]);
		}
	}
}

void dump_vcg_infonames(FILE *F)
{
	fputs(
		"infoname 1: \"Attribute\"\n"
		"infoname 2: \"Verification errors\"\n"
		"infoname 3: \"Debug info\"\n", F);
}

/**
 * dumps the VCG header
 */
void dump_vcg_header(FILE *F, const char *name, const char *layout, const char *orientation)
{
	const char *label
		= (flags & ir_dump_flag_disable_edge_labels) ? "no" : "yes";

	if (! layout)     layout = "Compilergraph";
	if (!orientation) orientation = "bottom_to_top";

	/* print header */
	fprintf(F,
		"graph: { title: \"ir graph of %s\"\n"
		"display_edge_labels: %s\n"
		"layoutalgorithm: mindepth //$ \"%s\"\n"
		"manhattan_edges: yes\n"
		"port_sharing: no\n"
		"orientation: %s\n"
		"classname 1:  \"intrablock Data\"\n"
		"classname 2:  \"Block\"\n"
		"classname 3:  \"Entity type\"\n"
		"classname 4:  \"Entity owner\"\n"
		"classname 5:  \"Method Param\"\n"
		"classname 6:  \"Method Res\"\n"
		"classname 7:  \"Super\"\n"
		"classname 8:  \"Union\"\n"
		"classname 9:  \"Points-to\"\n"
		"classname 10: \"Array Element Type\"\n"
		"classname 11: \"Overwrites\"\n"
		"classname 12: \"Member\"\n"
		"classname 13: \"Control Flow\"\n"
		"classname 14: \"intrablock Memory\"\n"
		"classname 15: \"Dominators\"\n"
		"classname 16: \"interblock Data\"\n"
		"classname 17: \"interblock Memory\"\n"
		"classname 18: \"Exception Control Flow for Interval Analysis\"\n"
		"classname 19: \"Postdominators\"\n"
		"classname 20: \"Keep Alive\"\n"
		"classname 21: \"Out Edges\"\n"
		"classname 22: \"Macro Block Edges\"\n",
		name, label, layout, orientation);
	dump_vcg_infonames(F);
	dump_vcg_header_colors(F);
	fprintf(F, "\n");
}

/**
 * Dumps the vcg file footer
 */
void dump_vcg_footer(FILE *F)
{
	fprintf(F, "}\n");
}



static void dump_blocks_as_subgraphs(FILE *out, ir_graph *irg)
{
	size_t i;

	construct_block_lists(irg);

	/*
	 * If we are in the interprocedural view, we dump not
	 * only the requested irg but also all irgs that can be reached
	 * from irg.
	 */
	for (i = get_irp_n_irgs(); i > 0;) {
		ir_graph *other_irg = get_irp_irg(--i);
		ir_node **arr = (ir_node**)ird_get_irg_link(other_irg);
		if (arr == NULL)
			continue;

		dump_graph_from_list(out, other_irg);
		DEL_ARR_F(arr);
	}
}

void dump_ir_graph_file(FILE *out, ir_graph *irg)
{
	dump_vcg_header(out, get_irg_dump_name(irg), NULL, NULL);

	/* dump nodes */
	if (flags & ir_dump_flag_blocks_as_subgraphs) {
		dump_blocks_as_subgraphs(out, irg);
	} else {
		/* dump_node_with_edges must be called in post visiting predecessors */
		ird_walk_graph(irg, NULL, dump_node_with_edges, out);
	}

	/* dump type info */
	if (flags & ir_dump_flag_with_typegraph) {
		type_walk_irg(irg, dump_type_info, NULL, out);
		inc_irg_visited(get_const_code_irg());
		/* dump edges from graph to type info */
		irg_walk(get_irg_end(irg), dump_node2type_edges, NULL, out);
	}

	/* dump iredges out edges */
	if ((flags & ir_dump_flag_iredges) && edges_activated(irg)) {
		irg_walk_edges(get_irg_start_block(irg), dump_ir_edges, NULL, out);
	}

	/* dump the out edges in a separate walk */
	if ((flags & ir_dump_flag_out_edges)
			&& (irg_has_properties(irg, IR_GRAPH_PROPERTY_CONSISTENT_OUTS))) {
		irg_out_walk(get_irg_start(irg), dump_out_edge, NULL, out);
	}

	dump_vcg_footer(out);
}

static void dump_block_to_cfg(ir_node *block, void *env)
{
	FILE *F = (FILE*)env;
	int i;

	if (is_Bad(block) && get_irn_mode(block) == mode_X) {
		dump_node(F, block);
	}

	if (is_Block(block)) {
		/* This is a block. Dump a node for the block. */
		fprintf(F, "node: {title: ");
		print_nodeid(F, block);
		fprintf(F, " label: \"");
		if (block == get_irg_start_block(get_irn_irg(block)))
			fprintf(F, "Start ");
		if (block == get_irg_end_block(get_irn_irg(block)))
			fprintf(F, "End ");

		fprintf(F, "%s ", get_op_name(get_irn_op(block)));
		print_nodeid(F, block);
		fprintf(F, "\" ");
		fprintf(F, "info1:\"");

		/* the generic version. */
		dump_irnode_to_file(F, block);

		fprintf(F, "\"");  /* closing quote of info */

		if ((block == get_irg_start_block(get_irn_irg(block))) ||
			(block == get_irg_end_block(get_irn_irg(block)))     )
			fprintf(F, " color:blue ");

		fprintf(F, "}\n");

		/* Dump the edges */
		for (i = get_Block_n_cfgpreds(block) - 1; i >= 0; --i) {
			ir_node *pred = get_Block_cfgpred(block, i);
			if (!is_Bad(pred))
				pred = get_nodes_block(pred);
			fprintf(F, "edge: { sourcename: ");
			print_nodeid(F, block);
			fprintf(F, " targetname: ");
			print_nodeid(F, pred);
			fprintf(F, "\"}\n");
		}

		/* Dump dominator/postdominator edge */
		if (ir_get_dump_flags() & ir_dump_flag_dominance) {
			if (irg_has_properties(get_irn_irg(block), IR_GRAPH_PROPERTY_CONSISTENT_DOMINANCE) && get_Block_idom(block)) {
				ir_node *pred = get_Block_idom(block);
				fprintf(F, "edge: { sourcename: ");
				print_nodeid(F, block);
				fprintf(F, " targetname: ");
				print_nodeid(F, pred);
				fprintf(F, " " DOMINATOR_EDGE_ATTR "}\n");
			}
			if (irg_has_properties(get_irn_irg(block), IR_GRAPH_PROPERTY_CONSISTENT_POSTDOMINANCE) && get_Block_ipostdom(block)) {
				ir_node *pred = get_Block_ipostdom(block);
				fprintf(F, "edge: { sourcename: ");
				print_nodeid(F, block);
				fprintf(F, " targetname: ");
				print_nodeid(F, pred);
				fprintf(F, " " POSTDOMINATOR_EDGE_ATTR "}\n");
			}
		}
	}
}

void dump_cfg(FILE *F, ir_graph *irg)
{
	dump_vcg_header(F, get_irg_dump_name(irg), NULL, NULL);

	/* walk over the blocks in the graph */
	irg_walk_graph(irg, dump_block_to_cfg, NULL, F);

	dump_vcg_footer(F);
}

void dump_callgraph(FILE *F)
{
	size_t          i;
	ir_dump_flags_t old_flags = ir_get_dump_flags();

	ir_remove_dump_flags(ir_dump_flag_disable_edge_labels);
	dump_vcg_header(F, "Callgraph", "Hierarchic", NULL);

	for (i = get_irp_n_irgs(); i > 0;) {
		ir_graph *irg = get_irp_irg(--i);
		ir_entity *ent = get_irg_entity(irg);
		size_t j, n_callees = get_irg_n_callees(irg);

		dump_entity_node(F, ent);
		for (j = 0; j < n_callees; ++j) {
			ir_entity  *c    = get_irg_entity(get_irg_callee(irg, j));
			int         be   = is_irg_callee_backedge(irg, j);
			const char *attr = be
				? "label:\"recursion %zu\""
				: "label:\"calls %zu\"";
			print_ent_ent_edge(F, ent, c, be, ird_color_entity, attr,
			                   get_irg_callee_loop_depth(irg, j));
		}
	}

	ir_set_dump_flags(old_flags);
	dump_vcg_footer(F);
}

void dump_typegraph(FILE *out)
{
	dump_vcg_header(out, "All_types", NULL, NULL);
	type_walk(dump_type_info, NULL, out);
	dump_vcg_footer(out);
}

void dump_class_hierarchy(FILE *out)
{
	dump_vcg_header(out, "class_hierarchy", NULL, NULL);
	type_walk(dump_class_hierarchy_node, NULL, out);
	dump_vcg_footer(out);
}

static void dump_loops_standalone(FILE *F, ir_loop *loop)
{
	size_t i;
	bool   loop_node_started = false;
	size_t first      = 0;
	size_t son_number = 0;
	loop_element le;
	ir_loop *son = NULL;

	/* Dump a new loop node. */
	dump_loop_node(F, loop);

	/* Dump the loop elements. */
	for (i = 0; i < get_loop_n_elements(loop); i++) {
		le = get_loop_element(loop, i);
		son = le.son;
		if (get_kind(son) == k_ir_loop) {

			/* We are a loop son -> Recurse */

			if (loop_node_started) { /* Close the "firm-nodes" node first if we started one. */
				fprintf(F, "\" }\n");
				fprintf(F, "edge: {sourcename: ");
				print_loopid(F, loop);
				fprintf(F, " targetname: \"l%ld-%lu-nodes\" label:\"%lu...%lu\"}\n",
				        get_loop_loop_nr(loop),
						(unsigned long) first,
						(unsigned long) first,
				        (unsigned long) i-1);
				loop_node_started = false;
			}
			dump_loop_son_edge(F, loop, son_number++);
			dump_loops_standalone(F, son);
		} else if (get_kind(son) == k_ir_node) {
			/* We are a loop node -> Collect firm nodes */

			ir_node *n = le.node;
			if (!loop_node_started) {
				/* Start a new node which contains all firm nodes of the current loop */
				fprintf(F, "node: { title: \"l%ld-%lu-nodes\" color: lightyellow label: \"",
				        get_loop_loop_nr(loop),
				        (unsigned long)i);
				loop_node_started = true;
				first = i;
			} else
				fprintf(F, "\n");

			dump_node_label(F, n);
			if (has_backedges(n)) fprintf(F, "\t loop head!");
		} else { /* for callgraph loop tree */
			ir_graph *n;
			assert(get_kind(son) == k_ir_graph);

			/* We are a loop node -> Collect firm graphs */
			n = le.irg;
			if (!loop_node_started) {
				/* Start a new node which contains all firm nodes of the current loop */
				fprintf(F, "node: { title: \"l%ld-%lu-nodes\" color: lightyellow label: \"",
				        get_loop_loop_nr(loop),
				        (unsigned long)i);
				loop_node_started = true;
				first = i;
			} else
				fprintf(F, "\n");
			fprintf(F, " %s", get_irg_dump_name(n));
			/* fprintf(F, " %s (depth %d)", get_irg_dump_name(n), n->callgraph_weighted_loop_depth); */
		}
	}

	if (loop_node_started) {
		fprintf(F, "\" }\n");
		fprintf(F, "edge: {sourcename: \"");
		print_loopid(F, loop);
		fprintf(F, "\" targetname: \"l%ld-%lu-nodes\" label:\"%lu...%lu\"}\n",
		        get_loop_loop_nr(loop),
		        (unsigned long) first,
		        (unsigned long) first,
		        (unsigned long) i-1);
		loop_node_started = false;
	}
}

void dump_loop_tree(FILE *out, ir_graph *irg)
{
	ir_dump_flags_t old_flags = ir_get_dump_flags();

	ir_remove_dump_flags(ir_dump_flag_disable_edge_labels);

	dump_vcg_header(out, get_irg_dump_name(irg), "Tree", "top_to_bottom");

	if (get_irg_loop(irg))
		dump_loops_standalone(out, get_irg_loop(irg));

	dump_vcg_footer(out);

	ir_set_dump_flags(old_flags);
}

void dump_callgraph_loop_tree(FILE *out)
{
	dump_vcg_header(out, "callgraph looptree", "Tree", "top_to_bottom");
	dump_loops_standalone(out, irp->outermost_cg_loop);
	dump_vcg_footer(out);
}

static void collect_nodeloop(FILE *F, ir_loop *loop, pset *loopnodes)
{
	size_t i;
	int    son_number = 0;
	int    node_number = 0;

	if (flags & ir_dump_flag_loops)
		dump_loop_node(F, loop);

	for (i = 0; i < get_loop_n_elements(loop); i++) {
		loop_element le = get_loop_element(loop, i);
		if (*(le.kind) == k_ir_loop) {
			if (flags & ir_dump_flag_loops)
				dump_loop_son_edge(F, loop, son_number++);
			/* Recur */
			collect_nodeloop(F, le.son, loopnodes);
		} else {
			if (flags & ir_dump_flag_loops)
				dump_loop_node_edge(F, loop, node_number++);
			pset_insert_ptr(loopnodes, le.node);
		}
	}
}

static void collect_nodeloop_external_nodes(ir_loop *loop, pset *loopnodes,
                                            pset *extnodes)
{
	size_t i;
	int j, start;

	for (i = 0; i < get_loop_n_elements(loop); i++) {
		loop_element le = get_loop_element(loop, i);
		if (*(le.kind) == k_ir_loop) {
			/* Recur */
			collect_nodeloop_external_nodes(le.son, loopnodes, extnodes);
		} else {
			if (is_Block(le.node)) start = 0; else start = -1;
			for (j = start; j < get_irn_arity(le.node); j++) {
				ir_node *pred = get_irn_n(le.node, j);
				if (!pset_find_ptr(loopnodes, pred)) {
					pset_insert_ptr(extnodes, pred);
					if (!is_Block(pred)) {
						pred = get_nodes_block(pred);
						if (!pset_find_ptr(loopnodes, pred))
							pset_insert_ptr(extnodes, pred);
					}
				}
			}
		}
	}
}

void dump_loop(FILE *F, ir_loop *l)
{
	pset *loopnodes = pset_new_ptr_default();
	pset *extnodes  = pset_new_ptr_default();
	char name[50];

	snprintf(name, sizeof(name), "loop_%ld", get_loop_loop_nr(l));
	dump_vcg_header(F, name, NULL, NULL);

	/* collect all nodes to dump */
	collect_nodeloop(F, l, loopnodes);
	collect_nodeloop_external_nodes(l, loopnodes, extnodes);

	/* build block lists */
	foreach_pset(loopnodes, ir_node, n) {
		set_irn_link(n, NULL);
	}
	foreach_pset(extnodes, ir_node, n) {
		set_irn_link(n, NULL);
	}
	foreach_pset(loopnodes, ir_node, n) {
		if (!is_Block(n)) {
			ir_node *const b = get_nodes_block(n);
			set_irn_link(n, get_irn_link(b));
			set_irn_link(b, n);
		}
	}
	foreach_pset(extnodes, ir_node, n) {
		if (!is_Block(n)) {
			ir_node *const b = get_nodes_block(n);
			set_irn_link(n, get_irn_link(b));
			set_irn_link(b, n);
		}
	}

	foreach_pset(loopnodes, ir_node, b) {
		if (is_Block(b)) {
			fprintf(F, "graph: { title: ");
			print_nodeid(F, b);
			fprintf(F, "  label: \"");
			dump_node_opcode(F, b);
			fprintf(F, " %ld:%u", get_irn_node_nr(b), get_irn_idx(b));
			fprintf(F, "\" status:clustered color:yellow\n");

			/* dump the blocks edges */
			dump_ir_data_edges(F, b);

			/* dump the nodes that go into the block */
			for (ir_node *n = (ir_node*)get_irn_link(b); n; n = (ir_node*)get_irn_link(n)) {
				if (pset_find_ptr(extnodes, n))
					overrule_nodecolor = ird_color_block_inout;
				dump_node(F, n);
				overrule_nodecolor = ird_color_default_node;
				if (!pset_find_ptr(extnodes, n)) dump_ir_data_edges(F, n);
			}

			/* Close the vcg information for the block */
			fprintf(F, "}\n");
			dump_const_node_local(F, b);
			fprintf(F, "\n");
		}
	}
	foreach_pset(extnodes, ir_node, b) {
		if (is_Block(b)) {
			fprintf(F, "graph: { title: ");
			print_nodeid(F, b);
			fprintf(F, " label: \"");
			dump_node_opcode(F, b);
			fprintf(F, " %ld:%u", get_irn_node_nr(b), get_irn_idx(b));
			fprintf(F, "\" status:clustered color:lightblue\n");

			/* dump the nodes that go into the block */
			for (ir_node *n = (ir_node*)get_irn_link(b); n; n = (ir_node*)get_irn_link(n)) {
				if (!pset_find_ptr(loopnodes, n))
					overrule_nodecolor = ird_color_block_inout;
				dump_node(F, n);
				overrule_nodecolor = ird_color_default_node;
				if (pset_find_ptr(loopnodes, n)) dump_ir_data_edges(F, n);
			}

			/* Close the vcg information for the block */
			fprintf(F, "}\n");
			dump_const_node_local(F, b);
			fprintf(F, "\n");
		}
	}
	del_pset(loopnodes);
	del_pset(extnodes);

	dump_vcg_footer(F);
}

static bool   obstack_init;
static struct obstack obst;
static char  *dump_path;

void ir_set_dump_path(const char *path)
{
	xfree(dump_path);
	dump_path = xstrdup(path);
}

static void add_string_escaped(const char *string)
{
	const char *p;
	for (p = string; *p != '\0'; ++p) {
		char c = *p;
		if (c == '/') {
			obstack_1grow(&obst, '@');
			obstack_1grow(&obst, '1');
		} else if (c == '@') {
			obstack_1grow(&obst, '@');
			obstack_1grow(&obst, '2');
		} else {
			obstack_1grow(&obst, c);
		}
	}
}

static void add_dump_path(void)
{
	if (!obstack_init) {
		obstack_init(&obst);
		obstack_init = true;
	}

	if (dump_path != NULL) {
		size_t len = strlen(dump_path);
		obstack_grow(&obst, dump_path, len);
		if (len > 0 && dump_path[len-1] != '/')
			obstack_1grow(&obst, '/');
	}
}

void dump_ir_graph_ext(ir_graph_dump_func func, ir_graph *graph,
                       const char *suffix)
{
	const char *dump_name = get_irg_dump_name(graph);
	char       *file_name;
	FILE       *out;

	if (!ir_should_dump(dump_name))
		return;

	add_dump_path();

	add_string_escaped(dump_name);
	obstack_printf(&obst, "-%02u", graph->dump_nr++);

	if (suffix != NULL) {
		if (suffix[0] != '.')
			obstack_1grow(&obst, '-');
		add_string_escaped(suffix);
	}
	obstack_1grow(&obst, '\0');

	file_name = (char*)obstack_finish(&obst);
	/* xvcg expects only <LF> so we need "b"inary mode (for win32) */
	out       = fopen(file_name, "wb");
	obstack_free(&obst, file_name);

	if (out == NULL) {
		fprintf(stderr, "Couldn't open '%s': %s\n", file_name, strerror(errno));
		return;
	}

	func(out, graph);
	fclose(out);
}

void dump_ir_prog_ext(ir_prog_dump_func func, const char *suffix)
{
	char *file_name;
	FILE *out;

	add_dump_path();

	obstack_printf(&obst, "%02u", irp->dump_nr++);
	if (suffix != NULL) {
		if (suffix[0] != '.')
			obstack_1grow(&obst, '-');
		add_string_escaped(suffix);
	}
	obstack_1grow(&obst, '\0');

	file_name = (char*)obstack_finish(&obst);
	out       = fopen(file_name, "wb");
	obstack_free(&obst, file_name);

	if (out == NULL) {
		fprintf(stderr, "Couldn't open '%s': %s\n", file_name, strerror(errno));
		return;
	}
	func(out);
	fclose(out);
}

void dump_ir_graph(ir_graph *graph, const char *suffix)
{
	char buf[256];

	snprintf(buf, sizeof(buf), "%s.vcg", suffix);
	dump_ir_graph_ext(dump_ir_graph_file, graph, buf);
}

void dump_all_ir_graphs(const char *suffix)
{
	size_t i, n_irgs = get_irp_n_irgs();

	for (i = 0; i < n_irgs; ++i) {
		ir_graph *irg = get_irp_irg(i);
		dump_ir_graph(irg, suffix);
	}
}

typedef struct pass_t {
	ir_prog_pass_t pass;
	char           suffix[1];
} pass_t;

/**
 * Wrapper around dump_all_ir_graphs().
 */
static int dump_all_ir_graphs_wrapper(ir_prog *irp, void *context)
{
	pass_t *pass = (pass_t*)context;

	(void)irp;
	dump_all_ir_graphs(pass->suffix);
	return 0;
}

ir_prog_pass_t *dump_all_ir_graph_pass(const char *name, const char *suffix)
{
	size_t  len  = strlen(suffix) + 1;
	pass_t *pass = XMALLOCF(pass_t, suffix, len);
	ir_prog_pass_t *res  = def_prog_pass_constructor(
		&pass->pass, name ? name : "dump_all_graphs", dump_all_ir_graphs_wrapper);

	/* this pass does not change anything, so neither dump nor verify is needed. */
	res->dump_irprog   = ir_prog_no_dump;
	res->verify_irprog = ir_prog_no_verify;

	memcpy(pass->suffix, suffix, len);

	return res;
}
