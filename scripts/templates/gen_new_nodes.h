{%- set ARCH %}{{arch|upper}}{% endset -%}
{{warning}}
#ifndef FIRM_BE_{{ARCH}}_GEN_{{ARCH}}_NEW_NODES_H
#define FIRM_BE_{{ARCH}}_GEN_{{ARCH}}_NEW_NODES_H

typedef enum {
{%- for node in nodes %}
	iro_{{node.name}},
{%- endfor %}
	iro_{{arch}}_last
} {{arch}}_opcodes;

bool is_{{arch}}_op(const ir_op *op);
bool is_{{arch}}_irn(const ir_node *node);

int get_{{arch}}_irn_opcode(const ir_node *node);
void {{arch}}_create_opcodes(const arch_irn_ops_t *be_ops);
void {{arch}}_free_opcodes(void);

{% for node in nodes %}

extern ir_op *op_{{node.name}};

static inline bool is_{{node.name}}(const ir_node *const node)
{
	return get_irn_op(node) == op_{{node.name}};
}

{% for c in node.constructors %}
ir_node *new_bd_{{node.name}}_{{c.name}}(
	{%- filter parameters %}
		dbg_info *dbgi
		ir_node *block
		int arity
		ir_node *in[]
		int n_res
		{{c.attr}}
	{% endfilter %});
{%- endfor %}

{% if node.ins %}
typedef enum {
	{% for input in node.ins -%}
	n_{{node.name}}_{{input.name}}, /**< {{input.comment}} */
	{% endfor -%}
	n_{{node.name}}_max = n_{{node.name}}_{{node.ins[-1].name}}
} n_{{node.name}};
{% endif %}

{% if node.outs %}
typedef enum {
	{% for out in node.outs -%}
	pn_{{node.name}}_{{out.name}}, /**< {{out.comment}} */
	{% endfor -%}
	pn_{{node.name}}_max = pn_{{node.name}}_{{node.outs[-1].name}}
} pn_{{node.name}};
{% endif %}

{%- endfor %}

#endif
