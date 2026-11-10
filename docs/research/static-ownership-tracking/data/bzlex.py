#!/usr/bin/env python3
"""bzlex — a coarse lexer + declaration/statement splitter for Bezel source text.

Not a parser of the language; a token stream with enough structure (class bodies, member
signatures, statement spans, assignment shapes, call shapes) for the trust-admission scanner
(scan_trust.py) to find K4.3 store sites and resolve the declared types of parameters and
fields. Comments and string contents are dropped; interpolation holes are kept as tokens.
"""
import re

KEYWORDS = set("""abstract and as async await base bool break byte case catch char choice class closed
const constructor continue decimal default destructor do double elif else end ensures
enum extension extern false finally float for if import in int interface internal
invariant is lambda long method mod module move nameof new not null or out owned
override private property protected public readonly record ref requires return sbyte
sealed short static string then this throw true try type typeof uint ulong ushort
using var virtual when where while with yield scoped pure get set operator""".split())

TOK_RE = re.compile(r"""
   (?P<ws>\s+)
 | (?P<lcom>//[^\n]*)
 | (?P<bcom>/\*.*?\*/)
 | (?P<str>\$?@?"(?:""|\\.|[^"\\])*")
 | (?P<chr>'(?:\\.|[^'\\])+')
 | (?P<num>\d[\w.]*)
 | (?P<id>[A-Za-z_][A-Za-z0-9_]*)
 | (?P<op>:=|<<=|>>=|\?\?=|mod=|\+=|-=|\*=|/=|&=|\|=|\^=|=>|->|<>|<=|>=|\?\.|\?\(|\.\.<|\.\.|\?\?|\+\+|--|[()\[\]{}<>.,;:=+\-*/&|^~!?@$])
""", re.S | re.X)

class Tok:
    __slots__ = ("kind", "text", "line")
    def __init__(self, kind, text, line):
        self.kind = kind; self.text = text; self.line = line
    def __repr__(self):
        return f"{self.kind}:{self.text}@{self.line}"

def lex(src):
    toks = []
    line = 1
    pos = 0
    n = len(src)
    while pos < n:
        m = TOK_RE.match(src, pos)
        if not m:
            pos += 1
            continue
        kind = m.lastgroup
        text = m.group(kind)
        if kind in ("ws", "lcom", "bcom"):
            line += text.count("\n")
        elif kind == "str":
            # keep interpolation holes as identifier tokens so member accesses inside are visible
            toks.append(Tok("str", '""', line))
            if text.startswith("$"):
                for hole in re.findall(r"\{([^{}]*)\}", text):
                    for hm in TOK_RE.finditer(hole):
                        if hm.lastgroup in ("id", "op", "num"):
                            toks.append(Tok(hm.lastgroup, hm.group(hm.lastgroup), line))
            line += text.count("\n")
        else:
            toks.append(Tok(kind, text, line))
        pos = m.end()
    return toks

# ------------------------------------------------------------------ type knowledge
VALUE_TYPES = set("""int uint long ulong short ushort byte sbyte bool char double float decimal string Type
Span ReadOnlySpan ArraySpan ReadOnlyArraySpan ReadOnlyStringSpan DictionaryEntry CancellationToken TimeSpan
DateTime DateTimeOffset Guid Range SlotHandle IntPtr UIntPtr Int32 Int64 String Boolean Double Single Decimal
Char Byte SByte Int16 UInt16 UInt32 UInt64 Version KeyValuePair Result Pair Entry Unit Instant Duration
Index ParseResult Ordering""".split())
BORROWING_COLLECTIONS = set("Array LinkedList Dictionary Set Stack Queue SortedDictionary SortedSet Counter IList IDictionary ISet ICollection IReadOnlyList".split())
OWNING_COLLECTIONS = set("OwningArray OwningLinkedList OwningDictionary OwningSet OwningStack OwningQueue OwningSlotMap".split())
STORE_METHODS = {"Add", "Insert", "Push", "Enqueue", "AddFirst", "AddLast", "AddBefore", "AddAfter", "AddRange", "TryAdd", "GetOrAdd"}

