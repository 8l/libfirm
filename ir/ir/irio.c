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
 * @brief   Write textual representation of firm to file.
 * @author  Moritz Kroll, Matthias Braun
 */
#include "config.h"

#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdarg.h>

#include "irio.h"

#include "irnode_t.h"
#include "irprog.h"
#include "irgraph_t.h"
#include "irprintf.h"
#include "ircons_t.h"
#include "irgmod.h"
#include "irflag_t.h"
#include "irgwalk.h"
#include "tv.h"
#include "array.h"
#include "error.h"
#include "typerep.h"
#include "set.h"
#include "obst.h"
#include "pmap.h"
#include "pdeq.h"

#define SYMERROR ((unsigned) ~0)

static void register_generated_node_readers(void);
static void register_generated_node_writers(void);

typedef struct delayed_initializer_t {
	ir_initializer_t *initializer;
	long              node_nr;
} delayed_initializer_t;

typedef struct delayed_pred_t {
	ir_node *node;
	int      n_preds;
	long     preds[];
} delayed_pred_t;

typedef struct read_env_t {
	int            c;           /**< currently read char */
	FILE          *file;
	const char    *inputname;
	unsigned       line;

	ir_graph      *irg;
	set           *idset;       /**< id_entry set, which maps from file ids to
	                                 new Firm elements */
	ir_type      **fixedtypes;
	bool           read_errors;
	struct obstack obst;
	struct obstack preds_obst;
	delayed_initializer_t *delayed_initializers;
	const delayed_pred_t **delayed_preds;
} read_env_t;

typedef struct write_env_t {
	FILE *file;
	pdeq *write_queue;
} write_env_t;

typedef enum typetag_t {
	tt_align,
	tt_builtin_kind,
	tt_cond_jmp_predicate,
	tt_initializer,
	tt_irg_inline_property,
	tt_keyword,
	tt_linkage,
	tt_mode_arithmetic,
	tt_pin_state,
	tt_segment,
	tt_throws,
	tt_tpo,
	tt_type_state,
	tt_visibility,
	tt_volatility,
	tt_where_alloc,
} typetag_t;

typedef enum keyword_t {
	kw_asm,
	kw_compound_member,
	kw_constirg,
	kw_entity,
	kw_float_mode,
	kw_int_mode,
	kw_irg,
	kw_label,
	kw_method,
	kw_modes,
	kw_parameter,
	kw_program,
	kw_reference_mode,
	kw_segment_type,
	kw_type,
	kw_typegraph,
	kw_unknown,
} keyword_t;

typedef struct symbol_t {
	const char *str;      /**< The name of this symbol. */
	typetag_t   typetag;  /**< The type tag of this symbol. */
	unsigned    code;     /**< The value of this symbol. */
} symbol_t;

typedef struct id_entry {
	long id;
	void *elem;
} id_entry;

/** The symbol table, a set of symbol_t elements. */
static set *symtbl;

/**
 * Compare two symbol table entries.
 */
static int symbol_cmp(const void *elt, const void *key, size_t size)
{
	int res;
	const symbol_t *entry = (const symbol_t *) elt;
	const symbol_t *keyentry = (const symbol_t *) key;
	(void) size;
	res = entry->typetag - keyentry->typetag;
	if (res) return res;
	return strcmp(entry->str, keyentry->str);
}

static int id_cmp(const void *elt, const void *key, size_t size)
{
	const id_entry *entry = (const id_entry *) elt;
	const id_entry *keyentry = (const id_entry *) key;
	(void) size;
	return entry->id - keyentry->id;
}

