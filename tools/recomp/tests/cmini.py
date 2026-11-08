"""A small interpreter for the C translate.py emits for x87 and integer code.

    from cmini import Machine, run_function

test_x87_carry.py runs synthetic functions through it, so the x87 cache,
its write-back trimming and the label carry can be checked against the
per-instruction helpers on any host, with no C compiler: the same body is
translated with and without each switch and both run from the same state.

It understands the subset the translator writes for that code: one function
body of declarations, expression statements, blocks, if/else, labels, goto
and return, over C's integer and double arithmetic with the usual
conversions. The runtime helpers it calls (rd32, fpush, fcom, ...) are
Python models of runtime/x86.h, written for agreement between the two sides
of a comparison rather than as a second reference for x87 semantics: both
sides call the same model. Anything it does not understand raises."""

import math
import re
import struct

# ---------------------------------------------------------------- values ---

INT_TYPES = {
    "uint8_t": (8, False), "uint16_t": (16, False), "uint32_t": (32, False),
    "uint64_t": (64, False), "int8_t": (8, True), "int16_t": (16, True),
    "int32_t": (32, True), "int64_t": (64, True), "int": (32, True),
    "unsigned": (32, False),
}
FLOAT_TYPES = ("double", "float")
TYPES = set(INT_TYPES) | set(FLOAT_TYPES)


class V(object):
    """A C value: an int with (bits, signed), or a float as 'double'/'float'."""
    __slots__ = ("v", "t")

    def __init__(self, v, t):
        self.v, self.t = v, t

    def __repr__(self):
        return "V(%r, %s)" % (self.v, self.t)


def wrap(v, t):
    bits, signed = INT_TYPES[t]
    v &= (1 << bits) - 1
    if signed and v >> (bits - 1):
        v -= 1 << bits
    return v


def f32(x):
    try:
        return struct.unpack("<f", struct.pack("<f", x))[0]
    except OverflowError:
        return math.copysign(math.inf, x)


def convert(val, t):
    if t in FLOAT_TYPES:
        x = float(val.v)
        return V(f32(x) if t == "float" else x, t)
    if val.t in FLOAT_TYPES:
        x = val.v
        if x != x or math.isinf(x):
            raise ValueError("float to integer conversion of %r" % x)
        return V(wrap(int(x), t), t)  # truncation toward zero
    return V(wrap(val.v, t), t)


def promote(val):
    if val.t in FLOAT_TYPES:
        return val
    bits, _signed = INT_TYPES[val.t]
    return convert(val, "int") if bits < 32 else val


def common(a, b):
    """The usual arithmetic conversions."""
    if a.t == "double" or b.t == "double":
        return "double"
    if a.t == "float" or b.t == "float":
        return "float"
    a, b = promote(a), promote(b)
    ra, rb = INT_TYPES[a.t], INT_TYPES[b.t]
    if ra == rb:
        return a.t
    if ra[1] == rb[1]:
        return a.t if ra[0] > rb[0] else b.t
    u, s = (a, b) if not ra[1] else (b, a)
    if INT_TYPES[u.t][0] >= INT_TYPES[s.t][0]:
        return u.t
    return s.t


def truth(val):
    return val.v != 0


def boolean(x):
    return V(1 if x else 0, "int")


# --------------------------------------------------------------- lexer ---

TOKEN = re.compile(r"""
    (?P<ws>\s+)
  | (?P<num>0[xX][0-9a-fA-F]+[uUlL]*|\d+\.\d*(?:[eE][-+]?\d+)?[fF]?|\d+[eE][-+]?\d+|\d+[uUlL]*)
  | (?P<id>[A-Za-z_]\w*)
  | (?P<op><<=|>>=|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||\+=|-=|\*=|/=|&=|\|=|\^=|[-+*/%&|^~!<>=?:;,(){}\[\].])
""", re.X)


