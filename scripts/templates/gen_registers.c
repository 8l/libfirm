{%- set ARCH %}{{arch|upper}}{% endset -%}
/* Warning: automatically generated code */
#include "config.h"

#include "gen_{{arch}}_regalloc_if.h"
#include "bearch_{{arch}}_t.h"
#include "irmode.h"

{% for cls in register_classes %}
static const arch_register_req_t {{arch}}_class_reg_req_{{cls.name}} = {
	arch_register_req_type_normal,
	&{{arch}}_reg_classes[CLASS_{{ARCH}}_{{cls.name|upper}}],
	NULL,
	0,
	0,
	1
};
{%- endfor %}

arch_register_class_t {{arch}}_reg_classes[] = {
{%- for cls in register_classes %}
	{ CLASS_{{ARCH}}_{{cls.name|upper}}, "{{arch}}_{{cls.name}}", N_{{ARCH}}_{{cls.name|upper}}_REGISTERS, NULL, &{{arch}}_registers[REG_{{cls.registers[0].name|upper}}],
	{%- if cls.flags == [] %} arch_register_class_flag_none
	{%- else -%}
	{%- set ors = joiner("| ") -%}
	{%- for flag in cls.flags %} {{ors()}}arch_register_class_flag_{{flag}}{% endfor -%}
	{%- endif -%}
	, &{{arch}}_class_reg_req_{{cls.name}} },
{%- endfor %}
};

{% for cls in register_classes %}
{%- for reg in cls.registers %}
static const unsigned sparc_limited_{{cls.name}}_{{reg.name}}[] = {{make_limit_bitset(cls, reg)}};
static const arch_register_req_t {{arch}}_single_reg_req_{{cls.name}}_{{reg.name}} = {
	arch_register_req_type_limited,
	&{{arch}}_reg_classes[CLASS_{{ARCH}}_{{cls.name|upper}}],
	{{arch}}_limited_{{cls.name}}_{{reg.name}},
	0,
	0,
	1
};
{%- endfor -%}
{%- endfor %}

const arch_register_t {{arch}}_registers[] = {
	{%- for cls in register_classes %}
	{%- for reg in cls.registers %}
	{
		"{{reg.realname}}",
		&{{arch}}_reg_classes[CLASS_{{arch}}_{{cls.name}}],
		REG_{{cls.name|upper}}_{{reg.name|upper}},
		REG_{{reg.name|upper}},
		arch_register_type_none,
		&{{arch}}_single_reg_req_{{cls.name}}_{{reg.name}}
	},
	{%- endfor -%}
	{%- endfor %}
};

void {{arch}}_register_init(void)
{
	{%- for cls in register_classes %}
	{{arch}}_reg_classes[CLASS_{{arch}}_{{cls.name}}].mode = {{cls.mode}};
	{%- endfor %}
}