static void __attribute__((format(printf, 2, 3)))
parse_error(read_env_t *env, const char *fmt, ...)
{
	va_list  ap;
	unsigned line = env->line;

	/* workaround read_c "feature" that a '\n' triggers the line++
	 * instead of the character after the '\n' */
	if (env->c == '\n') {
		line--;
	}

	fprintf(stderr, "%s:%u: error ", env->inputname, line);
	env->read_errors = true;

	/* let's hope firm doesn't die on further errors */
	do_node_verification(0);

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

/** Initializes the symbol table. May be called more than once without problems. */
static void symtbl_init(void)
{
	symbol_t key;

	/* Only initialize once */
	if (symtbl != NULL)
		return;

	symtbl = new_set(symbol_cmp, 256);

#define INSERT(tt, s, cod)                                       \
	key.str = (s);                                               \
	key.typetag = (tt);                                          \
	key.code = (cod);                                            \
	set_insert(symtbl, &key, sizeof(key), hash_str(s) + tt * 17)

#define INSERTENUM(tt, e) INSERT(tt, #e, e)
#define INSERTKEYWORD(k) INSERT(tt_keyword, #k, kw_##k)

	INSERT(tt_tpo, "array", tpo_array);
	INSERT(tt_tpo, "class", tpo_class);
	INSERT(tt_tpo, "method", tpo_method);
	INSERT(tt_tpo, "pointer", tpo_pointer);
	INSERT(tt_tpo, "primitive", tpo_primitive);
	INSERT(tt_tpo, "struct", tpo_struct);
	INSERT(tt_tpo, "union", tpo_union);
	INSERT(tt_tpo, "Unknown", tpo_unknown);

	INSERT(tt_segment, "global", IR_SEGMENT_GLOBAL);
	INSERT(tt_segment, "thread_local", IR_SEGMENT_THREAD_LOCAL);
	INSERT(tt_segment, "constructors", IR_SEGMENT_CONSTRUCTORS);
	INSERT(tt_segment, "destructors", IR_SEGMENT_DESTRUCTORS);

	INSERT(tt_linkage, "constant", IR_LINKAGE_CONSTANT);
	INSERT(tt_linkage, "weak", IR_LINKAGE_WEAK);
	INSERT(tt_linkage, "garbage_collect", IR_LINKAGE_GARBAGE_COLLECT);
	INSERT(tt_linkage, "merge", IR_LINKAGE_MERGE);
	INSERT(tt_linkage, "hidden_user", IR_LINKAGE_HIDDEN_USER);

	INSERT(tt_visibility, "local", ir_visibility_local);
	INSERT(tt_visibility, "external", ir_visibility_external);
	INSERT(tt_visibility, "default", ir_visibility_default);
	INSERT(tt_visibility, "private", ir_visibility_private);

	INSERT(tt_throws, "throw",   true);
	INSERT(tt_throws, "nothrow", false);

	INSERTKEYWORD(asm);
	INSERTKEYWORD(compound_member);
	INSERTKEYWORD(constirg);
	INSERTKEYWORD(entity);
	INSERTKEYWORD(float_mode);
	INSERTKEYWORD(int_mode);
	INSERTKEYWORD(irg);
	INSERTKEYWORD(label);
	INSERTKEYWORD(method);
	INSERTKEYWORD(modes);
	INSERTKEYWORD(parameter);
	INSERTKEYWORD(program);
	INSERTKEYWORD(reference_mode);
	INSERTKEYWORD(segment_type);
	INSERTKEYWORD(type);
	INSERTKEYWORD(typegraph);
	INSERTKEYWORD(unknown);

	INSERTENUM(tt_align, align_non_aligned);
	INSERTENUM(tt_align, align_is_aligned);

	INSERTENUM(tt_builtin_kind, ir_bk_trap);
	INSERTENUM(tt_builtin_kind, ir_bk_debugbreak);
	INSERTENUM(tt_builtin_kind, ir_bk_return_address);
	INSERTENUM(tt_builtin_kind, ir_bk_frame_address);
	INSERTENUM(tt_builtin_kind, ir_bk_prefetch);
	INSERTENUM(tt_builtin_kind, ir_bk_ffs);
	INSERTENUM(tt_builtin_kind, ir_bk_clz);
	INSERTENUM(tt_builtin_kind, ir_bk_ctz);
	INSERTENUM(tt_builtin_kind, ir_bk_popcount);
	INSERTENUM(tt_builtin_kind, ir_bk_parity);
	INSERTENUM(tt_builtin_kind, ir_bk_bswap);
	INSERTENUM(tt_builtin_kind, ir_bk_inport);
	INSERTENUM(tt_builtin_kind, ir_bk_outport);
	INSERTENUM(tt_builtin_kind, ir_bk_inner_trampoline);

	INSERTENUM(tt_cond_jmp_predicate, COND_JMP_PRED_NONE);
	INSERTENUM(tt_cond_jmp_predicate, COND_JMP_PRED_TRUE);
	INSERTENUM(tt_cond_jmp_predicate, COND_JMP_PRED_FALSE);

	INSERTENUM(tt_initializer, IR_INITIALIZER_CONST);
	INSERTENUM(tt_initializer, IR_INITIALIZER_TARVAL);
	INSERTENUM(tt_initializer, IR_INITIALIZER_NULL);
	INSERTENUM(tt_initializer, IR_INITIALIZER_COMPOUND);

	INSERT(tt_mode_arithmetic, "uninitialized",      irma_uninitialized);
	INSERT(tt_mode_arithmetic, "none",               irma_none);
	INSERT(tt_mode_arithmetic, "twos_complement",    irma_twos_complement);
	INSERT(tt_mode_arithmetic, "ieee754",            irma_ieee754);
	INSERT(tt_mode_arithmetic, "x86_extended_float", irma_x86_extended_float);

	INSERT(tt_irg_inline_property, "any",            irg_inline_any);
	INSERT(tt_irg_inline_property, "recommended",    irg_inline_recomended);
	INSERT(tt_irg_inline_property, "forbidden",      irg_inline_forbidden);
	INSERT(tt_irg_inline_property, "forced",         irg_inline_forced);
	INSERT(tt_irg_inline_property, "forced_no_body", irg_inline_forced_no_body);

	INSERTENUM(tt_pin_state, op_pin_state_floats);
	INSERTENUM(tt_pin_state, op_pin_state_pinned);
	INSERTENUM(tt_pin_state, op_pin_state_exc_pinned);
	INSERTENUM(tt_pin_state, op_pin_state_mem_pinned);

	INSERTENUM(tt_type_state, layout_undefined);
	INSERTENUM(tt_type_state, layout_fixed);

	INSERTENUM(tt_volatility, volatility_non_volatile);
	INSERTENUM(tt_volatility, volatility_is_volatile);

	INSERTENUM(tt_where_alloc, stack_alloc);
	INSERTENUM(tt_where_alloc, heap_alloc);

#undef INSERTKEYWORD
#undef INSERTENUM
#undef INSERT
}

static const char *get_segment_name(ir_segment_t segment)
{
	switch (segment) {
	case IR_SEGMENT_GLOBAL:       return "global";
	case IR_SEGMENT_THREAD_LOCAL: return "thread_local";
	case IR_SEGMENT_CONSTRUCTORS: return "constructors";
	case IR_SEGMENT_DESTRUCTORS:  return "destructors";
	}
	panic("INVALID_SEGMENT");
}

static const char *get_visibility_name(ir_visibility visibility)
{
	switch (visibility) {
	case ir_visibility_local:    return "local";
	case ir_visibility_external: return "external";
	case ir_visibility_default:  return "default";
	case ir_visibility_private:  return "private";
	}
	panic("INVALID_VISIBILITY");
}

static const char *get_mode_arithmetic_name(ir_mode_arithmetic arithmetic)
{
	switch (arithmetic) {
	case irma_uninitialized:      return "uninitialized";
	case irma_none:               return "none";
	case irma_twos_complement:    return "twos_complement";
	case irma_ieee754:            return "ieee754";
	case irma_x86_extended_float: return "x86_extended_float";
	}
	panic("invalid mode_arithmetic");
}

static const char *get_irg_inline_property_name(irg_inline_property prop)
{
	switch (prop) {
	case irg_inline_any:            return "any";
	case irg_inline_recomended:     return "recommended";
	case irg_inline_forbidden:      return "forbidden";
	case irg_inline_forced:         return "forced";
	case irg_inline_forced_no_body: return "forced_no_body";
	}
	panic("invalid irg_inline_property");
}

/** Returns the according symbol value for the given string and tag, or SYMERROR if none was found. */
static unsigned symbol(const char *str, typetag_t typetag)
{
	symbol_t key, *entry;

	key.str = str;
	key.typetag = typetag;

	entry = (symbol_t*)set_find(symtbl, &key, sizeof(key), hash_str(str) + typetag * 17);
	return entry ? entry->code : SYMERROR;
}

static void write_long(write_env_t *env, long value)
{
	fprintf(env->file, "%ld ", value);
}

static void write_int(write_env_t *env, int value)
{
	fprintf(env->file, "%d ", value);
}

static void write_unsigned(write_env_t *env, unsigned value)
{
	fprintf(env->file, "%u ", value);
}

static void write_size_t(write_env_t *env, size_t value)
{
	ir_fprintf(env->file, "%zu ", value);
}

static void write_symbol(write_env_t *env, const char *symbol)
{
	fputs(symbol, env->file);
	fputc(' ', env->file);
}

static void write_entity_ref(write_env_t *env, ir_entity *entity)
{
	write_long(env, get_entity_nr(entity));
}

static void write_type_ref(write_env_t *env, ir_type *type)
{
	switch (get_type_tpop_code(type)) {
	case tpo_unknown:
		write_symbol(env, "unknown");
		return;
	case tpo_none:
		write_symbol(env, "none");
		return;
	case tpo_code:
		write_symbol(env, "code");
		return;
	default:
		break;
	}
	write_long(env, get_type_nr(type));
}

static void write_string(write_env_t *env, const char *string)
{
	const char *c;
	fputc('"', env->file);
	for (c = string; *c != '\0'; ++c) {
		switch (*c) {
		case '\n':
			fputc('\\', env->file);
			fputc('n', env->file);
			break;
		case '"':
		case '\\':
			fputc('\\', env->file);
			/* FALLTHROUGH */
		default:
			fputc(*c, env->file);
			break;
		}
	}
	fputc('"', env->file);
	fputc(' ', env->file);
}

static void write_ident(write_env_t *env, ident *id)
{
	write_string(env, get_id_str(id));
}

static void write_ident_null(write_env_t *env, ident *id)
{
	if (id == NULL) {
		fputs("NULL ", env->file);
	} else {
		write_ident(env, id);
	}
}

static void write_mode_ref(write_env_t *env, ir_mode *mode)
{
	write_string(env, get_mode_name(mode));
}

static void write_tarval(write_env_t *env, ir_tarval *tv)
{
	write_mode_ref(env, get_tarval_mode(tv));
	if (tv == tarval_bad) {
		write_symbol(env, "bad");
	} else {
		char buf[1024];
		tarval_snprintf(buf, sizeof(buf), tv);
		fputs(buf, env->file);
		fputc(' ', env->file);
	}
}

static void write_align(write_env_t *env, ir_align align)
{
	fputs(get_align_name(align), env->file);
	fputc(' ', env->file);
}

static void write_builtin_kind(write_env_t *env, const ir_node *node)
{
	fputs(get_builtin_kind_name(get_Builtin_kind(node)), env->file);
	fputc(' ', env->file);
}

static void write_cond_jmp_predicate(write_env_t *env, const ir_node *node)
{
	fputs(get_cond_jmp_predicate_name(get_Cond_jmp_pred(node)), env->file);
	fputc(' ', env->file);
}

static void write_relation(write_env_t *env, ir_relation relation)
{
	write_long(env, (long)relation);
}

static void write_where_alloc(write_env_t *env, ir_where_alloc where_alloc)
{
	switch (where_alloc) {
	case stack_alloc: write_symbol(env, "stack_alloc"); return;
	case heap_alloc:  write_symbol(env, "heap_alloc");  return;
	}
	panic("invalid where_alloc value");
}

static void write_throws(write_env_t *env, bool throws)
{
	write_symbol(env, throws ? "throw" : "nothrow");
}

static void write_list_begin(write_env_t *env)
{
	fputs("[", env->file);
}

static void write_list_end(write_env_t *env)
{
	fputs("] ", env->file);
}

static void write_scope_begin(write_env_t *env)
{
	fputs("{\n", env->file);
}

static void write_scope_end(write_env_t *env)
{
	fputs("}\n\n", env->file);
}

static void write_node_ref(write_env_t *env, const ir_node *node)
{
	write_long(env, get_irn_node_nr(node));
}

static void write_initializer(write_env_t *env, ir_initializer_t *ini)
{
	FILE *f = env->file;
	ir_initializer_kind_t ini_kind = get_initializer_kind(ini);

	fputs(get_initializer_kind_name(ini_kind), f);
	fputc(' ', f);

	switch (ini_kind) {
	case IR_INITIALIZER_CONST:
		write_node_ref(env, get_initializer_const_value(ini));
		return;

	case IR_INITIALIZER_TARVAL:
		write_tarval(env, get_initializer_tarval_value(ini));
		return;

	case IR_INITIALIZER_NULL:
		return;

	case IR_INITIALIZER_COMPOUND: {
		size_t i, n = get_initializer_compound_n_entries(ini);
		write_size_t(env, n);
		for (i = 0; i < n; ++i)
			write_initializer(env, get_initializer_compound_value(ini, i));
		return;
	}
	}
	panic("Unknown initializer kind");
}

static void write_pin_state(write_env_t *env, op_pin_state state)
{
	fputs(get_op_pin_state_name(state), env->file);
	fputc(' ', env->file);
}

static void write_volatility(write_env_t *env, ir_volatility vol)
{
	fputs(get_volatility_name(vol), env->file);
	fputc(' ', env->file);
}

static void write_inline_property(write_env_t *env, irg_inline_property prop)
{
	fputs(get_irg_inline_property_name(prop), env->file);
	fputc(' ', env->file);
}

static void write_type_state(write_env_t *env, ir_type_state state)
{
	fputs(get_type_state_name(state), env->file);
	fputc(' ', env->file);
}

static void write_visibility(write_env_t *env, ir_visibility visibility)
{
	fputs(get_visibility_name(visibility), env->file);
	fputc(' ', env->file);
}

static void write_mode_arithmetic(write_env_t *env, ir_mode_arithmetic arithmetic)
{
	fputs(get_mode_arithmetic_name(arithmetic), env->file);
	fputc(' ', env->file);
}

static void write_type(write_env_t *env, ir_type *tp);
static void write_entity(write_env_t *env, ir_entity *entity);

static void write_type_common(write_env_t *env, ir_type *tp)
{
	fputc('\t', env->file);
	write_symbol(env, "type");
	write_long(env, get_type_nr(tp));
	write_symbol(env, get_type_tpop_name(tp));
	write_unsigned(env, get_type_size_bytes(tp));
	write_unsigned(env, get_type_alignment_bytes(tp));
	write_type_state(env, get_type_state(tp));
	write_unsigned(env, tp->flags);
}

static void write_type_primitive(write_env_t *env, ir_type *tp)
{
	ir_type *base_type = get_primitive_base_type(tp);

	if (base_type != NULL)
		write_type(env, base_type);

	write_type_common(env, tp);
	write_mode_ref(env, get_type_mode(tp));
	if (base_type == NULL)
		base_type = get_none_type();
	write_type_ref(env, base_type);
	fputc('\n', env->file);
}

static void write_type_compound(write_env_t *env, ir_type *tp)
{
	size_t n_members = get_compound_n_members(tp);
	size_t i;

	if (is_Class_type(tp)) {
		if (get_class_n_subtypes(tp) > 0 || get_class_n_supertypes(tp) > 0
		    || get_class_type_info(tp) != NULL || get_class_vtable_size(tp) > 0) {
			/* sub/superclass export not implemented yet, it's unclear wether
			 * class types will stay in libfirm anyway */
			panic("can't export class types yet");
		}
	}
	write_type_common(env, tp);
	write_ident_null(env, get_compound_ident(tp));
	fputc('\n', env->file);

	for (i = 0; i < n_members; ++i) {
		ir_entity *member = get_compound_member(tp, i);
		write_entity(env, member);
	}
}

static void write_type_array(write_env_t *env, ir_type *tp)
{
	size_t     n_dimensions   = get_array_n_dimensions(tp);
	ir_type   *element_type   = get_array_element_type(tp);
	ir_entity *element_entity = get_array_element_entity(tp);
	size_t   i;

	write_type(env, element_type);

	write_type_common(env, tp);
	write_size_t(env, n_dimensions);
	write_type_ref(env, get_array_element_type(tp));
	for (i = 0; i < n_dimensions; i++) {
		ir_node *lower = get_array_lower_bound(tp, i);
		ir_node *upper = get_array_upper_bound(tp, i);

		if (is_Const(lower))
			write_long(env, get_tarval_long(get_Const_tarval(lower)));
		else
			panic("Lower array bound is not constant");

		if (is_Const(upper))
			write_long(env, get_tarval_long(get_Const_tarval(upper)));
		else if (is_Unknown(upper))
			write_symbol(env, "unknown");
		else
			panic("Upper array bound is not constant");
	}
	/* note that we just write a reference to the element entity
	 * but never the entity itself */
	write_entity_ref(env, element_entity);
	fputc('\n', env->file);
}

static void write_type_method(write_env_t *env, ir_type *tp)
{
	size_t nparams  = get_method_n_params(tp);
	size_t nresults = get_method_n_ress(tp);
	size_t i;

	for (i = 0; i < nparams; i++)
		write_type(env, get_method_param_type(tp, i));
	for (i = 0; i < nresults; i++)
		write_type(env, get_method_res_type(tp, i));

	write_type_common(env, tp);
	write_unsigned(env, get_method_calling_convention(tp));
	write_unsigned(env, get_method_additional_properties(tp));
	write_size_t(env, nparams);
	write_size_t(env, nresults);
	for (i = 0; i < nparams; i++)
		write_type_ref(env, get_method_param_type(tp, i));
	for (i = 0; i < nresults; i++)
		write_type_ref(env, get_method_res_type(tp, i));
	write_unsigned(env, get_method_variadicity(tp));
	fputc('\n', env->file);
}

static void write_type_pointer(write_env_t *env, ir_type *tp)
{
	ir_type *points_to = get_pointer_points_to_type(tp);

	write_type(env, points_to);

	write_type_common(env, tp);
	write_mode_ref(env, get_type_mode(tp));
	write_type_ref(env, points_to);
	fputc('\n', env->file);
}

static void write_type_enumeration(write_env_t *env, ir_type *tp)
{
	write_type_common(env, tp);
	write_ident_null(env, get_enumeration_ident(tp));
	fputc('\n', env->file);
}

static void write_type(write_env_t *env, ir_type *tp)
{
	if (type_visited(tp))
		return;
	mark_type_visited(tp);

	switch ((tp_opcode)get_type_tpop_code(tp)) {
	case tpo_none:
	case tpo_unknown:
	case tpo_code:
	case tpo_uninitialized:
		/* no need to write special builtin types */
		return;

	case tpo_union:
	case tpo_struct:
	case tpo_class:
		write_type_compound(env, tp);
		return;

	case tpo_primitive:    write_type_primitive(env, tp);   return;
	case tpo_enumeration:  write_type_enumeration(env, tp); return;
	case tpo_method:       write_type_method(env, tp);      return;
	case tpo_pointer:      write_type_pointer(env, tp);     return;
	case tpo_array:        write_type_array(env, tp);       return;
	}
	panic("can't write invalid type %+F\n", tp);
}

static void write_entity(write_env_t *env, ir_entity *ent)
{
	ir_type       *type       = get_entity_type(ent);
	ir_type       *owner      = get_entity_owner(ent);
	ir_visibility  visibility = get_entity_visibility(ent);
	ir_linkage     linkage    = get_entity_linkage(ent);

	if (entity_visited(ent))
		return;
	mark_entity_visited(ent);

	write_type(env, type);
	write_type(env, owner);

	fputc('\t', env->file);
	switch ((ir_entity_kind)ent->entity_kind) {
	case IR_ENTITY_NORMAL:          write_symbol(env, "entity");          break;
	case IR_ENTITY_METHOD:          write_symbol(env, "method");          break;
	case IR_ENTITY_LABEL:           write_symbol(env, "label");           break;
	case IR_ENTITY_COMPOUND_MEMBER: write_symbol(env, "compound_member"); break;
	case IR_ENTITY_PARAMETER:       write_symbol(env, "parameter");       break;
	case IR_ENTITY_UNKNOWN:
		write_symbol(env, "unknown");
		write_long(env, get_entity_nr(ent));
		return;
	}
	write_long(env, get_entity_nr(ent));

	if (ent->entity_kind != IR_ENTITY_LABEL
	    && ent->entity_kind != IR_ENTITY_PARAMETER) {
		write_ident_null(env, get_entity_ident(ent));
		if (!entity_has_ld_ident(ent)) {
			write_ident_null(env, NULL);
		} else {
			write_ident_null(env, get_entity_ld_ident(ent));
		}
	}

	write_visibility(env, visibility);
	write_list_begin(env);
	if (linkage & IR_LINKAGE_CONSTANT)
		write_symbol(env, "constant");
	if (linkage & IR_LINKAGE_WEAK)
		write_symbol(env, "weak");
	if (linkage & IR_LINKAGE_GARBAGE_COLLECT)
		write_symbol(env, "garbage_collect");
	if (linkage & IR_LINKAGE_MERGE)
		write_symbol(env, "merge");
	if (linkage & IR_LINKAGE_HIDDEN_USER)
		write_symbol(env, "hidden_user");
	write_list_end(env);

	write_type_ref(env, type);
	if (ent->entity_kind != IR_ENTITY_LABEL)
		write_type_ref(env, owner);
	write_long(env, is_entity_compiler_generated(ent));
	write_volatility(env, get_entity_volatility(ent));

	switch ((ir_entity_kind)ent->entity_kind) {
	case IR_ENTITY_NORMAL:
		if (ent->initializer != NULL) {
			write_symbol(env, "initializer");
			write_initializer(env, get_entity_initializer(ent));
		} else if (entity_has_compound_ent_values(ent)) {
			/* compound graph API is deprecated */
			panic("exporting compound_graph initializers not supported");
		} else {
			write_symbol(env, "none");
		}
		break;
	case IR_ENTITY_COMPOUND_MEMBER:
		write_long(env, get_entity_offset(ent));
		write_unsigned(env, get_entity_offset_bits_remainder(ent));
		break;
	case IR_ENTITY_PARAMETER: {
		size_t num = get_entity_parameter_number(ent);
		if (num == IR_VA_START_PARAMETER_NUMBER) {
			write_symbol(env, "va_start");
		} else {
			write_size_t(env, num);
		}
		break;
	}
	case IR_ENTITY_UNKNOWN:
	case IR_ENTITY_LABEL:
	case IR_ENTITY_METHOD:
		break;
	}

	fputc('\n', env->file);
}

static void write_switch_table(write_env_t *env, const ir_switch_table *table)
{
	size_t n_entries = ir_switch_table_get_n_entries(table);
	size_t i;

	write_size_t(env, n_entries);
	for (i = 0; i < n_entries; ++i) {
		long       pn  = ir_switch_table_get_pn(table, i);
		ir_tarval *min = ir_switch_table_get_min(table, i);
		ir_tarval *max = ir_switch_table_get_max(table, i);
		write_long(env, pn);
		write_tarval(env, min);
		write_tarval(env, max);
	}
}

static void write_pred_refs(write_env_t *env, const ir_node *node, int from)
{
	int arity = get_irn_arity(node);
	int i;
	write_list_begin(env);
	assert(from <= arity);
	for (i = from; i < arity; ++i) {
		ir_node *pred = get_irn_n(node, i);
		write_node_ref(env, pred);
	}
	write_list_end(env);
}

static void write_node_nr(write_env_t *env, const ir_node *node)
{
	write_long(env, get_irn_node_nr(node));
}

static void write_ASM(write_env_t *env, const ir_node *node)
{
	ir_asm_constraint *input_constraints    = get_ASM_input_constraints(node);
	ir_asm_constraint *output_constraints   = get_ASM_output_constraints(node);
	ident            **clobbers             = get_ASM_clobbers(node);
	size_t             n_input_constraints  = get_ASM_n_input_constraints(node);
	size_t             n_output_constraints = get_ASM_n_output_constraints(node);
	size_t             n_clobbers           = get_ASM_n_clobbers(node);
	size_t             i;

	write_symbol(env, "ASM");
	write_node_nr(env, node);
	write_node_nr(env, get_nodes_block(node));

	write_ident(env, get_ASM_text(node));
	write_list_begin(env);
	for (i = 0; i < n_input_constraints; ++i) {
		const ir_asm_constraint *constraint = &input_constraints[i];
		write_unsigned(env, constraint->pos);
		write_ident(env, constraint->constraint);
		write_mode_ref(env, constraint->mode);
	}
	write_list_end(env);

	write_list_begin(env);
	for (i = 0; i < n_output_constraints; ++i) {
		const ir_asm_constraint *constraint = &output_constraints[i];
		write_unsigned(env, constraint->pos);
		write_ident(env, constraint->constraint);
		write_mode_ref(env, constraint->mode);
	}
	write_list_end(env);

	write_list_begin(env);
	for (i = 0; i < n_clobbers; ++i) {
		ident *clobber = clobbers[i];
		write_ident(env, clobber);
	}
	write_list_end(env);

	write_pin_state(env, get_irn_pinned(node));
	write_pred_refs(env, node, 0);
}

static void write_Phi(write_env_t *env, const ir_node *node)
{
	write_symbol(env, "Phi");
	write_node_nr(env, node);
	write_node_ref(env, get_nodes_block(node));
	write_mode_ref(env, get_irn_mode(node));
	write_pred_refs(env, node, 0);
}

static void write_Block(write_env_t *env, const ir_node *node)
{
	ir_entity *entity = get_Block_entity(node);

	if (entity != NULL) {
		write_symbol(env, "BlockL");
		write_node_nr(env, node);
		write_entity_ref(env, entity);
	} else {
		write_symbol(env, "Block");
		write_node_nr(env, node);
	}
	write_pred_refs(env, node, 0);
}

static void write_Anchor(write_env_t *env, const ir_node *node)
{
	write_symbol(env, "Anchor");
	write_node_nr(env, node);
	write_pred_refs(env, node, 0);
}

static void write_SymConst(write_env_t *env, const ir_node *node)
{
	/* TODO: only symconst_addr_ent implemented yet */
	if (get_SymConst_kind(node) != symconst_addr_ent)
		panic("Can't export %+F (only symconst_addr_ent supported)", node);

	write_symbol(env, "SymConst");
	write_node_nr(env, node);
	write_mode_ref(env, get_irn_mode(node));
	write_entity_ref(env, get_SymConst_entity(node));
}

typedef void (*write_node_func)(write_env_t *env, const ir_node *node);

static void register_node_writer(ir_op *op, write_node_func func)
{
	set_generic_function_ptr(op, (op_func)func);
}

static void writers_init(void)
{
	ir_clear_opcodes_generic_func();
	register_node_writer(op_Anchor,   write_Anchor);
	register_node_writer(op_ASM,      write_ASM);
	register_node_writer(op_Block,    write_Block);
	register_node_writer(op_Phi,      write_Phi);
	register_node_writer(op_SymConst, write_SymConst);
	register_generated_node_writers();
}

static void write_node(const ir_node *node, write_env_t *env)
{
	ir_op          *op   = get_irn_op(node);
	write_node_func func = (write_node_func) get_generic_function_ptr(op);

	fputc('\t', env->file);
	if (func == NULL)
		panic("No write_node_func for %+F", node);
	func(env, node);
	fputc('\n', env->file);
}

static void write_node_recursive(ir_node *node, write_env_t *env);

static void write_preds(ir_node *node, write_env_t *env)
{
	int arity = get_irn_arity(node);
	int i;
	for (i = 0; i < arity; ++i) {
		ir_node *pred = get_irn_n(node, i);
		write_node_recursive(pred, env);
	}
}

/**
 * Recursively write nodes.
 * The reader expects nodes in a way that except for block/phi/anchor nodes
 * all predecessors are already defined when we reach them. So usually we
 * recurse to all our predecessors except for block/phi/anchor nodes where
 * we put the predecessors into a queue for later processing.
 */
static void write_node_recursive(ir_node *node, write_env_t *env)
{
	if (irn_visited_else_mark(node))
		return;

	if (!is_Block(node)) {
		write_node_recursive(get_nodes_block(node), env);
	}
	/* write predecessors */
	if (!is_Phi(node) && !is_Block(node) && !is_Anchor(node)) {
		write_preds(node, env);
	} else {
		int arity = get_irn_arity(node);
		int i;
		for (i = 0; i < arity; ++i) {
			ir_node *pred = get_irn_n(node, i);
			pdeq_putr(env->write_queue, pred);
		}
	}
	write_node(node, env);
}

static void write_mode(write_env_t *env, ir_mode *mode)
{
	if (mode_is_int(mode)) {
		write_symbol(env, "int_mode");
		write_string(env, get_mode_name(mode));
		write_mode_arithmetic(env, get_mode_arithmetic(mode));
		write_unsigned(env, get_mode_size_bits(mode));
		write_int(env, get_mode_sign(mode));
		write_unsigned(env, get_mode_modulo_shift(mode));
	} else if (mode_is_reference(mode)) {
		write_symbol(env, "reference_mode");
		write_string(env, get_mode_name(mode));
		write_mode_arithmetic(env, get_mode_arithmetic(mode));
		write_unsigned(env, get_mode_size_bits(mode));
		write_unsigned(env, get_mode_modulo_shift(mode));

		write_mode_ref(env, get_reference_mode_signed_eq(mode));
		write_mode_ref(env, get_reference_mode_unsigned_eq(mode));
		write_int(env, (mode == mode_P ? 1 : 0));
	} else if (mode_is_float(mode)) {
		write_symbol(env, "float_mode");
		write_string(env, get_mode_name(mode));
		write_mode_arithmetic(env, get_mode_arithmetic(mode));
		write_unsigned(env, get_mode_exponent_size(mode));
		write_unsigned(env, get_mode_mantissa_size(mode));
	} else {
		panic("Can't write internal modes");
	}
}

static void write_modes(write_env_t *env)
{
	size_t n_modes = ir_get_n_modes();
	size_t i;

	write_symbol(env, "modes");
	fputs("{\n", env->file);

	for (i = 0; i < n_modes; i++) {
		ir_mode *mode = ir_get_mode(i);
		if (!mode_is_int(mode) && !mode_is_reference(mode)
		    && !mode_is_float(mode)) {
		    /* skip internal modes */
		    continue;
		}
		fputc('\t', env->file);
		write_mode(env, mode);
		fputc('\n', env->file);
	}

	fputs("}\n\n", env->file);
}

static void write_program(write_env_t *env)
{
	ir_segment_t s;
	size_t n_asms = get_irp_n_asms();
	size_t i;

	write_symbol(env, "program");
	write_scope_begin(env);
	if (irp_prog_name_is_set()) {
		fputc('\t', env->file);
		write_symbol(env, "name");
		write_string(env, get_irp_name());
		fputc('\n', env->file);
	}

	for (s = IR_SEGMENT_FIRST; s <= IR_SEGMENT_LAST; ++s) {
		ir_type *segment_type = get_segment_type(s);
		fputc('\t', env->file);
		write_symbol(env, "segment_type");
		write_symbol(env, get_segment_name(s));
		if (segment_type == NULL) {
			write_symbol(env, "NULL");
		} else {
			write_type_ref(env, segment_type);
		}
		fputc('\n', env->file);
	}

	for (i = 0; i < n_asms; ++i) {
		ident *asm_text = get_irp_asm(i);
		fputc('\t', env->file);
		write_symbol(env, "asm");
		write_ident(env, asm_text);
		fputc('\n', env->file);
	}
	write_scope_end(env);
}

int ir_export(const char *filename)
{
	FILE *file = fopen(filename, "wt");
	int   res  = 0;
	if (file == NULL) {
		perror(filename);
		return 1;
	}

	ir_export_file(file);
	res = ferror(file);
	fclose(file);
	return res;
}

static void write_node_cb(ir_node *node, void *ctx)
{
	write_env_t *env = (write_env_t*)ctx;
	write_node(node, env);
}

static void write_typegraph(write_env_t *env)
{
	size_t n_types = get_irp_n_types();
	size_t i;

	write_symbol(env, "typegraph");
	write_scope_begin(env);
	irp_reserve_resources(irp, IRP_RESOURCE_TYPE_VISITED);
	inc_master_type_visited();
	for (i = 0; i < n_types; ++i) {
		ir_type *type = get_irp_type(i);
		write_type(env, type);
	}
	irp_free_resources(irp, IRP_RESOURCE_TYPE_VISITED);
	write_scope_end(env);
}

static void write_irg(write_env_t *env, ir_graph *irg)
{
	write_symbol(env, "irg");
	write_entity_ref(env, get_irg_entity(irg));
	write_type_ref(env, get_irg_frame_type(irg));
	write_inline_property(env, get_irg_inline_property(irg));
	write_unsigned(env, get_irg_additional_properties(irg));
	write_scope_begin(env);
	ir_reserve_resources(irg, IR_RESOURCE_IRN_VISITED);
	inc_irg_visited(irg);
	assert(pdeq_empty(env->write_queue));
	pdeq_putr(env->write_queue, irg->anchor);
	do {
		ir_node *node = (ir_node*) pdeq_getl(env->write_queue);
		write_node_recursive(node, env);
	} while (!pdeq_empty(env->write_queue));
	ir_free_resources(irg, IR_RESOURCE_IRN_VISITED);
	write_scope_end(env);
}

/* Exports the whole irp to the given file in a textual form. */
void ir_export_file(FILE *file)
{
	write_env_t my_env;
	write_env_t *env = &my_env;
	size_t i, n_irgs = get_irp_n_irgs();

	memset(env, 0, sizeof(*env));
	env->file        = file;
	env->write_queue = new_pdeq();

	writers_init();
	write_modes(env);

	write_typegraph(env);

	for (i = 0; i < n_irgs; i++) {
		ir_graph *irg = get_irp_irg(i);
		write_irg(env, irg);
	}

	write_symbol(env, "constirg");
	write_node_ref(env, get_const_code_irg()->current_block);
	write_scope_begin(env);
	walk_const_code(NULL, write_node_cb, env);
	write_scope_end(env);

	write_program(env);

	del_pdeq(env->write_queue);
}



static void read_c(read_env_t *env)
{
	int c = fgetc(env->file);
	env->c = c;
	if (c == '\n')
		env->line++;
}

/** Returns the first non-whitespace character or EOF. **/
static void skip_ws(read_env_t *env)
{
	while (true) {
		switch (env->c) {
		case ' ':
		case '\t':
		case '\n':
		case '\r':
			read_c(env);
			continue;

		default:
			return;
		}
	}
}

static void skip_to(read_env_t *env, char to_ch)
{
	while (env->c != to_ch && env->c != EOF) {
		read_c(env);
	}
}

static bool expect_char(read_env_t *env, char ch)
{
	skip_ws(env);
	if (env->c != ch) {
		parse_error(env, "Unexpected char '%c', expected '%c'\n",
		            env->c, ch);
		return false;
	}
	read_c(env);
	return true;
}

#define EXPECT(c) if (expect_char(env, (c))) {} else return

static char *read_word(read_env_t *env)
{
	skip_ws(env);

	assert(obstack_object_size(&env->obst) == 0);
	while (true) {
		int c = env->c;
		switch (c) {
		case EOF:
		case ' ':
		case '\t':
		case '\n':
		case '\r':
			goto endofword;

		default:
			obstack_1grow(&env->obst, c);
			break;
		}
		read_c(env);
	}

endofword:
	obstack_1grow(&env->obst, '\0');
	return (char*)obstack_finish(&env->obst);
}

static char *read_string(read_env_t *env)
{
	skip_ws(env);
	if (env->c != '"') {
		parse_error(env, "Expected string, got '%c'\n", env->c);
		exit(1);
	}
	read_c(env);

	assert(obstack_object_size(&env->obst) == 0);
	while (env->c != '"') {
		if (env->c == EOF) {
			parse_error(env, "Unexpected EOF while parsing string\n");
			exit(1);
		}

		if (env->c == '\\') {
			read_c(env);
			switch (env->c) {
			case 'n':
				obstack_1grow(&env->obst, '\n');
				break;
			case '"':
			case '\\':
				obstack_1grow(&env->obst, env->c);
				break;
			default:
				parse_error(env, "Unknown escape sequence '\\%c'\n", env->c);
				exit(1);
			}
		} else {
			obstack_1grow(&env->obst, env->c);
		}
		read_c(env);
	}
	read_c(env);
	obstack_1grow(&env->obst, 0);

	return (char*)obstack_finish(&env->obst);
}

static ident *read_ident(read_env_t *env)
{
	char  *str = read_string(env);
	ident *res = new_id_from_str(str);
	obstack_free(&env->obst, str);
	return res;
}

static ident *read_symbol(read_env_t *env)
{
	char  *str = read_word(env);
	ident *res = new_id_from_str(str);
	obstack_free(&env->obst, str);
	return res;
}

/*
 * reads a "quoted string" or alternatively the token NULL
 */
static char *read_string_null(read_env_t *env)
{
	skip_ws(env);
	if (env->c == 'N') {
		char *str = read_word(env);
		if (strcmp(str, "NULL") == 0) {
			obstack_free(&env->obst, str);
			return NULL;
		}
	} else if (env->c == '"') {
		return read_string(env);
	}

	parse_error(env, "Expected \"string\" or NULL\n");
	exit(1);
}

static ident *read_ident_null(read_env_t *env)
{
	ident *res;
	char  *str = read_string_null(env);
	if (str == NULL)
		return NULL;

	res = new_id_from_str(str);
	obstack_free(&env->obst, str);
	return res;
}

static long read_long(read_env_t *env)
{
	long  result;
	char *str;

	skip_ws(env);
	if (!isdigit(env->c) && env->c != '-') {
		parse_error(env, "Expected number, got '%c'\n", env->c);
		exit(1);
	}

	assert(obstack_object_size(&env->obst) == 0);
	do {
		obstack_1grow(&env->obst, env->c);
		read_c(env);
	} while (isdigit(env->c));
	obstack_1grow(&env->obst, 0);

	str = (char*)obstack_finish(&env->obst);
	result = atol(str);
	obstack_free(&env->obst, str);

	return result;
}

static int read_int(read_env_t *env)
{
	return (int) read_long(env);
}

static unsigned read_unsigned(read_env_t *env)
{
	return (unsigned) read_long(env);
}

static size_t read_size_t(read_env_t *env)
{
	/* FIXME */
	return (size_t) read_unsigned(env);
}

static void expect_list_begin(read_env_t *env)
{
	skip_ws(env);
	if (env->c != '[') {
		parse_error(env, "Expected list, got '%c'\n", env->c);
		exit(1);
	}
	read_c(env);
}

static bool list_has_next(read_env_t *env)
{
	if (feof(env->file)) {
		parse_error(env, "Unexpected EOF while reading list");
		exit(1);
	}
	skip_ws(env);
	if (env->c == ']') {
		read_c(env);
		return false;
	}

	return true;
}

static void *get_id(read_env_t *env, long id)
{
	id_entry key, *entry;
	key.id = id;

	entry = (id_entry*)set_find(env->idset, &key, sizeof(key), (unsigned) id);
	return entry ? entry->elem : NULL;
}

static void set_id(read_env_t *env, long id, void *elem)
{
	id_entry key;
	key.id = id;
	key.elem = elem;
	set_insert(env->idset, &key, sizeof(key), (unsigned) id);
}

static ir_node *get_node_or_null(read_env_t *env, long nodenr)
{
	ir_node *node = (ir_node *) get_id(env, nodenr);
	if (node && node->kind != k_ir_node) {
		parse_error(env, "Irn ID %ld collides with something else\n",
		            nodenr);
		return NULL;
	}
	return node;
}

static ir_type *get_type(read_env_t *env, long typenr)
{
	ir_type *type = (ir_type *) get_id(env, typenr);
	if (type == NULL) {
		parse_error(env, "Type %ld not defined (yet?)\n", typenr);
		return get_unknown_type();
	}
	if (type->kind != k_type) {
		parse_error(env, "Object %ld is not a type (but should be)\n", typenr);
		return get_unknown_type();
	}
	return type;
}

static ir_type *read_type_ref(read_env_t *env)
{
	char *str = read_word(env);
	if (strcmp(str, "none") == 0) {
		obstack_free(&env->obst, str);
		return get_none_type();
	}
	if (strcmp(str, "unknown") == 0) {
		obstack_free(&env->obst, str);
		return get_unknown_type();
	}
	if (strcmp(str, "code") == 0) {
		obstack_free(&env->obst, str);
		return get_code_type();
	}
	long nr = atol(str);
	obstack_free(&env->obst, str);

	return get_type(env, nr);
}

static ir_entity *create_error_entity(void)
{
	ir_entity *res = new_entity(get_glob_type(), new_id_from_str("error"),
	                            get_unknown_type());
	return res;
}

static ir_entity *get_entity(read_env_t *env, long entnr)
{
	ir_entity *entity = (ir_entity *) get_id(env, entnr);
	if (entity == NULL) {
		parse_error(env, "unknown entity: %ld\n", entnr);
		return create_error_entity();
	}
	if (entity->kind != k_entity) {
		parse_error(env, "Object %ld is not an entity (but should be)\n",
		            entnr);
		return create_error_entity();
	}

	return entity;
}

static ir_entity *read_entity_ref(read_env_t *env)
{
	long nr = read_long(env);
	return get_entity(env, nr);
}

static ir_mode *read_mode_ref(read_env_t *env)
{
	char  *str = read_string(env);
	size_t n   = ir_get_n_modes();
	size_t i;

	for (i = 0; i < n; i++) {
		ir_mode *mode = ir_get_mode(i);
		if (strcmp(str, get_mode_name(mode)) == 0) {
			obstack_free(&env->obst, str);
			return mode;
		}
	}

	parse_error(env, "unknown mode \"%s\"\n", str);
	return mode_ANY;
}

static const char *get_typetag_name(typetag_t typetag)
{
	switch (typetag) {
	case tt_align:               return "align";
	case tt_builtin_kind:        return "builtin kind";
	case tt_cond_jmp_predicate:  return "cond_jmp_predicate";
	case tt_initializer:         return "initializer kind";
	case tt_irg_inline_property: return "irg_inline_property";
	case tt_keyword:             return "keyword";
	case tt_linkage:             return "linkage";
	case tt_mode_arithmetic:     return "mode_arithmetic";
	case tt_pin_state:           return "pin state";
	case tt_segment:             return "segment";
	case tt_throws:              return "throws";
	case tt_tpo:                 return "type";
	case tt_type_state:          return "type state";
	case tt_visibility:          return "visibility";
	case tt_volatility:          return "volatility";
	case tt_where_alloc:         return "where alloc";
	}
	return "<UNKNOWN>";
}

/**
 * Read and decode an enum constant.
 */
static unsigned read_enum(read_env_t *env, typetag_t typetag)
{
	char     *str  = read_word(env);
	unsigned  code = symbol(str, typetag);

	if (code != SYMERROR) {
		obstack_free(&env->obst, str);
		return code;
	}

	parse_error(env, "invalid %s: \"%s\"\n", get_typetag_name(typetag), str);
	return 0;
}

static ir_align read_align(read_env_t *env)
{
	return (ir_align)read_enum(env, tt_align);
}

static ir_builtin_kind read_builtin_kind(read_env_t *env)
{
	return (ir_builtin_kind)read_enum(env, tt_builtin_kind);
}

static cond_jmp_predicate read_cond_jmp_predicate(read_env_t *env)
{
	return (cond_jmp_predicate)read_enum(env, tt_cond_jmp_predicate);
}

static ir_initializer_kind_t read_initializer_kind(read_env_t *env)
{
	return (ir_initializer_kind_t)read_enum(env, tt_initializer);
}

static ir_mode_arithmetic read_mode_arithmetic(read_env_t *env)
{
	return (ir_mode_arithmetic)read_enum(env, tt_mode_arithmetic);
}

static op_pin_state read_pin_state(read_env_t *env)
{
	return (op_pin_state)read_enum(env, tt_pin_state);
}

static ir_type_state read_type_state(read_env_t *env)
{
	return (ir_type_state)read_enum(env, tt_type_state);
}

static ir_visibility read_visibility(read_env_t *env)
{
	return (ir_visibility)read_enum(env, tt_visibility);
}

static ir_linkage read_linkage(read_env_t *env)
{
	return (ir_linkage)read_enum(env, tt_linkage);
}

static ir_volatility read_volatility(read_env_t *env)
{
	return (ir_volatility)read_enum(env, tt_volatility);
}

static ir_where_alloc read_where_alloc(read_env_t *env)
{
	return (ir_where_alloc)read_enum(env, tt_where_alloc);
}

static bool read_throws(read_env_t *env)
{
	return (bool)read_enum(env, tt_throws);
}

static keyword_t read_keyword(read_env_t *env)
{
	return (keyword_t)read_enum(env, tt_keyword);
}

static irg_inline_property read_inline_property(read_env_t *env)
{
	return (irg_inline_property)read_enum(env, tt_irg_inline_property);
}

static ir_relation read_relation(read_env_t *env)
{
	return (ir_relation)read_long(env);
}

static ir_tarval *read_tarval(read_env_t *env)
{
	ir_mode   *tvmode = read_mode_ref(env);
	char      *str    = read_word(env);
	ir_tarval *tv;
	if (strcmp(str, "bad") == 0)
		return tarval_bad;
	tv = new_tarval_from_str(str, strlen(str), tvmode);
	if (tv == tarval_bad)
		parse_error(env, "problem while parsing tarval '%s'\n", str);
	obstack_free(&env->obst, str);

	return tv;
}

static ir_switch_table *read_switch_table(read_env_t *env)
{
	size_t           n_entries = read_size_t(env);
	ir_switch_table *table     = ir_new_switch_table(env->irg, n_entries);
	size_t           i;

	for (i = 0; i < n_entries; ++i) {
		long       pn  = read_long(env);
		ir_tarval *min = read_tarval(env);
		ir_tarval *max = read_tarval(env);
		ir_switch_table_set(table, i, min, max, pn);
	}
	return table;
}

static ir_initializer_t *read_initializer(read_env_t *env)
{
	ir_initializer_kind_t ini_kind = read_initializer_kind(env);

	switch (ini_kind) {
	case IR_INITIALIZER_CONST: {
		long nr = read_long(env);
		ir_node *node = get_node_or_null(env, nr);
		ir_initializer_t *initializer = create_initializer_const(node);
		if (node == NULL) {
			delayed_initializer_t di;
			di.initializer = initializer;
			di.node_nr     = nr;
			ARR_APP1(delayed_initializer_t, env->delayed_initializers, di);
		}
		return initializer;
	}

	case IR_INITIALIZER_TARVAL:
		return create_initializer_tarval(read_tarval(env));

	case IR_INITIALIZER_NULL:
		return get_initializer_null();

	case IR_INITIALIZER_COMPOUND: {
		size_t i, n = read_size_t(env);
		ir_initializer_t *ini = create_initializer_compound(n);
		for (i = 0; i < n; i++) {
			ir_initializer_t *curini = read_initializer(env);
			set_initializer_compound_value(ini, i, curini);
		}
		return ini;
	}
	}

	panic("Unknown initializer kind");
}

/** Reads a type description and remembers it by its id. */
static void read_type(read_env_t *env)
{
	long           typenr = read_long(env);
	tp_opcode      tpop   = (tp_opcode) read_enum(env, tt_tpo);
	unsigned       size   = (unsigned) read_long(env);
	unsigned       align  = (unsigned) read_long(env);
	ir_type_state  state  = read_type_state(env);
	unsigned       flags  = (unsigned) read_long(env);
	ir_type       *type;

	switch (tpop) {
	case tpo_array: {
		size_t     n_dimensions = read_size_t(env);
		ir_type   *elemtype     = read_type_ref(env);
		size_t     i;
		ir_entity *element_entity;
		long       element_entity_nr;

		type = new_type_array(n_dimensions, elemtype);
		for (i = 0; i < n_dimensions; i++) {
			char *str = read_word(env);
			if (strcmp(str, "unknown") != 0) {
				long lowerbound = atol(str);
				set_array_lower_bound_int(type, i, lowerbound);
			}
			obstack_free(&env->obst, str);

			str = read_word(env);
			if (strcmp(str, "unknown") != 0) {
				long upperbound = atol(str);
				set_array_upper_bound_int(type, i, upperbound);
			}
			obstack_free(&env->obst, str);
		}

		element_entity_nr = read_long(env);
		element_entity = get_array_element_entity(type);
		set_id(env, element_entity_nr, element_entity);

		set_type_size_bytes(type, size);
		goto finish_type;
	}

	case tpo_class: {
		ident *id = read_ident_null(env);

		if (typenr == (long) IR_SEGMENT_GLOBAL)
			type = get_glob_type();
		else
			type = new_type_class(id);
		set_type_size_bytes(type, size);
		goto finish_type;
	}

	case tpo_method: {
		unsigned                  callingconv = read_unsigned(env);
		mtp_additional_properties addprops
			= (mtp_additional_properties) read_long(env);
		size_t nparams  = read_size_t(env);
		size_t nresults = read_size_t(env);
		size_t i;
		int    variadicity;

		type = new_type_method(nparams, nresults);

		for (i = 0; i < nparams; i++) {
			long ptypenr = read_long(env);
			ir_type *paramtype = get_type(env, ptypenr);

			set_method_param_type(type, i, paramtype);
		}
		for (i = 0; i < nresults; i++) {
			long ptypenr = read_long(env);
			ir_type *restype = get_type(env, ptypenr);

			set_method_res_type(type, i, restype);
		}

		variadicity = (int) read_long(env);
		set_method_variadicity(type, variadicity);

		set_method_calling_convention(type, callingconv);
		set_method_additional_properties(type, addprops);
		goto finish_type;
	}

	case tpo_pointer: {
		ir_mode *mode     = read_mode_ref(env);
		ir_type *pointsto = get_type(env, read_long(env));
		type = new_type_pointer(pointsto);
		set_type_mode(type, mode);
		goto finish_type;
	}

	case tpo_primitive: {
		ir_mode *mode = read_mode_ref(env);
		ir_type *base_type = read_type_ref(env);
		type = new_type_primitive(mode);
		if (base_type != get_none_type()) {
			set_primitive_base_type(type, base_type);
		}
		goto finish_type;
	}

	case tpo_struct: {
		ident *id = read_ident_null(env);
		type = new_type_struct(id);
		set_type_size_bytes(type, size);
		goto finish_type;
	}

	case tpo_union: {
		ident *id = read_ident_null(env);
		type = new_type_union(id);
		set_type_size_bytes(type, size);
		goto finish_type;
	}

	case tpo_none:
	case tpo_code:
	case tpo_unknown:
	case tpo_enumeration:
	case tpo_uninitialized:
		parse_error(env, "can't import this type kind (%d)", tpop);
		return;
	}
	parse_error(env, "unknown type kind: \"%d\"\n", tpop);
	skip_to(env, '\n');
	return;

finish_type:
	set_type_alignment_bytes(type, align);
	type->flags = flags;

	if (state == layout_fixed)
		ARR_APP1(ir_type *, env->fixedtypes, type);

	set_id(env, typenr, type);
}

static void read_unknown_entity(read_env_t *env)
{
	long       entnr  = read_long(env);
	ir_entity *entity = get_unknown_entity();
	set_id(env, entnr, entity);
}

/** Reads an entity description and remembers it by its id. */
static void read_entity(read_env_t *env, ir_entity_kind kind)
{
	long           entnr      = read_long(env);
	ident         *name       = NULL;
	ident         *ld_name    = NULL;
	ir_visibility  visibility = ir_visibility_default;
	ir_linkage     linkage    = IR_LINKAGE_DEFAULT;
	ir_type       *owner      = NULL;
	ir_entity     *entity     = NULL;
	int            compiler_generated;
	ir_volatility  volatility;
	const char    *str;
	ir_type       *type;

	if (kind != IR_ENTITY_LABEL && kind != IR_ENTITY_PARAMETER) {
		name    = read_ident(env);
		ld_name = read_ident_null(env);
	}

	visibility = read_visibility(env);
	expect_list_begin(env);
	while (list_has_next(env)) {
		linkage |= read_linkage(env);
	}

	type = read_type_ref(env);
	if (kind != IR_ENTITY_LABEL)
		owner = read_type_ref(env);

	compiler_generated = read_long(env) != 0;
	volatility         = read_volatility(env);

	switch (kind) {
	case IR_ENTITY_NORMAL:
		entity = new_entity(owner, name, type);
		if (ld_name != NULL)
			set_entity_ld_ident(entity, ld_name);
		str = read_word(env);
		if (strcmp(str, "initializer") == 0) {
			ir_initializer_t *initializer = read_initializer(env);
			if (initializer != NULL)
				set_entity_initializer(entity, initializer);
		} else if (strcmp(str, "none") == 0) {
			/* do nothing */
		} else {
			parse_error(env, "expected 'initializer' or 'none' got '%s'\n", str);
		}
		break;
	case IR_ENTITY_COMPOUND_MEMBER:
		entity = new_entity(owner, name, type);
		if (ld_name != NULL)
			set_entity_ld_ident(entity, ld_name);
		set_entity_offset(entity, (int) read_long(env));
		set_entity_offset_bits_remainder(entity, (unsigned char) read_long(env));
		break;
	case IR_ENTITY_METHOD:
		entity = new_entity(owner, name, type);
		if (ld_name != NULL)
			set_entity_ld_ident(entity, ld_name);
		break;
	case IR_ENTITY_PARAMETER: {
		char  *str = read_word(env);
		size_t parameter_number;
		if (strcmp(str, "va_start") == 0) {
			parameter_number = IR_VA_START_PARAMETER_NUMBER;
		} else {
			parameter_number = atol(str);
		}
		obstack_free(&env->obst, str);
		entity = new_parameter_entity(owner, parameter_number, type);
		break;
	}
	case IR_ENTITY_LABEL: {
		ir_label_t nr = get_irp_next_label_nr();
		entity = new_label_entity(nr);
		break;
	case IR_ENTITY_UNKNOWN:
		panic("read_entity with IR_ENTITY_UNKNOWN?");
	}
	}

	set_entity_compiler_generated(entity, compiler_generated);
	set_entity_volatility(entity, volatility);
	set_entity_visibility(entity, visibility);
	set_entity_linkage(entity, linkage);

	if (owner != NULL && is_Array_type(owner)) {
		set_array_element_entity(owner, entity);
	}

	set_id(env, entnr, entity);
}

/** Parses the whole type graph. */
static void read_typegraph(read_env_t *env)
{
	ir_graph *old_irg = env->irg;

	EXPECT('{');

	env->irg = get_const_code_irg();

	/* parse all types first */
	while (true) {
		keyword_t kwkind;
		skip_ws(env);
		if (env->c == '}') {
			read_c(env);
			break;
		}

		kwkind = read_keyword(env);
		switch (kwkind) {
		case kw_type:
			read_type(env);
			break;

		case kw_entity:
			read_entity(env, IR_ENTITY_NORMAL);
			break;
		case kw_label:
			read_entity(env, IR_ENTITY_LABEL);
			break;
		case kw_method:
			read_entity(env, IR_ENTITY_METHOD);
			break;
		case kw_compound_member:
			read_entity(env, IR_ENTITY_COMPOUND_MEMBER);
			break;
		case kw_parameter:
			read_entity(env, IR_ENTITY_PARAMETER);
			break;
		case kw_unknown:
			read_unknown_entity(env);
			break;
		default:
			parse_error(env, "type graph element not supported yet: %d\n", kwkind);
			skip_to(env, '\n');
			break;
		}
	}
	env->irg = old_irg;
}

/**
 * Read a node reference and return the node for it. This assumes that the node
 * was previously read. This is fine for all normal nodes.
 * (Note: that we "break" loops by having special code for phi, block or anchor
 *  nodes in place, firm guarantees us that a loop in the graph always contains
 *  a phi, block or anchor node)
 */
static ir_node *read_node_ref(read_env_t *env)
{
	long     nr   = read_long(env);
	ir_node *node = get_node_or_null(env, nr);
	if (node == NULL) {
		parse_error(env, "node %ld not defined (yet?)\n", nr);
		return new_r_Bad(env->irg, mode_ANY);
	}
	return node;
}

static int read_preds(read_env_t *env)
{
	expect_list_begin(env);
	assert(obstack_object_size(&env->preds_obst) == 0);
	while (list_has_next(env)) {
		ir_node *pred = read_node_ref(env);
		obstack_grow(&env->preds_obst, &pred, sizeof(pred));
	}
	return obstack_object_size(&env->preds_obst) / sizeof(ir_node*);
}

static void read_preds_delayed(read_env_t *env, ir_node *node)
{
	int             n_preds = 0;
	delayed_pred_t *d;

	expect_list_begin(env);
	assert(obstack_object_size(&env->preds_obst) == 0);
	obstack_blank(&env->preds_obst, sizeof(delayed_pred_t));
	while (list_has_next(env)) {
		long pred_nr = read_long(env);
		obstack_grow(&env->preds_obst, &pred_nr, sizeof(pred_nr));
		++n_preds;
	}
	d          = (delayed_pred_t*) obstack_finish(&env->preds_obst);
	d->node    = node;
	d->n_preds = n_preds;

	ARR_APP1(const delayed_pred_t*, env->delayed_preds, d);
}

static ir_node *read_ASM(read_env_t *env)
{
	int                n_in;
	ir_node          **in;
	ir_node           *newnode;
	ir_asm_constraint *input_constraints  = NEW_ARR_F(ir_asm_constraint, 0);
	ir_asm_constraint *output_constraints = NEW_ARR_F(ir_asm_constraint, 0);
	ident            **clobbers           = NEW_ARR_F(ident*, 0);
	ir_node           *block              = read_node_ref(env);
	op_pin_state       pin_state;

	ident *asm_text = read_ident(env);

	expect_list_begin(env);
	while (list_has_next(env)) {
		ir_asm_constraint constraint;
		constraint.pos        = read_unsigned(env);
		constraint.constraint = read_ident(env);
		constraint.mode       = read_mode_ref(env);
		ARR_APP1(ir_asm_constraint, input_constraints, constraint);
	}

	expect_list_begin(env);
	while (list_has_next(env)) {
		ir_asm_constraint constraint;
		constraint.pos        = read_unsigned(env);
		constraint.constraint = read_ident(env);
		constraint.mode       = read_mode_ref(env);
		ARR_APP1(ir_asm_constraint, output_constraints, constraint);
	}

	expect_list_begin(env);
	while (list_has_next(env)) {
		ident *clobber = read_ident(env);
		ARR_APP1(ident*, clobbers, clobber);
	}

	pin_state = read_pin_state(env);

	n_in = read_preds(env);
	in   = obstack_finish(&env->preds_obst);

	if (ARR_LEN(input_constraints) != (size_t)n_in) {
		parse_error(env, "input_constraints != n_in in ir file");
		return new_r_Bad(env->irg, mode_T);
	}

	newnode = new_r_ASM(block, n_in, in,
		input_constraints, ARR_LEN(output_constraints),
		output_constraints, ARR_LEN(clobbers),
		clobbers, asm_text);
	set_irn_pinned(newnode, pin_state);
	obstack_free(&env->preds_obst, in);
	DEL_ARR_F(clobbers);
	DEL_ARR_F(output_constraints);
	DEL_ARR_F(input_constraints);
	return newnode;
}

static ir_node *read_Phi(read_env_t *env)
{
	ir_node *block = read_node_ref(env);
	ir_mode *mode  = read_mode_ref(env);
	ir_node *res   = new_r_Phi(block, 0, NULL, mode);
	read_preds_delayed(env, res);
	return res;
}

static ir_node *read_Block(read_env_t *env)
{
	ir_node *res = new_r_Block(env->irg, 0, NULL);
	read_preds_delayed(env, res);
	return res;
}

static ir_node *read_labeled_Block(read_env_t *env)
{
	ir_node   *res    = new_r_Block(env->irg, 0, NULL);
	ir_entity *entity = read_entity_ref(env);
	read_preds_delayed(env, res);
	set_Block_entity(res, entity);
	return res;
}

static ir_node *read_SymConst(read_env_t *env)
{
	ir_mode   *mode   = read_mode_ref(env);
	ir_entity *entity = read_entity_ref(env);
	ir_node   *res;
	symconst_symbol sym;

	sym.entity_p = entity;
	res = new_r_SymConst(env->irg, mode, sym, symconst_addr_ent);
	return res;
}

static ir_node *read_Anchor(read_env_t *env)
{
	ir_node *res = new_r_Anchor(env->irg);
	read_preds_delayed(env, res);
	return res;
}

typedef ir_node* (*read_node_func)(read_env_t *env);
static pmap *node_readers;

static void register_node_reader(ident *ident, read_node_func func)
{
	pmap_insert(node_readers, ident, func);
}

static ir_node *read_node(read_env_t *env)
{
	ident         *id   = read_symbol(env);
	read_node_func func = pmap_get(node_readers, id);
	long           nr   = read_long(env);
	ir_node       *res;
	if (func == NULL) {
		parse_error(env, "Unknown nodetype '%s'", get_id_str(id));
		skip_to(env, '\n');
		res = new_r_Bad(env->irg, mode_ANY);
	} else {
		res = func(env);
	}
	set_id(env, nr, res);
	return res;
}

static void readers_init(void)
{
	assert(node_readers == NULL);
	node_readers = pmap_create();
	register_node_reader(new_id_from_str("Anchor"),   read_Anchor);
	register_node_reader(new_id_from_str("ASM"),      read_ASM);
	register_node_reader(new_id_from_str("Block"),    read_Block);
	register_node_reader(new_id_from_str("BlockL"),   read_labeled_Block);
	register_node_reader(new_id_from_str("Phi"),      read_Phi);
	register_node_reader(new_id_from_str("SymConst"), read_SymConst);
	register_generated_node_readers();
}

static void read_graph(read_env_t *env, ir_graph *irg)
{
	size_t n_delayed_preds;
	size_t i;
	env->irg = irg;

	env->delayed_preds = NEW_ARR_F(const delayed_pred_t*, 0);

	EXPECT('{');
	while (true) {
		skip_ws(env);
		if (env->c == '}' || env->c == EOF) {
			read_c(env);
			break;
		}

		read_node(env);
	}

	/* resolve delayed preds */
	n_delayed_preds = ARR_LEN(env->delayed_preds);
	for (i = 0; i < n_delayed_preds; ++i) {
		const delayed_pred_t *dp  = env->delayed_preds[i];
		ir_node             **ins = ALLOCAN(ir_node*, dp->n_preds);
		int                   i;
		for (i = 0; i < dp->n_preds; ++i) {
			long     pred_nr = dp->preds[i];
			ir_node *pred    = get_node_or_null(env, pred_nr);
			if (pred == NULL) {
				parse_error(env, "predecessor %ld of a node not defined\n",
				            pred_nr);
				goto next_delayed_pred;
			}
			ins[i] = pred;
		}
		set_irn_in(dp->node, dp->n_preds, ins);
		if (is_Anchor(dp->node)) {
			irg_anchors a;
			for (a = anchor_first; a <= anchor_last; ++a) {
				ir_node *old_anchor = get_irg_anchor(irg, a);
				ir_node *new_anchor = ins[a];
				exchange(old_anchor, new_anchor);
			}
		}
next_delayed_pred: ;
	}
	DEL_ARR_F(env->delayed_preds);
	env->delayed_preds = NULL;
}

static ir_graph *read_irg(read_env_t *env)
{
	ir_entity          *irgent = get_entity(env, read_long(env));
	ir_graph           *irg    = new_ir_graph(irgent, 0);
	ir_type            *frame  = read_type_ref(env);
	irg_inline_property prop   = read_inline_property(env);
	unsigned            props  = read_unsigned(env);
	irg_finalize_cons(irg);
	set_irg_frame_type(irg, frame);
	set_irg_inline_property(irg, prop);
	set_irg_additional_properties(irg, (mtp_additional_properties)props);
	read_graph(env, irg);
	return irg;
}

static void read_modes(read_env_t *env)
{
	EXPECT('{');

	while (true) {
		keyword_t kwkind;

		skip_ws(env);
		if (env->c == '}' || env->c == EOF) {
			read_c(env);
			break;
		}

		kwkind = read_keyword(env);
		switch (kwkind) {
		case kw_int_mode: {
			const char *name = read_string(env);
			ir_mode_arithmetic arith = read_mode_arithmetic(env);
			int size = read_long(env);
			int sign = read_long(env);
			unsigned modulo_shift = read_long(env);
			new_int_mode(name, arith, size, sign, modulo_shift);
			break;
		}
		case kw_reference_mode: {
			const char *name = read_string(env);
			ir_mode_arithmetic arith = read_mode_arithmetic(env);
			int size = read_long(env);
			unsigned modulo_shift = read_long(env);
			ir_mode *mode = new_reference_mode(name, arith, size, modulo_shift);
			set_reference_mode_signed_eq(mode, read_mode_ref(env));
			set_reference_mode_unsigned_eq(mode, read_mode_ref(env));
			int is_mode_P = read_int(env);
			if (is_mode_P) {
				set_modeP_data(mode);
				set_modeP_code(mode);
			}
			break;
		}
		case kw_float_mode: {
			const char *name = read_string(env);
			ir_mode_arithmetic arith = read_mode_arithmetic(env);
			int exponent_size = read_long(env);
			int mantissa_size = read_long(env);
			new_float_mode(name, arith, exponent_size, mantissa_size);
			break;
		}

		default:
			skip_to(env, '\n');
			break;
		}
	}
}

static void read_program(read_env_t *env)
{
	EXPECT('{');

	while (true) {
		keyword_t kwkind;

		skip_ws(env);
		if (env->c == '}') {
			read_c(env);
			break;
		}

		kwkind = read_keyword(env);
		switch (kwkind) {
		case kw_segment_type: {
			ir_segment_t  segment = (ir_segment_t) read_enum(env, tt_segment);
			ir_type      *type    = read_type_ref(env);
			set_segment_type(segment, type);
			break;
		}
		case kw_asm: {
			ident *text = read_ident(env);
			add_irp_asm(text);
			break;
		}
		default:
			parse_error(env, "unexpected keyword %d\n", kwkind);
			skip_to(env, '\n');
		}
	}
}

int ir_import(const char *filename)
{
	FILE *file = fopen(filename, "rt");
	int   res;
	if (file == NULL) {
		perror(filename);
		return 1;
	}

	res = ir_import_file(file, filename);
	fclose(file);

	return res;
}

int ir_import_file(FILE *input, const char *inputname)
{
	read_env_t          myenv;
	int                 oldoptimize = get_optimize();
	read_env_t         *env         = &myenv;
	size_t              i;
	size_t              n;
	size_t              n_delayed_initializers;

	readers_init();
	symtbl_init();

	memset(env, 0, sizeof(*env));
	obstack_init(&env->obst);
	obstack_init(&env->preds_obst);
	env->idset      = new_set(id_cmp, 128);
	env->fixedtypes = NEW_ARR_F(ir_type *, 0);
	env->inputname  = inputname;
	env->file       = input;
	env->line       = 1;
	env->delayed_initializers = NEW_ARR_F(delayed_initializer_t, 0);

	/* read first character */
	read_c(env);

	set_optimize(0);

	while (true) {
		keyword_t kw;

		skip_ws(env);
		if (env->c == EOF)
			break;

		kw = read_keyword(env);
		switch (kw) {
		case kw_modes:
			read_modes(env);
			break;

		case kw_typegraph:
			read_typegraph(env);
			break;

		case kw_irg:
			read_irg(env);
			break;

		case kw_constirg: {
			ir_graph *constirg = get_const_code_irg();
			long bodyblockid = read_long(env);
			set_id(env, bodyblockid, constirg->current_block);
			read_graph(env, constirg);
			break;
		}

		case kw_program:
			read_program(env);
			break;

		default: {
			parse_error(env, "Unexpected keyword %d at toplevel\n", kw);
			exit(1);
		}
		}
	}

	n = ARR_LEN(env->fixedtypes);
	for (i = 0; i < n; i++)
		set_type_state(env->fixedtypes[i], layout_fixed);

	DEL_ARR_F(env->fixedtypes);

	/* resolve delayed initializers */
	n_delayed_initializers = ARR_LEN(env->delayed_initializers);
	for (i = 0; i < n_delayed_initializers; ++i) {
		const delayed_initializer_t *di   = &env->delayed_initializers[i];
		ir_node                     *node = get_node_or_null(env, di->node_nr);
		if (node == NULL) {
			parse_error(env, "node %ld mentioned in an initializer was never defined\n",
			            di->node_nr);
			continue;
		}
		assert(di->initializer->kind == IR_INITIALIZER_CONST);
		di->initializer->consti.value = node;
	}
	DEL_ARR_F(env->delayed_initializers);
	env->delayed_initializers = NULL;

	del_set(env->idset);

	irp_finalize_cons();

	set_optimize(oldoptimize);

	obstack_free(&env->preds_obst, NULL);
	obstack_free(&env->obst, NULL);

	pmap_destroy(node_readers);
	node_readers = NULL;

	return env->read_errors;
}

#include "gen_irio.inl"
