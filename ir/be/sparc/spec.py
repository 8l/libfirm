from registers import regclass, reg
from irops import abstract, op, Attribute, prepare_nodes
from beops import constructor, singlereq, producestackreq, noreq
from jinjautil import export

arch = "sparc"
export(arch, "arch")

@regclass
class gp:
	mode = "mode_Iu"
	registers = map(reg, [
		"g0", "g1", "g2", "g3", "g4", "g5", "g6", "g7",
		"o0", "o1", "o2", "o3", "o4", "o5", "sp", "o7",
		"l0", "l1", "l2", "l3", "l4", "l5", "l6", "l7",
		"i0", "i1", "i2", "i3", "i4", "i5", "fp", "i7",
	])

@regclass
class fp:
	mode = "mode_F"
	# f0 .. f31
	registers = map(reg, ["f%s" % num for num in range(0,32) ])

@regclass
class flags:
	mode = "mode_Bu"
	flags = [ "manual_ra" ]
	registers = [ reg("flags") ]

@regclass
class fpflags:
	mode = "mode_Bu"
	flags = [ "manual_ra" ]
	registers = [ reg("fpflags") ]


# Create an alias for "flags" as that has a different meaning for nodes
_flagscls = flags

class SparcBase(object):
	attr_struct = "sparc_attr_t"

# Graph anchors

@op
class Start(SparcBase):
	beflags   = [ "schedule_first" ]
	pinned    = "yes"
	outs_reqs = "..."
	ins       = []
	emit      = None

# Control Flow

class JumpBase(SparcBase):
	pinned   = "yes"
	flags    = [ "cfopcode" ]
	out_reqs = [ noreq ]
	mode     = "mode_X"

class BranchBase(JumpBase):
	flags       = [ "cfopcode", "forking" ]
	beflags     = [ "has_delay_slot" ]
	attr_struct = "sparc_jmp_cond_attr_t"
	ins         = [ "flags" ]
	outs        = [ "false", "true" ]
	out_reqs    = [ noreq, noreq ]

@op
class Ba(JumpBase):
	beflags = [ "simple_jump" ]

@op
class Return(JumpBase):
	beflags = [ "has_delay_slot" ]
	in_reqs = "..."
	constructors = [
		constructor("imm",
			attr       = "ir_entity *entity, int32_t offset",
			custominit = "sparc_set_attr_imm(res, entity, offset);"
		),
		constructor("reg")
	]

@op
class Call(JumpBase):
	beflags     = [ "has_delay_slot" ]
	pinned      = "exception"
	in_reqs     = "..."
	out_reqs    = "..."
	outs        = [ "M", "stack", "first_result" ]
	attr_struct = "sparc_call_attr_t"
	constructors = [
		constructor("imm",
			attr       = "ir_type *call_type, ir_entity *entity, int32_t offset, bool aggregate_return",
			custominit = "sparc_set_attr_imm(res, entity, offset);\n" + \
			             "\tif (aggregate_return) arch_add_irn_flags(res, (arch_irn_flags_t)sparc_arch_irn_flag_aggregate_return);"
		),
		constructor("reg",
			attr       = "ir_type *call_type, bool aggregate_return",
			custominit = "if (aggregate_return) arch_add_irn_flags(res, (arch_irn_flags_t)sparc_arch_irn_flag_aggregate_return);"
		),
	]

@op
class SwitchJmp(BranchBase):
	in_reqs     = [ gp ]
	out_reqs    = "..."
	attr_struct = "sparc_switch_jmp_attr_t"
	constructors = [
		constructor("",
			attr = "const ir_switch_table *table, ir_entity *jump_table"
		)
	]

@op
class Bicc(BranchBase):
	attr      = "ir_relation relation, bool is_unsigned"
	init_attr = "init_sparc_jmp_cond_attr(res, relation, is_unsigned);"
	in_reqs   = [ _flagscls ]

@op
class fbfcc(BranchBase):
	attr      = "ir_relation relation"
	init_attr = "init_sparc_jmp_cond_attr(res, relation, false);"
	in_reqs   = [ fpflags ]

# Arithmetic operations

_binop_constructors = [
	constructor("imm",
		attr       = "ir_entity *immediate_entity, int32_t immediate_value",
		custominit = "sparc_set_attr_imm(res, immediate_entity, immediate_value);",
		in_reqs    = [ gp ],
		ins        = [ "left" ]
	),
	constructor("reg",
		in_reqs = [ gp, gp ],
		ins     = [ "left", "right" ]
	),
]

class BinopBase(SparcBase):
	beflags      = [ "rematerializable" ]
	mode         = gp.mode
	out_reqs     = [ gp ]
	constructors = _binop_constructors

class BinopCCBase(SparcBase):
	beflags      = [ "rematerializable" ]
	outs         = [ "res", "flags" ]
	out_reqs     = [ gp, _flagscls ]
	constructors = _binop_constructors

class BinopCCZeroBase(SparcBase):
	beflags      = [ "rematerializable" ]
	mode         = _flagscls.mode
	out_reqs     = [ _flagscls ]
	constructors = _binop_constructors

class BinopXBase(SparcBase):
	# At the moment not rematerializable because of assert in beflags.c
	# (it claims that spiller can't rematerialize flag stuff correctly)
	#beflags = [ "rematerializable" ]
	out_reqs = [ gp ]
	mode     = gp.mode
	constructors = [
		constructor("imm",
			attr       = "ir_entity *immediate_entity, int32_t immediate_value",
			custominit = "sparc_set_attr_imm(res, immediate_entity, immediate_value);",
			in_reqs    = [ gp, _flagscls ],
			ins        = [ "left", "carry" ]
		),
		constructor("reg",
			in_reqs = [ gp, gp, _flagscls ],
			ins     = [ "left", "right", "carry" ]
		),
	]

@op
class Add(BinopBase):
	emit = "add %S0, %SI1, %D0"

@op
class AddCC(BinopCCBase):
	emit = "addcc %S0, %SI1, %D0"

@op
class AddCCZero(BinopCCZeroBase):
	emit = "addcc %S0, %SI1, %%g0"

@op
class AddX(BinopXBase):
	emit = "addx %S0, %SI1, %D0"

@op
class AddSP(SparcBase):
	in_reqs  = [ singlereq(gp, "sp"), gp ],
	out_reqs = [ producestackreq(gp, "sp") ],
	ins      = [ "stack", "size" ],
	outs     = [ "stack" ],
	mode     = gp.mode
	emit     = "add %S0, %S1, %D0"

# TODO: AddCC_t
# TODO: AddX_t

@op
class Sub(BinopBase):
	emit = "sub %S0, %SI1, %D0"

@op
class SubCC(BinopCCBase):
	emit = "subcc %S0, %SI1, %D0"

@op
class SubCCZero(BinopCCZeroBase):
	emit = "subcc %S0, %SI1, %%g0"

@op
class SubX(BinopXBase):
	emit = "subx %S0, %SI1, %D0"

# TODO: SubCC_t
# TODO: SubX_t

@op
class Sll(BinopBase):
	emit = "sll %S0, %SI1, %D0"

@op
class Srl(BinopBase):
	emit = "srl %S0, %SI1, %D0"

@op
class Sra(BinopBase):
	emit = "sra %S0, %SI1, %D0"

@op
class And(BinopBase):
	emit = "and %S0, %SI1, %D0"

@op
class AndN(BinopBase):
	emit = "andn %S0, %SI1, %D0"

(nodes, abstract_nodes) = prepare_nodes(globals())
for node in nodes:
	node.name = "sparc_" + node.name
export(nodes, "nodes")
export(abstract_nodes, "abstract_nodes")
