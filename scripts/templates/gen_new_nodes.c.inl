#include "gen_{{arch}}_regalloc_if.h"
#include "fourcc.h"
#include "irgopt.h"

{% for node in nodes %}
ir_op *op_{{arch}}_{{node.name}};
{%- endfor -%}

static int {{arch}}_opcode_start = -1;

#define {{arch}}_op_tag FOURCC({{arch[0]}}, {{arch[1]}}, {{arch[2]}}, {{arch[3]}})

bool is_{{arch}}_op(const ir_op *op)
{
	return get_op_tag(op) == {{arch}}_op_tag;
}

bool is_{{arch}}_irn(const ir_node *node)
{
	return is_{{arch}}_op(get_irn_op(node));
}

int get_{{arch}}_irn_opcode(const ir_node *node)
{
	assert(is_{{arch}}_irn(node));
	return get_irn_opcode(node) - {{arch}}_opcode_start;
}
