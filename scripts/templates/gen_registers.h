{%- set ARCH %}{{arch|upper}}{% endset -%}
{{warning}}
#ifndef FIRM_BE_{{ARCH}}_GEN_{{ARCH}}_REGALLOC_IF_H
#define FIRM_BE_{{ARCH}}_GEN_{{ARCH}}_REGALLOC_IF_H

#include "bearch.h"
#include "{{arch}}_nodes_attr.h"

/** global register indices */
enum {{arch}}_reg_indices {
	{%- for regclass in register_classes %}
	{%- for reg in regclass.registers %}
	REG_{{reg.name|upper}},
	{%- endfor %}
	{%- endfor %}
	N_{{ARCH}}_REGISTERS
};

enum {{arch}}_register_classes {
{%- for regclass in register_classes %}
	CLASS_{{ARCH}}_{{regclass.name|upper}},
{%- endfor %}
	N_{{ARCH}}_CLASSES
};
{%- for regclass in register_classes %}

enum {{arch}}_{{regclass.name}}_indices {
	{%- for reg in regclass.registers %}
	REG_{{regclass.name|upper}}_{{reg.name|upper}},
	{%- endfor %}
	N_{{ARCH}}_{{regclass.name|upper}}_REGISTERS
};
{%- endfor %}

extern const arch_register_t {{arch}}_registers[N_{{ARCH}}_REGISTERS];
extern arch_register_class_t {{arch}}_reg_classes[N_{{ARCH}}_CLASSES];

void {{arch}}_register_init(void);

#endif