def type_head(t):
    """'Customer?' -> 'Customer'; 'Array<Item>' -> 'Array'; '(int)->bool' -> '->'; '[4]Item' -> '[]'"""
    t = t.strip()
    if not t:
        return ""
    if "->" in t and not t.startswith("Array<") and t.count("->") >= 1 and (t.startswith("(") or t.startswith("lambda") or "->" in t.split("<")[0]):
        return "->"
    if t.startswith("["):
        return "[]"
    t = t.rstrip("?")
    m = re.match(r"[A-Za-z_][A-Za-z0-9_.]*", t)
    return m.group(0).split(".")[-1] if m else t

def type_args(t):
    """'Dictionary<string, Item>' -> ['string', 'Item']"""
    i = t.find("<")
    if i < 0:
        return []
    depth = 0; cur = ""; out = []
    for ch in t[i+1:]:
        if ch == "<": depth += 1
        elif ch == ">":
            if depth == 0:
                out.append(cur.strip()); break
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip()); cur = ""; continue
        cur += ch
    return out

# ------------------------------------------------------------------ coarse structure
class Param:
    def __init__(self, name, typ, mods):
        self.name = name; self.typ = typ; self.mods = mods  # mods: set of owned/scoped/ref/out
    def __repr__(self):
        return f"{' '.join(sorted(self.mods))} {self.name}: {self.typ}".strip()

class Member:
    def __init__(self, kind, name, cls, line):
        self.kind = kind      # field | prop | ctor | method | dtor | indexer
        self.name = name
        self.cls = cls
        self.line = line
        self.params = []
        self.mods = set()
        self.typ = None       # field/property type, method return type
        self.body = []        # tokens of the body (for ctor/method/prop accessors)
        self.init = []        # tokens of a field initializer
        self.static = False
    def __repr__(self):
        return f"{self.cls.name}.{self.name}({self.kind})"

class ClassDecl:
    def __init__(self, kind, name, line):
        self.kind = kind      # class | record | interface | choice | enum | extension
        self.name = name
        self.line = line
        self.primary = []     # primary-constructor params
        self.members = []
        self.bases = []
    def fields(self):
        return [m for m in self.members if m.kind in ("field", "prop")]
    def field(self, name):
        for m in self.members:
            if m.kind in ("field", "prop") and m.name == name:
                return m
        for p in self.primary:
            if p.name == name:
                m = Member("field", name, self, self.line); m.typ = p.typ; m.mods = set(p.mods) | {"primary"}
                return m
        return None

def split_params(toks):
    """tokens between ( and ) -> list of Param. handles nested <> and () and defaults."""
    params = []; cur = []; depth = 0
    for t in toks:
        if t.text in ("(", "[", "<"): depth += 1
        elif t.text in (")", "]", ">"): depth -= 1
        if t.text == "," and depth == 0:
            if cur: params.append(cur)
            cur = []
        else:
            cur.append(t)
    if cur: params.append(cur)
    out = []
    for p in params:
        mods = set(); i = 0
        while i < len(p) and p[i].text in ("owned", "scoped", "ref", "out"):
            mods.add(p[i].text); i += 1
        if i < len(p) and p[i].kind == "id" and i + 1 < len(p) and p[i+1].text == ":":
            name = p[i].text
            typ_toks = []
            j = i + 2
            while j < len(p) and p[j].text not in (":=", "when"):
                typ_toks.append(p[j].text); j += 1
            typ = "".join(typ_toks)
            if typ_toks and typ_toks[0] == "owned":
                mods.add("owned"); typ = "".join(typ_toks[1:])
            out.append(Param(name, typ, mods))
    return out

STMT_START = {"var", "using", "const", "if", "for", "while", "return", "throw", "case", "try", "do", "_", "await", "yield", "break", "continue", "lambda"}