def lex(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    out, pos = [], 0
    while pos < len(text):
        m = TOKEN.match(text, pos)
        if not m:
            raise SyntaxError("cannot lex %r" % text[pos:pos + 30])
        pos = m.end()
        if m.lastgroup != "ws":
            out.append((m.lastgroup, m.group(m.lastgroup)))
    out.append(("eof", ""))
    return out


def number(text):
    t = text.rstrip("uUlL")
    suffix = text[len(t):].lower()
    if "." in t or (("e" in t.lower()) and not t.lower().startswith("0x")):
        if t.endswith(("f", "F")):
            return V(f32(float(t[:-1])), "float")
        return V(float(t), "double")
    v = int(t, 16) if t.lower().startswith("0x") else int(t)
    if "ll" in suffix:
        return V(v, "uint64_t" if "u" in suffix else "int64_t")
    if "u" in suffix:
        return V(v, "uint32_t" if v < 1 << 32 else "uint64_t")
    return V(v, "int" if v < 1 << 31 else ("uint32_t" if v < 1 << 32 else "int64_t"))


# -------------------------------------------------------------- parser ---

BINARY = [("||",), ("&&",), ("|",), ("^",), ("&",), ("==", "!="), ("<", ">", "<=", ">="),
          ("<<", ">>"), ("+", "-"), ("*", "/", "%")]
ASSIGN = ("=", "+=", "-=", "*=", "/=", "&=", "|=", "^=", "<<=", ">>=")


class Parser(object):
    def __init__(self, tokens):
        self.t, self.i = tokens, 0

    def peek(self, k=0):
        return self.t[self.i + k]

    def take(self, want=None):
        tok = self.t[self.i]
        if want is not None and tok[1] != want:
            raise SyntaxError("expected %r, found %r near %r" % (want, tok[1], self.t[self.i:self.i + 8]))
        self.i += 1
        return tok

    def at(self, text):
        return self.t[self.i][1] == text

    # statements
    def statement(self):
        k, text = self.peek()
        if text == "{":
            self.take()
            body = []
            while not self.at("}"):
                body.append(self.statement())
            self.take("}")
            return ("block", body)
        if text == ";":
            self.take()
            return ("block", [])
        if text == "if":
            self.take()
            self.take("(")
            cond = self.expr()
            self.take(")")
            then = self.statement()
            other = None
            if self.at("else"):
                self.take()
                other = self.statement()
            return ("if", cond, then, other)
        if text == "goto":
            self.take()
            label = self.take()[1]
            self.take(";")
            return ("goto", label)
        if text == "return":
            self.take()
            self.take(";")
            return ("return",)
        if k == "id" and self.peek(1)[1] == ":" and text not in TYPES:
            self.take()
            self.take(":")
            return ("label", text)
        if text in TYPES:
            t = self.take()[1]
            decls = []
            while True:
                name = self.take()[1]
                init = None
                if self.at("["):
                    raise SyntaxError("arrays are not modelled")
                if self.at("="):
                    self.take()
                    init = self.assign()
                decls.append((name, init))
                if self.at(","):
                    self.take()
                    continue
                break
            self.take(";")
            return ("decl", t, decls)
        e = self.expr()
        self.take(";")
        return ("expr", e)

    # expressions
    def expr(self):
        e = self.assign()
        while self.at(","):
            self.take()
            e = ("comma", e, self.assign())
        return e

    def assign(self):
        lhs = self.ternary()
        if self.peek()[1] in ASSIGN:
            op = self.take()[1]
            return ("assign", op, lhs, self.assign())
        return lhs

    def ternary(self):
        c = self.binary(0)
        if self.at("?"):
            self.take()
            a = self.assign()
            self.take(":")
            b = self.assign()
            return ("cond", c, a, b)
        return c

    def binary(self, level):
        if level == len(BINARY):
            return self.unary()
        e = self.binary(level + 1)
        while self.peek()[1] in BINARY[level]:
            op = self.take()[1]
            e = ("bin", op, e, self.binary(level + 1))
        return e

    def unary(self):
        k, text = self.peek()
        if text in ("!", "~", "-", "+"):
            self.take()
            return ("un", text, self.unary())
        if text == "(" and self.peek(1)[1] in TYPES and self.peek(2)[1] in (")", "*"):
            self.take()
            t = self.take()[1]
            if self.at("*"):
                raise SyntaxError("pointer casts are not modelled")
            self.take(")")
            return ("cast", t, self.unary())
        return self.postfix()

    def postfix(self):
        e = self.primary()
        while True:
            if self.at("["):
                self.take()
                i = self.expr()
                self.take("]")
                e = ("index", e, i)
            elif self.at("->"):
                self.take()
                e = ("field", e, self.take()[1])
            elif self.at("("):
                self.take()
                args = []
                while not self.at(")"):
                    args.append(self.assign())
                    if self.at(","):
                        self.take()
                self.take(")")
                e = ("call", e, args)
            else:
                return e

    def primary(self):
        k, text = self.take()
        if k == "num":
            return ("num", number(text))
        if k == "id":
            return ("var", text)
        if text == "(":
            e = self.expr()
            self.take(")")
            return e
        raise SyntaxError("unexpected %r" % text)


# ------------------------------------------------------------- machine ---

FIELDS = {"eip": "uint32_t", "fpu_top": "uint32_t", "fpu_cw": "uint16_t", "fpu_sw": "uint16_t",
          "fpu_tag": "uint16_t", "fs_base": "uint32_t", "eflags_misc": "uint32_t"}
for _f in ("cf", "zf", "sf", "of", "pf", "af", "df"):
    FIELDS["eflags_" + _f] = "uint32_t"
ARRAYS = {"r": ("uint32_t", 8), "st": ("double", 8), "st_bits": ("uint64_t", 8),
          "st_exact": ("uint8_t", 8)}


class Return(Exception):
    pass


class State(object):
    """The parts of X86 the x87 and integer code touch, plus guest memory."""

    def __init__(self):
        self.f = {k: 0 for k in FIELDS}
        self.a = {k: [0.0 if t == "double" else 0] * n for k, (t, n) in ARRAYS.items()}
        self.mem = {}

    def copy(self):
        s = State()
        s.f = dict(self.f)
        s.a = {k: list(v) for k, v in self.a.items()}
        s.mem = dict(self.mem)
        return s

    # memory
    def rd(self, a, n):
        return int.from_bytes(bytes(self.mem.get((a + k) & 0xffffffff, 0) for k in range(n)), "little")

    def wr(self, a, n, v):
        for k, b in enumerate((v & ((1 << (8 * n)) - 1)).to_bytes(n, "little")):
            self.mem[(a + k) & 0xffffffff] = b


def ftag_classify(x):
    bits = struct.unpack("<Q", struct.pack("<d", x))[0] & 0x7fffffffffffffff
    if not bits:
        return 1
    if bits >= 0x7ff0000000000000:
        return 2
    return 0


INDEFINITE = struct.unpack("<d", struct.pack("<Q", 0xfff8000000000000))[0]


class Machine(object):
    """Runs one translated function body against a State."""

    def __init__(self, source, name):
        m = re.search(r"void %s\(X86 \*RECOMP_CTX c\) \{(.*)\n\}" % re.escape(name), source, re.S)
        if not m:
            raise ValueError("no function %s" % name)
        p = Parser(lex("{" + m.group(1) + "}"))
        self.code, self.labels = [], {}
        self.compile(p.statement())
        if p.peek()[0] != "eof":
            raise SyntaxError("text after the body: %r" % (p.t[p.i:p.i + 8],))

    # flatten the AST into a list with jumps
    def compile(self, node):
        kind = node[0]
        if kind == "block":
            for s in node[1]:
                self.compile(s)
        elif kind == "if":
            jf = len(self.code)
            self.code.append(["jf", node[1], None])
            self.compile(node[2])
            if node[3] is not None:
                j = len(self.code)
                self.code.append(["j", None])
                self.code[jf][2] = len(self.code)
                self.compile(node[3])
                self.code[j][1] = len(self.code)
            else:
                self.code[jf][2] = len(self.code)
        elif kind == "label":
            self.labels[node[1]] = len(self.code)
        elif kind == "goto":
            self.code.append(["goto", node[1]])
        else:
            self.code.append([kind] + list(node[1:]))

    def run(self, state, limit=200000):
        self.s, self.vars = state, {}
        #: Set when a helper read an empty register (x87 stack underflow,
        #: which the runtime does not model: it reads the stale value).
        self.underflow = False
        pc, steps = 0, 0
        while pc < len(self.code):
            steps += 1
            if steps > limit:
                raise RuntimeError("step limit")
            op = self.code[pc]
            pc += 1
            kind = op[0]
            if kind == "jf":
                if not truth(self.ev(op[1])):
                    pc = op[2]
            elif kind == "j":
                pc = op[1]
            elif kind == "goto":
                pc = self.labels[op[1]]
            elif kind == "return":
                return
            elif kind == "decl":
                for name, init in op[2]:
                    self.vars[name] = convert(self.ev(init), op[1]) if init is not None else V(
                        0.0 if op[1] in FLOAT_TYPES else 0, op[1])
            elif kind == "expr":
                try:
                    self.ev(op[1])
                except Return:
                    return
            else:
                raise RuntimeError("statement %r" % (op,))

    # lvalues: (getter, setter, type)
    def lvalue(self, e):
        if e[0] == "var":
            name = e[1]
            t = self.vars[name].t
            return (lambda: self.vars[name], lambda v: self.vars.__setitem__(name, convert(v, t)), t)
        if e[0] == "field" and e[1] == ("var", "c"):
            f = e[2]
            t = FIELDS[f]
            return (lambda: V(self.s.f[f], t),
                    lambda v: self.s.f.__setitem__(f, convert(v, t).v), t)
        if e[0] == "index" and e[1][0] == "field" and e[1][1] == ("var", "c"):
            arr = e[1][2]
            t, n = ARRAYS[arr]
            i = self.ev(e[2]).v
            if not 0 <= i < n:
                raise IndexError("%s[%d]" % (arr, i))
            return (lambda: V(self.s.a[arr][i], t),
                    lambda v: self.s.a[arr].__setitem__(i, convert(v, t).v), t)
        raise RuntimeError("not an lvalue: %r" % (e,))

    def ev(self, e):
        kind = e[0]
        if kind == "num":
            return e[1]
        if kind == "var":
            if e[1] in self.vars:
                return self.vars[e[1]]
            if e[1] in CONSTS:
                return CONSTS[e[1]]
            raise NameError(e[1])
        if kind in ("field", "index"):
            return self.lvalue(e)[0]()
        if kind == "assign":
            get, put, t = self.lvalue(e[2])
            rhs = self.ev(e[3])
            if e[1] != "=":
                rhs = self.binop(e[1][:-1], get(), rhs)
            put(rhs)
            return get()
        if kind == "comma":
            self.ev(e[1])
            return self.ev(e[2])
        if kind == "cond":
            return self.ev(e[2]) if truth(self.ev(e[1])) else self.ev(e[3])
        if kind == "cast":
            return convert(self.ev(e[2]), e[1])
        if kind == "un":
            a = self.ev(e[2])
            if e[1] == "!":
                return boolean(not truth(a))
            a = promote(a)
            if e[1] == "-":
                return V(-a.v, a.t) if a.t in FLOAT_TYPES else convert(V(-a.v, a.t), a.t)
            if e[1] == "~":
                return convert(V(~a.v, a.t), a.t)
            return a
        if kind == "bin":
            if e[1] == "&&":
                return boolean(truth(self.ev(e[2])) and truth(self.ev(e[3])))
            if e[1] == "||":
                return boolean(truth(self.ev(e[2])) or truth(self.ev(e[3])))
            return self.binop(e[1], self.ev(e[2]), self.ev(e[3]))
        if kind == "call":
            name = e[1][1]
            fn = getattr(self, "c_" + name, None)
            if fn is None:
                raise NameError("no model of %s()" % name)
            return fn(*e[2])
        raise RuntimeError("expression %r" % (e,))

    def binop(self, op, a, b):
        if op in ("<<", ">>"):
            a, b = promote(a), promote(b)
            bits = INT_TYPES[a.t][0]
            if not 0 <= b.v < bits:
                raise ValueError("shift by %d" % b.v)
            return convert(V(a.v << b.v if op == "<<" else a.v >> b.v, a.t), a.t)
        t = common(a, b)
        x, y = convert(a, t).v, convert(b, t).v
        if op in ("==", "!=", "<", ">", "<=", ">="):
            return boolean({"==": x == y, "!=": x != y, "<": x < y, ">": x > y,
                            "<=": x <= y, ">=": x >= y}[op])
        if t in FLOAT_TYPES:
            if op == "+":
                r = x + y
            elif op == "-":
                r = x - y
            elif op == "*":
                r = x * y
            elif op == "/":
                r = x / y if y else (math.copysign(math.inf, x) * math.copysign(1, y) if x and x == x else math.nan)
            else:
                raise RuntimeError("float %s" % op)
            return V(f32(r) if t == "float" else r, t)
        if op == "/":
            r = abs(x) // abs(y) * (1 if (x < 0) == (y < 0) else -1)
        elif op == "%":
            r = x - (abs(x) // abs(y) * (1 if (x < 0) == (y < 0) else -1)) * y
        else:
            r = {"+": x + y, "-": x - y, "*": x * y, "&": x & y, "|": x | y, "^": x ^ y}[op]
        return convert(V(r, t), t)

    # ---- runtime models (runtime/x86.h) ---------------------------------
    def args(self, args):
        return [self.ev(a) for a in args]

    def top(self):
        return self.s.f["fpu_top"] & 7

    def tag_put(self, phys, tag):
        sh = 2 * (phys & 7)
        self.s.f["fpu_tag"] = (self.s.f["fpu_tag"] & ~(3 << sh) & 0xffff) | (tag << sh)

    def tag_of(self, phys):
        return (self.s.f["fpu_tag"] >> (2 * (phys & 7))) & 3

    def c_rd8(self, a):
        return V(self.s.rd(self.ev(a).v, 1), "uint8_t")

    def c_rd16(self, a):
        return V(self.s.rd(self.ev(a).v, 2), "uint16_t")

    def c_rd32(self, a):
        return V(self.s.rd(self.ev(a).v, 4), "uint32_t")

    def c_rd64(self, a):
        return V(self.s.rd(self.ev(a).v, 8), "uint64_t")

    def c_wr8(self, a, v):
        self.s.wr(self.ev(a).v, 1, convert(self.ev(v), "uint8_t").v)

    def c_wr16(self, a, v):
        self.s.wr(self.ev(a).v, 2, convert(self.ev(v), "uint16_t").v)

    def c_wr32(self, a, v):
        self.s.wr(self.ev(a).v, 4, convert(self.ev(v), "uint32_t").v)

    def c_wr64(self, a, v):
        self.s.wr(self.ev(a).v, 8, convert(self.ev(v), "uint64_t").v)

    def c_rdf32(self, a):
        return V(struct.unpack("<f", struct.pack("<I", self.s.rd(self.ev(a).v, 4)))[0], "float")

    def c_rdf64(self, a):
        return V(struct.unpack("<d", struct.pack("<Q", self.s.rd(self.ev(a).v, 8)))[0], "double")

    def c_wrf32(self, a, v):
        self.s.wr(self.ev(a).v, 4, struct.unpack("<I", struct.pack("<f", convert(self.ev(v), "float").v))[0])

    def c_wrf64(self, a, v):
        self.s.wr(self.ev(a).v, 8, struct.unpack("<Q", struct.pack("<d", convert(self.ev(v), "double").v))[0])

    def c_parity8(self, v):
        return V(1 if bin(self.ev(v).v & 0xff).count("1") % 2 == 0 else 0, "uint32_t")

    def c_fto_float(self, c, v):
        # Round-to-nearest only: the cases set no directed rounding modes.
        return convert(self.ev(v), "float")

    def c_fx87(self, c, v):
        x = convert(self.ev(v), "double").v
        if x != x:
            self.s.f["fpu_sw"] |= 1
            return V(INDEFINITE, "double")
        if ((self.s.f["fpu_cw"] >> 8) & 3) == 0:
            return V(f32(x), "double")
        return V(x, "double")

    def c_fx87_exact(self, c, v):
        x = convert(self.ev(v), "double").v
        if x != x:
            self.s.f["fpu_sw"] |= 1
            return V(INDEFINITE, "double")
        return V(x, "double")

    def c_fdivz(self, c, a, b):
        x, y = convert(self.ev(a), "double").v, convert(self.ev(b), "double").v
        if y == 0.0 and x == x and not math.isinf(x) and x != 0.0:
            self.s.f["fpu_sw"] |= 4
        return self.binop("/", V(x, "double"), V(y, "double"))

    def c_fabs(self, v):
        return V(abs(convert(self.ev(v), "double").v), "double")

    def c_sqrt(self, v):
        x = convert(self.ev(v), "double").v
        return V(math.sqrt(x) if x >= 0 else math.nan, "double")

    def c_ftag_classify(self, v):
        return V(ftag_classify(convert(self.ev(v), "double").v), "uint32_t")

    def c_ftag_put(self, c, phys, tag):
        self.tag_put(self.ev(phys).v, self.ev(tag).v)

    def read(self, phys):
        if self.tag_of(phys) == 3:
            self.underflow = True
        return phys

    def c_ST(self, c, i):
        return V(self.s.a["st"][self.read((self.top() + self.ev(i).v) & 7)], "double")

    def push(self, v, bits, exact, tag):
        t = (self.top() - 1) & 7
        self.s.f["fpu_top"] = t
        self.s.a["st"][t] = v
        self.s.a["st_bits"][t] = bits
        self.s.a["st_exact"][t] = exact
        self.tag_put(t, tag)

    def c_fpush(self, c, v):
        x = convert(self.ev(v), "double").v
        self.push(x, 0, 0, ftag_classify(x))

    def c_fpush_int(self, c, v):
        n = convert(self.ev(v), "int64_t").v
        x = float(n)
        self.push(x, n & 0xffffffffffffffff, 1, ftag_classify(x))

    def c_fpush_st(self, c, i):
        src = self.read((self.top() + self.ev(i).v) & 7)
        self.push(self.s.a["st"][src], self.s.a["st_bits"][src], self.s.a["st_exact"][src], self.tag_of(src))

    def c_fdrop(self, c):
        t = self.top()
        self.s.a["st_exact"][t] = 0
        self.tag_put(t, 3)
        self.s.f["fpu_top"] = (t + 1) & 7

    def c_fpop(self, c):
        v = self.s.a["st"][self.top()]
        self.c_fdrop(c)
        return V(v, "double")

    def c_fset(self, c, i, v):
        p = (self.top() + self.ev(i).v) & 7
        x = convert(self.ev(v), "double").v
        self.s.a["st"][p] = x
        self.s.a["st_bits"][p] = 0
        self.s.a["st_exact"][p] = 0
        self.tag_put(p, ftag_classify(x))

    def c_fcopy(self, c, d, s):
        a, b = (self.top() + self.ev(d).v) & 7, self.read((self.top() + self.ev(s).v) & 7)
        self.s.a["st"][a] = self.s.a["st"][b]
        self.s.a["st_bits"][a] = self.s.a["st_bits"][b]
        self.s.a["st_exact"][a] = self.s.a["st_exact"][b]
        self.tag_put(a, self.tag_of(b))

    def c_fxch(self, c, i):
        a, b = self.read(self.top()), self.read((self.top() + self.ev(i).v) & 7)
        for k in ("st", "st_bits", "st_exact"):
            self.s.a[k][a], self.s.a[k][b] = self.s.a[k][b], self.s.a[k][a]
        ta, tb = self.tag_of(a), self.tag_of(b)
        self.tag_put(a, tb)
        self.tag_put(b, ta)

    def c_fstsw(self, c):
        return V((self.s.f["fpu_sw"] & ~0x3800 & 0xffff) | (self.top() << 11), "uint16_t")

    def fcom_common(self, a, b, quiet):
        sw = self.s.f["fpu_sw"] & ~0x4700 & 0xffff
        if a != a or b != b:
            sw |= 0x4500 | (0 if quiet else 1)
        elif a < b:
            sw |= 0x100
        elif a == b:
            sw |= 0x4000
        self.s.f["fpu_sw"] = sw

    def c_fcom(self, c, a, b):
        self.fcom_common(convert(self.ev(a), "double").v, convert(self.ev(b), "double").v, False)

    def c_fucom(self, c, a, b):
        self.fcom_common(convert(self.ev(a), "double").v, convert(self.ev(b), "double").v, True)

    def fcomi_common(self, a, b, quiet):
        f = self.s.f
        f["eflags_of"] = f["eflags_sf"] = f["eflags_af"] = 0
        if a != a or b != b:
            f["eflags_zf"] = f["eflags_pf"] = f["eflags_cf"] = 1
            if not quiet:
                f["fpu_sw"] |= 1
        else:
            f["eflags_pf"] = 0
            f["eflags_zf"] = int(a == b)
            f["eflags_cf"] = int(a < b)

    def c_fcomi(self, c, a, b):
        self.fcomi_common(convert(self.ev(a), "double").v, convert(self.ev(b), "double").v, False)

    def c_fucomi(self, c, a, b):
        self.fcomi_common(convert(self.ev(a), "double").v, convert(self.ev(b), "double").v, True)

    def fto_int(self, x, bits):
        lo, hi = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
        if x == x and not math.isinf(x):
            r = float(round(x))  # round half to even: RC = nearest
            if lo <= r <= hi:
                return int(r)
        self.s.f["fpu_sw"] |= 1
        return lo

    def fist(self, bits):
        t = self.read(self.top())
        v = self.s.a["st_bits"][t]
        v = v - (1 << 64) if v >> 63 else v
        if self.s.a["st_exact"][t] and -(1 << (bits - 1)) <= v < (1 << (bits - 1)):
            return v
        return self.fto_int(self.s.a["st"][t], bits)

    def c_fist_i16(self, c):
        return V(self.fist(16), "int16_t")

    def c_fist_i32(self, c):
        return V(self.fist(32), "int32_t")

    def c_fist_i64(self, c):
        return V(self.fist(64), "int64_t")

    def c_fto_i64(self, c, v):
        return V(self.fto_int(convert(self.ev(v), "double").v, 64), "int64_t")

    def c_recomp_return(self, c):
        raise Return()

    def c_RECOMP_BACKEDGE(self, c):
        return V(0, "int")

    def c_recomp_x87_ftol2(self, c, v, exact, bits):
        """runtime/x87_leaves.h, line for line."""
        x = convert(self.ev(v), "double").v
        ex, b = truth(self.ev(exact)), convert(self.ev(bits), "uint64_t").v
        vf = struct.unpack("<I", struct.pack("<f", f32(x)))[0]
        r = b if ex else self.fto_int(x, 64) & 0xffffffffffffffff
        eax, edx, ecx = r & 0xffffffff, r >> 32, self.s.a["r"][1]
        if not (eax == 0 and (edx & 0x7fffffff) == 0):
            ri = float(r - (1 << 64) if r >> 63 else r)
            diff = self.c_fx87(None, ("num", V(x - ri, "double"))).v
            d = struct.unpack("<I", struct.pack("<f", f32(diff)))[0]
            sign = edx if eax == 0 else vf
            if sign >> 31:
                s = (d ^ 0x80000000) + 0x7fffffff
                ecx = s & 0xffffffff
                lo = eax + (s >> 32)
                eax = lo & 0xffffffff
                edx = (edx + (lo >> 32)) & 0xffffffff
            else:
                s = d + 0x7fffffff
                ecx = s & 0xffffffff
                borrow = s >> 32
                b2 = 1 if eax < borrow else 0
                eax = (eax - borrow) & 0xffffffff
                edx = (edx - b2) & 0xffffffff
        self.s.a["r"][0], self.s.a["r"][2], self.s.a["r"][1] = eax, edx, ecx


CONSTS = {"R_EAX": V(0, "int"), "R_ECX": V(1, "int"), "R_EDX": V(2, "int"), "R_EBX": V(3, "int"),
          "R_ESP": V(4, "int"), "R_EBP": V(5, "int"), "R_ESI": V(6, "int"), "R_EDI": V(7, "int"),
          "c": V(0, "int")}
