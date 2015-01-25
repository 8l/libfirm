class Constructor(object):
	pass

def constructor(name, attr = "", custominit = "", in_reqs = [], ins = []):
	c = Constructor()
	c.name= name
	c.attr = attr
	c.custominit = custominit
	c.in_reqs = in_reqs
	c.ins = ins
	return c

class RegisterReq(object):
	pass

def singlereq(regclass, regname):
	reg = None
	for r in regclass.registers:
		print "Cls %s R: %s" % (regclass, r,)
		if r.name == regname:
			reg = r
			break
	if reg is None:
		raise Exception("No register named %s in regclass %s" % (regname, regclass.name))

	req = RegisterReq()
	req.kind = singlereq
	req.cls = regclass
	req.reg = reg
	return req

def producestackreq(regclass, regname):
	req = singlereq(regclass, regname)
	req.kind = producestackreq
	return req

noreq = RegisterReq()
noreq.kind = noreq
