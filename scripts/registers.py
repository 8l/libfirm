from jinjautil import export

register_classes = []

def prepare_class(cls):
	cls.name = cls.__name__

def regclass(cls):
	"""class decorator to mark a (python) class as a register class."""
	prepare_class(cls)
	register_classes.append(cls)
	return cls

class Register(object):
	pass

def reg(name):
	r = Register()
	r.name = name
	return r

def make_limit_bitset(cls, register):
	n_regs    = len(cls.registers)
	n_buckets = (n_regs+31) / 32
	index     = cls.registers.index(register)
	res       = "{ "
	for b in range(0,n_buckets):
		if b > 0:
			res += ", "
		if index >= b*32 and index < (b+1)*32:
			if b > 0:
				res += "(1 << (REG_%s_%s %% 32))" % (cls.name.upper(), register.name.upper())
			else:
				res += "(1 << REG_%s_%s)" % (cls.name.upper(), register.name.upper())
		else:
			res += "0"
	res += " }"
	return res

export(register_classes, "register_classes")
export(make_limit_bitset)