def _skip_type(toks, i):
    """from index i, consume a type spelling; return (type_text, next_index)."""
    parts = []; depth = 0; last = None
    while i < len(toks):
        t = toks[i].text; k = toks[i].kind
        if t == "<": depth += 1
        elif t == ">": depth -= 1
        elif depth == 0 and t in (":=", ";", "=>", ")", ",", "where", "requires", "ensures", "get", "set", "private", "public", "internal", "protected", "end"):
            break
        if depth == 0 and (t in STMT_START or (k == "id" and last is not None and (last.kind == "id" or last.text in (">", "?", "]", ")")))):
            break
        parts.append(t); last = toks[i]; i += 1
        if depth == 0 and t == "?" and i < len(toks) and toks[i].text in (":=", ";", "=>", ")", ","):
            break
    return "".join(parts), i

def parse_unit(src):
    """Return (classes, toks). Coarse: class/record/choice/interface/enum decls with members."""
    toks = lex(src)
    classes = []
    i = 0; n = len(toks)
    stack = []  # (ClassDecl, end_marker)
    cur_cls = None
    def at(k): return toks[k].text if k < n else ""
    while i < n:
        t = toks[i]
        # type declarations
        if t.text in ("class", "record", "interface", "choice", "enum", "extension") and (i == 0 or toks[i-1].text not in (".", "typeof", "nameof", "where", ":")) and at(i+1) and toks[i+1].kind == "id":
            # 'where T: class' is excluded by the ':' check; 'is class' not a form
            if t.text == "class" and i > 0 and toks[i-1].text == ":":
                i += 1; continue
            cd = ClassDecl(t.text, toks[i+1].text, t.line)
            j = i + 2
            if at(j) == "<":
                d = 0
                while j < n:
                    if toks[j].text == "<": d += 1
                    elif toks[j].text == ">":
                        d -= 1
                        if d == 0: j += 1; break
                    j += 1
            if at(j) == "(":
                d = 0; k = j
                while k < n:
                    if toks[k].text == "(": d += 1
                    elif toks[k].text == ")":
                        d -= 1
                        if d == 0: break
                    k += 1
                cd.primary = split_params(toks[j+1:k])
                j = k + 1
            if at(j) == ":":
                k = j + 1
                while k < n and toks[k].text not in ("where", "end") and not (toks[k].kind == "id" and toks[k].text in ("public", "private", "internal", "protected", "method", "property", "constructor", "destructor", "static", "abstract", "virtual", "override", "owned", "scoped", "readonly", "async", "pure", "invariant", "polymorphic")):
                    if toks[k].kind == "id" and toks[k].text not in ("where",):
                        cd.bases.append(toks[k].text)
                    k += 1
                    if toks[k-1].line != toks[k].line if k < n else True:
                        pass
                j = k
            # positional record ending in ';'
            if t.text == "record" and at(j) == ";":
                classes.append(cd); i = j + 1; continue
            classes.append(cd)
            if t.text != "extension":
                stack.append(cd); cur_cls = cd
            i = j; continue
        if cur_cls is not None and t.text in ("public", "private", "internal", "protected", "destructor", "static", "invariant", "polymorphic") or (cur_cls is not None and t.text in ("method", "property", "constructor") and cur_cls.kind == "interface"):
            # member header
            j = i; mods = set(); static = False
            while j < n and toks[j].text in ("public", "private", "internal", "protected", "static", "async", "pure", "abstract", "virtual", "override", "sealed", "owned", "scoped", "readonly"):
                if toks[j].text == "static": static = True
                mods.add(toks[j].text); j += 1
            kw = at(j)
            if kw == "constructor" or (kw == "constructor" and static):
                m = Member("ctor", "constructor", cur_cls, toks[j].line)
                k = j + 1
                if at(k) == "(":
                    d = 0; e = k
                    while e < n:
                        if toks[e].text == "(": d += 1
                        elif toks[e].text == ")":
                            d -= 1
                            if d == 0: break
                        e += 1
                    m.params = split_params(toks[k+1:e]); k = e + 1
                # body until 'end constructor' / 'end' at depth 0
                b, k = _body_until_end(toks, k)
                m.body = b; m.mods = mods; m.static = static
                cur_cls.members.append(m); i = k; continue
            if kw == "destructor":
                m = Member("dtor", "destructor", cur_cls, toks[j].line)
                b, k = _body_until_end(toks, j + 1)
                m.body = b; cur_cls.members.append(m); i = k; continue
            if kw == "method":
                k = j + 1
                name = at(k)
                if name == "operator":
                    name = "operator" + at(k+1); k += 1
                k += 1
                if at(k) == "<":
                    d = 0
                    while k < n:
                        if toks[k].text == "<": d += 1
                        elif toks[k].text == ">":
                            d -= 1
                            if d == 0: k += 1; break
                        k += 1
                m = Member("method", name, cur_cls, toks[j].line); m.mods = mods; m.static = static
                if at(k) == "(":
                    d = 0; e = k
                    while e < n:
                        if toks[e].text == "(": d += 1
                        elif toks[e].text == ")":
                            d -= 1
                            if d == 0: break
                        e += 1
                    m.params = split_params(toks[k+1:e]); k = e + 1
                if at(k) == ":":
                    typ, k = _skip_type(toks, k + 1); m.typ = typ
                # skip where/requires/ensures clauses
                while at(k) in ("where", "requires", "ensures"):
                    if at(k) == "where":
                        while k < n and toks[k].text not in ("=>", ";", "requires", "ensures") and not _starts_statement(toks, k): k += 1
                    else:
                        k += 1
                        d = 0
                        while k < n:
                            if toks[k].text == "(": d += 1
                            elif toks[k].text == ")":
                                d -= 1
                                if d == 0: k += 1; break
                            k += 1
                if at(k) == ";":
                    cur_cls.members.append(m); i = k + 1; continue
                if at(k) == "=>":
                    b, k = _expr_body(toks, k + 1)
                    m.body = b; cur_cls.members.append(m); i = k; continue
                b, k = _body_until_end(toks, k)
                m.body = b; cur_cls.members.append(m); i = k; continue
            if kw == "property":
                k = j + 1
                if at(k) == "[":
                    d = 0; e = k
                    while e < n:
                        if toks[e].text == "[": d += 1
                        elif toks[e].text == "]":
                            d -= 1
                            if d == 0: break
                        e += 1
                    m = Member("indexer", "[]", cur_cls, toks[j].line); m.params = split_params(toks[k+1:e]); k = e + 1
                else:
                    m = Member("prop", at(k), cur_cls, toks[j].line); k += 1
                m.mods = mods; m.static = static
                if at(k) == ":":
                    typ, k = _skip_type(toks, k + 1); m.typ = typ
                if at(k) == ";":
                    cur_cls.members.append(m); i = k + 1; continue
                if at(k) in ("private", "internal", "protected") and at(k+1) == "set":
                    k += 2
                    if at(k) == ";": cur_cls.members.append(m); i = k + 1; continue
                if at(k) == ":=":
                    b, k = _expr_body(toks, k + 1); m.init = b
                    cur_cls.members.append(m); i = k; continue
                if at(k) == "=>":
                    b, k = _expr_body(toks, k + 1); m.body = b
                    cur_cls.members.append(m); i = k; continue
                # block property: get/set accessors until end
                b, k = _body_until_end(toks, k)
                m.body = b; cur_cls.members.append(m); i = k; continue
            if kw in ("invariant", "polymorphic"):
                k = j + 1
                while k < n and toks[k].text != ";": k += 1
                i = k + 1; continue
            # field: [mods] name : Type [:= init] ;
            if toks[j].kind == "id" and at(j+1) == ":":
                m = Member("field", toks[j].text, cur_cls, toks[j].line); m.mods = mods; m.static = static
                typ, k = _skip_type(toks, j + 2); m.typ = typ
                if at(k) == ":=":
                    b, k = _expr_body(toks, k + 1); m.init = b
                elif at(k) == ";":
                    k += 1
                cur_cls.members.append(m); i = k; continue
            if toks[j].kind == "id" and at(j+1) in ("record", "enum", "class", "choice", "interface"):
                i = j; continue
            if at(j) in ("record", "enum", "class", "choice", "interface"):
                i = j; continue
            i = j + 1 if j > i else i + 1; continue
        if t.text == "end" and stack and at(i+1) == stack[-1].name:
            stack.pop(); cur_cls = stack[-1] if stack else None; i += 2; continue
        if t.text == "end" and stack and at(i+1) == ";":
            # a bare end; closes the innermost open construct; we only pop when the body parse left us here
            stack.pop(); cur_cls = stack[-1] if stack else None; i += 2; continue
        i += 1
    return classes, toks

def _starts_statement(toks, k):
    return toks[k].text in ("var", "using", "const", "if", "for", "while", "return", "throw", "case", "try", "do", "_", "await", "yield") or (toks[k].kind == "id" and k + 1 < len(toks) and toks[k+1].text in (":=", "(", "."))

OPENERS = {"if": "if", "while": "while", "for": "for", "case": "case", "try": "try", "do": "do"}

def _body_until_end(toks, k):
    """Consume statements until the matching 'end' of the member; return (body_tokens, index after 'end X;')."""
    n = len(toks); depth = 0; start = k
    while k < n:
        t = toks[k].text
        if t in ("if", "while", "for", "case", "try") and not (k > 0 and toks[k-1].text in ("(", "else", ":=", "=>", "return", ",", "and", "or", "not", "then", "?", "??")):
            depth += 1
        elif t == "do":
            # `do` opens a block only when not part of if/while/for/case-arm/lambda headers
            prev = toks[k-1].text if k > 0 else ""
            # heuristics: 'while c do', 'for … do', '=> do', ', do' (case arm), 'lambda() => do' all open; a bare 'do' statement opens too.
            if prev in ("then", "else", "=>", ",", ";", "end", "(") or prev == "do":
                depth += 1
            elif prev not in ("end",):
                # 'while x do' / 'for .. do' — these are already counted by the while/for opener
                pass
        elif t == "lambda":
            pass
        elif t == "end":
            if depth == 0:
                # end of member: consume optional label and ';'
                j = k + 1
                if j < n and (toks[j].kind == "id" or toks[j].text in ("constructor", "destructor", "operator", "if", "while", "for", "case", "try", "do")):
                    j += 1
                    if toks[j-1].text == "operator": j += 1
                if j < n and toks[j].text == ";": j += 1
                return toks[start:k], j
            depth -= 1
            # a block-form `do … end` inside a lambda or case arm has no ';'; skip label if any
            j = k + 1
            if j < n and toks[j].text in ("if", "while", "for", "case", "try", "do") or (j < n and toks[j].kind == "id" and j + 1 < n and toks[j+1].text == ";" and toks[j].text not in KEYWORDS):
                k = j
        k += 1
    return toks[start:k], k

def _expr_body(toks, k):
    """an expression body up to the terminating ';' at depth 0 (handles `=> do … end` blocks)."""
    n = len(toks); depth = 0; start = k
    if k < n and toks[k].text == "do":
        b, k2 = _body_until_end(toks, k + 1)
        if k2 < n and toks[k2].text == ";": k2 += 1
        return toks[start:k2], k2
    while k < n:
        t = toks[k].text
        if t in ("(", "[", "{"): depth += 1
        elif t in (")", "]", "}"): depth -= 1
        elif t == ";" and depth <= 0:
            return toks[start:k], k + 1
        k += 1
    return toks[start:k], k

def statements(body):
    """split a body token list into statements (top-level only; nested blocks kept inline)."""
    out = []; cur = []; depth = 0
    for t in body:
        if t.text in ("(", "[", "{"): depth += 1
        elif t.text in (")", "]", "}"): depth -= 1
        cur.append(t)
        if t.text == ";" and depth <= 0:
            out.append(cur); cur = []
    if cur: out.append(cur)
    return out
