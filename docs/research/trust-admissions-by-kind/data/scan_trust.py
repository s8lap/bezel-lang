#!/usr/bin/env python3
"""scan_trust.py — find K4.3 trust-admission store sites in Bezel corpora and classify them by kind.

Usage:
  python3 -I scan_trust.py --head <repo-root> --docs <exported-root> --out <dir>

Writes <out>/hits.tsv (one row per store site), <out>/units.tsv (per-unit counts) and
<out>/summary.txt. Classification is syntactic (see README §data): every hit's kind rests on
the call sites the scanner can see inside the same unit, and the `evidence` column says which.
"""
import argparse, os, re, sys, glob
from collections import Counter, defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bzlex as L

# ----------------------------------------------------------------------------- unit collection
RAW_RE = re.compile(r'R"([A-Za-z]*)\((.*?)\)\1"', re.S)

def collect_units(head, docs):
    units = []  # (corpus, path, text)
    for p in sorted(glob.glob(os.path.join(head, "tests/conformance/rules/cases/*.bzl"))):
        units.append(("rules", p, open(p, encoding="utf-8", errors="replace").read()))
    for p in sorted(glob.glob(os.path.join(head, "tests/conformance/rules/cases/*/*.bzl"))):
        units.append(("rules", p, open(p, encoding="utf-8", errors="replace").read()))
    for p in sorted(glob.glob(os.path.join(docs, "tests/conformance/cases/*.bzl"))):
        units.append(("docs", p, open(p, encoding="utf-8", errors="replace").read()))
    for p in sorted(glob.glob(os.path.join(head, "tests/lit/**/*.bzl"), recursive=True)):
        units.append(("lit", p, open(p, encoding="utf-8", errors="replace").read()))
    for p in sorted(glob.glob(os.path.join(head, "tests/**/*.cpp"), recursive=True)):
        txt = open(p, encoding="utf-8", errors="replace").read()
        parts = [m.group(2) for m in RAW_RE.finditer(txt)]
        parts = [x for x in parts if re.search(r"\b(class|method|record|module)\b", x)]
        if parts:
            units.append(("gtest", p, "\n".join(parts)))
    return units

# ----------------------------------------------------------------------------- helpers
def is_class_typed(typ, kinds):
    """declared type -> True if class or interface typed (a borrow carrier)."""
    if not typ:
        return None
    h = L.type_head(typ)
    if h in ("->", "[]", "") or h in L.VALUE_TYPES:
        return False
    if h in ("Task", "OwningTask"):
        return "task"
    k = kinds.get(h)
    if k in ("record", "enum", "choice"):
        return False
    return True

def elem_type_of(typ):
    """element type of a collection declaration: the single arg, or the value arg of a dictionary."""
    h = L.type_head(typ); args = L.type_args(typ)
    if not args:
        return None
    if h in ("Dictionary", "SortedDictionary", "OwningDictionary", "IDictionary"):
        return args[1] if len(args) > 1 else None
    return args[0]

def key_type_of(typ):
    h = L.type_head(typ); args = L.type_args(typ)
    if h in ("Dictionary", "SortedDictionary", "OwningDictionary", "IDictionary", "Counter") and args:
        return args[0]
    return None

class Scope:
    def __init__(self, cls, member, classes):
        self.cls = cls; self.member = member; self.classes = classes
        self.params = {p.name: p for p in member.params} if member else {}
        self.primary = {p.name: p for p in cls.primary} if cls else {}
        self.locals = {}   # name -> (declkind, init_tokens, declared_type)
        if member and member.kind == "prop" and member.body:
            # block-form setter has an implicit `value` parameter
            self.params["value"] = L.Param("value", member.typ or "", set())

    def declare_locals(self, body):
        toks = body; n = len(toks)
        for i, t in enumerate(toks):
            if t.text in ("var", "const") and i + 1 < n and toks[i+1].kind == "id":
                name = toks[i+1].text
                kind = "local"
                if i > 0 and toks[i-1].text == "using": kind = "usingvar"
                if i > 0 and toks[i-1].text == "for": kind = "loopvar"
                if i > 1 and toks[i-1].text == "var" and toks[i-2].text == "out": kind = "outvar"
                typ = None; init = []
                j = i + 2
                if j < n and toks[j].text == ":":
                    typ, j = L._skip_type(toks, j + 1)
                if j < n and toks[j].text == ":=":
                    k = j + 1; d = 0
                    while k < n:
                        if toks[k].text in ("(", "[", "{"): d += 1
                        elif toks[k].text in (")", "]", "}"): d -= 1
                        elif toks[k].text == ";" and d <= 0: break
                        k += 1
                    init = toks[j+1:k]
                    if typ is None and init and init[0].text == "new" and len(init) > 1:
                        typ = init[1].text
                    if typ is None and init and init[0].text == "move" and len(init) > 1 and init[1].text in self.locals:
                        typ = self.locals[init[1].text][2]
                if j < n and toks[j].text == "in":
                    kind = "loopvar"
                if name not in self.locals:
                    self.locals[name] = (kind, init, typ)
            if t.text == "catch" or (t.text == "lambda"):
                pass

    def provenance(self, expr):
        """classify an expression (token list) as a borrow source.
        returns (kind, root, detail) with kind in:
          this | param | ownedparam | scopedparam | primary | field | staticfield | local | usingvar | loopvar
          | fresh | move | null | call | staticcall | lambda | value | unknown ; detail 'path' when a member chain
        """
        e = [t for t in expr]
        if not e:
            return ("unknown", None, "")
        # strip trailing '!'
        while e and e[-1].text == "!": e = e[:-1]
        if not e:
            return ("unknown", None, "")
        if e[0].text == "lambda" or e[0].text == "async":
            return ("lambda", None, "")
        if e[0].text == "new":
            return ("fresh", e[1].text if len(e) > 1 else None, "")
        if e[0].text == "move":
            return ("move", e[1].text if len(e) > 1 else None, "")
        if e[0].text == "null":
            return ("null", None, "")
        if e[0].text == "[":
            return ("fresh", "literal", "")
        if e[0].kind in ("str", "num") or e[0].text in ("true", "false", "-", "("):
            return ("value", None, "")
        if e[0].text == "await":
            return self.provenance(e[1:])
        # a member chain: root (. seg)* possibly with calls/indexers
        root = e[0]
        path = len(e) > 1
        has_call = any(t.text == "(" for t in e[1:])
        if root.text == "this":
            if not path: return ("this", "this", "")
            # this.f, this.f.g, this.M()
            if len(e) >= 3 and e[1].text == ".":
                f = self.cls.field(e[2].text) if self.cls else None
                if f is not None and len(e) == 3:
                    return self._field_kind(f, "")
                return ("this", "this", "path")
            return ("this", "this", "path")
        if root.text == "base":
            return ("this", "this", "path")
        if root.kind == "id":
            name = root.text
            if name in self.params:
                p = self.params[name]
                if "owned" in p.mods: return ("ownedparam", name, "path" if path else "")
                if "scoped" in p.mods: return ("scopedparam", name, "path" if path else "")
                if "ref" in p.mods or "out" in p.mods: return ("refparam", name, "path" if path else "")
                if L.type_head(p.typ) == "->":
                    return ("funcparam", name, "path" if path else "")
                return ("param", name, "path" if path else "")
            if name in self.primary:
                p = self.primary[name]
                if "owned" in p.mods: return ("ownedparam", name, "path" if path else "")
                if "scoped" in p.mods: return ("scopedparam", name, "path" if path else "")
                return ("primary", name, "path" if path else "")
            if name in self.locals:
                k = self.locals[name][0]
                return (k, name, "path" if path else "")
            f = self.cls.field(name) if self.cls else None
            if f is not None:
                return self._field_kind(f, "path" if path else "")
            # a type name (static call / static field / enum constant / nested type)
            if path and len(e) >= 3 and e[1].text == ".":
                if name[:1].isupper():
                    if has_call: return ("staticcall", name, "path")
                    return ("staticfield", name, "path")
            if has_call and not path:
                return ("call", name, "")
            if path:
                return ("unknown", name, "path")
            return ("unknown", name, "")
        return ("unknown", None, "")

    def _field_kind(self, f, detail):
        if f.static: return ("staticfield", f.name, detail)
        if "owned" in f.mods or L.type_head(f.typ or "") in L.OWNING_COLLECTIONS: return ("ownedfield", f.name, detail)
        if "scoped" in f.mods: return ("scopedfield", f.name, detail)
        return ("field", f.name, detail)

BORROW_SOURCES = {"param", "primary", "this", "field", "ownedfield"}

def split_args(toks):
    """tokens between ( and ) -> list of argument token lists (depth-aware); strips 'scoped'/'move' markers? no: keep."""
    args = []; cur = []; d = 0
    for t in toks:
        if t.text in ("(", "[", "{", "<"): d += 1
        elif t.text in (")", "]", "}", ">"): d -= 1
        if t.text == "," and d == 0:
            args.append(cur); cur = []
        else:
            cur.append(t)
    if cur: args.append(cur)
    # named arguments `name := expr`
    out = []
    for a in args:
        if len(a) > 2 and a[1].text == ":=":
            a = a[2:]
        out.append(a)
    return out

def find_calls(toks):
    """yield (callee_chain_tokens, args_token_lists, index_of_open_paren) for every call in toks."""
    n = len(toks); i = 0
    while i < n:
        if toks[i].text == "(" and i > 0 and (toks[i-1].kind == "id" or toks[i-1].text == ">"):
            # walk back over a member chain / generic args to find its root
            j = i - 1
            if toks[j].text == ">":
                d = 0
                while j >= 0:
                    if toks[j].text == ">": d += 1
                    elif toks[j].text == "<":
                        d -= 1
                        if d == 0: j -= 1; break
                    j -= 1
            # now toks[j] is an identifier (method name)
            if j < 0 or toks[j].kind != "id":
                i += 1; continue
            k = j
            while k - 2 >= 0 and toks[k-1].text in (".", "?.") and (toks[k-2].kind == "id" or toks[k-2].text in ("this", "base", ")", "]", "!")):
                if toks[k-2].text in (")", "]"):
                    # skip a balanced group
                    d = 0; m = k - 2
                    while m >= 0:
                        if toks[m].text in (")", "]"): d += 1
                        elif toks[m].text in ("(", "["):
                            d -= 1
                            if d == 0: break
                        m -= 1
                    k = m
                    # the group's head
                    while k - 1 >= 0 and toks[k-1].kind == "id": k -= 1
                    continue
                k -= 2
                if toks[k].text == "!": k -= 1
            chain = toks[k:j+1]
            # args
            d = 0; e = i
            while e < n:
                if toks[e].text == "(": d += 1
                elif toks[e].text == ")":
                    d -= 1
                    if d == 0: break
                e += 1
            args = split_args(toks[i+1:e])
            if chain:
                yield chain, args, i, e
        i += 1

def assignments(stmt):
    """(lhs tokens, rhs tokens) if the statement is an assignment `lhs := rhs;` (not a declaration)."""
    if not stmt or stmt[0].text in ("var", "using", "const", "for", "if", "while", "return", "case", "while"):
        return None
    for i, t in enumerate(stmt):
        if t.text == ":=":
            lhs = stmt[:i]; rhs = stmt[i+1:]
            if rhs and rhs[-1].text == ";": rhs = rhs[:-1]
            # only simple targets: name | this.name | expr.name | expr[idx]
            return lhs, rhs
        if t.text in ("(", "=>", "lambda"):
            # an assignment inside a lambda / call is not a statement-level store of this body (still scanned via nested statements)
            if t.text == "(":
                continue
            return None
    return None

# ----------------------------------------------------------------------------- the scanner
class Hit:
    FIELDS = ["corpus", "unit", "expect", "line", "shape", "class", "member", "target", "target_type", "source", "source_kind", "source_type",
              "kind", "evidence", "callsites", "virality", "name_hint", "notes"]
    def __init__(self, **kw):
        for f in self.FIELDS: setattr(self, f, kw.get(f, ""))
    def row(self):
        return "\t".join(str(getattr(self, f)).replace("\t", " ").replace("\n", " ") for f in self.FIELDS)

def unit_kinds(classes):
    kinds = {}
    for c in classes:
        kinds[c.name] = c.kind
    return kinds

DOC_MANIFEST = {}

def unit_expect(corpus, path, text):
    if corpus == "rules":
        m = re.search(r"^//! expect:\s*(\S+)", text, re.M)
        return m.group(1) if m else "?"
    if corpus == "docs":
        return DOC_MANIFEST.get(os.path.basename(path)[:-4], "?")
    if corpus == "lit":
        first = text.split("\n", 1)[0]
        if "%not" in first: return "reject"
        return "accept"
    return "mixed"

def scan_unit(corpus, path, text, out_hits, stats):
    expect = unit_expect(corpus, path, text)
    try:
        classes, toks = L.parse_unit(text)
    except Exception as ex:  # a parser hiccup must not kill the sweep
        stats["parse_errors"] += 1
        return
    kinds = unit_kinds(classes)
    byname = {c.name: c for c in classes}
    stats["units"] += 1
    stats["classes"] += len(classes)
    hits = []
    for cls in classes:
        if cls.kind != "class":
            continue  # a record's borrows are §9.7's regime (no trust residual: rejected statically)
        # --- F3: field initializer from a primary-constructor parameter; F4: primary capture
        primary_cls = is_class_typed
        for m in cls.members:
            if m.kind in ("field", "prop") and m.init and not m.static and "owned" not in m.mods:
                ct = is_class_typed(m.typ, kinds)
                if ct is True:
                    sc = Scope(cls, None, classes)
                    pk = sc.provenance(m.init)
                    if pk[0] in ("primary",):
                        p = sc.primary[pk[1]]
                        hits.append(Hit(corpus=corpus, unit=path, line=m.line, shape="field-init", **{"class": cls.name}, member="<init>",
                                        target=m.name, target_type=m.typ, source=pk[1], source_kind="primary" + ("-path" if pk[2] else ""), source_type=p.typ))
            if m.kind == "field" and "scoped" in m.mods and not m.static:
                hits.append(Hit(corpus=corpus, unit=path, line=m.line, shape="scoped-field", **{"class": cls.name}, member="<decl>",
                                target=m.name, target_type=m.typ, source="", source_kind="", source_type="", kind="D", evidence="§28.14 quiescence residual"))
        captured = set()
        for m in cls.members:
            if not m.body or m.kind in ("field",):
                continue
            sc = Scope(cls, m, classes)
            sc.declare_locals(m.body)
            names_in_body = {t.text for t in m.body if t.kind == "id"}
            for pname, p in sc.primary.items():
                if pname in names_in_body and pname not in sc.params and pname not in sc.locals and "owned" not in p.mods and "scoped" not in p.mods:
                    ct = is_class_typed(p.typ, kinds)
                    if ct is True and (cls.name, pname) not in captured:
                        captured.add((cls.name, pname))
                        hits.append(Hit(corpus=corpus, unit=path, line=m.line, shape="primary-capture", **{"class": cls.name}, member=m.name,
                                        target="<capture:" + pname + ">", target_type=p.typ, source=pname, source_kind="primary", source_type=p.typ))
            stmts = L.statements(m.body)
            # nested statements inside blocks: statements() keeps blocks inline, so also split on inner ';' by scanning tokens linearly.
            flat = m.body
            # --- F1/F2: assignments
            for st in _all_statements(flat):
                a = assignments(st)
                if a:
                    lhs, rhs = a
                    _scan_assignment(corpus, path, cls, m, sc, lhs, rhs, kinds, byname, hits, stats)
            # --- C1/C2/T1: calls
            for chain, args, i0, i1 in find_calls(flat):
                _scan_call(corpus, path, cls, m, sc, chain, args, flat, i0, kinds, byname, hits, stats)
            # --- S1: slot-displaced (0210): `var b := x.f;` then `move x.f with` then use of b
            _scan_slot_displaced(corpus, path, cls, m, sc, flat, hits)
    # kind classification needs the whole unit
    for h in hits:
        if h.shape in ("field-store", "field-init", "primary-capture", "prop-store", "coll-store", "indexer-set", "comparer", "frame-fireandforget", "frame-store"):
            classify(h, classes, byname, kinds)
    for h in hits:
        h.expect = expect
        h.name_hint = name_hint(h)
        out_hits.append(h)
    stats["hits"] += len(hits)
    # `scoped` class-typed parameters in this unit (the DECLINED-808 ratio's denominator)
    for cls in classes:
        for m in cls.members:
            for p in m.params:
                if "scoped" in p.mods:
                    stats["scoped_params_" + ("func" if L.type_head(p.typ) == "->" else "class")] += 1
        for p in cls.primary:
            if "scoped" in p.mods:
                stats["scoped_params_" + ("func" if L.type_head(p.typ) == "->" else "class")] += 1

OWNER_NAMES = re.compile(r"owner|parent|host|container|back|outer|tree|graph|doc|root|service|svc|app|ctx|context|env|session|conn", re.I)
REGISTRY_NAMES = re.compile(r"log|registry|cache|observer|listener|subscriber|handler|sink|index|lookup|table|pool|bus|hub|store|repo", re.I)

def name_hint(h):
    n = (h.target or "") + " " + (h.source or "")
    if h.shape in ("coll-store", "indexer-set"):
        return "registry-shape"
    if OWNER_NAMES.search(n): return "owner-name"
    if REGISTRY_NAMES.search(n): return "registry-name"
    return ""

def _all_statements(flat):
    """statements at every nesting level: split on ';' at paren-depth 0, then recurse into block openers.
    We approximate: produce every ';'-terminated run, ignoring block structure, which is fine for finding `lhs := rhs;`."""
    out = []; cur = []; d = 0
    for t in flat:
        if t.text in ("(", "[", "{"): d += 1
        elif t.text in (")", "]", "}"): d -= 1
        if t.text in ("then", "else", "do", "elif") and d <= 0:
            # statement boundary inside a block header; drop the header
            cur = []; continue
        if t.text == "=>" and d <= 0:
            # a case arm `pattern => stmt` : drop the pattern
            cur = []; continue
        cur.append(t)
        if t.text == ";" and d <= 0:
            out.append(cur); cur = []
    if cur: out.append(cur)
    return out

def _scan_assignment(corpus, path, cls, m, sc, lhs, rhs, kinds, byname, hits, stats):
    if not lhs or not rhs:
        return
    # drop a leading `end` label residue etc.
    while lhs and lhs[0].text in ("end", ";", ")", "}"): lhs = lhs[1:]
    if not lhs: return
    # indexer set: recv[i] := rhs
    if lhs[-1].text == "]":
        d = 0; j = len(lhs) - 1
        while j >= 0:
            if lhs[j].text == "]": d += 1
            elif lhs[j].text == "[":
                d -= 1
                if d == 0: break
            j -= 1
        recv = lhs[:j]
        _coll_store(corpus, path, cls, m, sc, recv, [rhs], "indexer-set", kinds, byname, hits, stats, elem_index=0)
        return
    # field store: name | this.name | obj.name
    target = lhs[-1].text
    if lhs[-1].kind != "id":
        return
    src = sc.provenance(rhs)
    if len(lhs) == 1 or (len(lhs) == 3 and lhs[0].text == "this" and lhs[1].text == "."):
        if len(lhs) == 1 and (target in sc.locals or target in sc.params):
            return  # a local/parameter assignment
        f = cls.field(target)
        if f is None:
            return
        if f.static:
            return
        ft = is_class_typed(f.typ, kinds)
        if "owned" in f.mods or L.type_head(f.typ or "") in L.OWNING_COLLECTIONS:
            # a task frame stored? an owned Task field is a transfer, not a borrow; form-3 registration residual:
            if L.type_head(f.typ or "") == "Subscription" and any(t.text == "lambda" for t in rhs):
                hits.append(Hit(corpus=corpus, unit=path, line=lhs[0].line, shape="form3-registration", **{"class": cls.name}, member=m.name,
                                target=f.name, target_type=f.typ, source="", source_kind="lambda", source_type="", kind="D", evidence="§28.14 form-3 registration residual"))
            return
        if "scoped" in f.mods:
            return
        if ft == "task":
            if src[0] in BORROW_SOURCES or src[0] in ("call",):
                hits.append(Hit(corpus=corpus, unit=path, line=lhs[0].line, shape="frame-store", **{"class": cls.name}, member=m.name,
                                target=f.name, target_type=f.typ, source=src[1] or "", source_kind=src[0] + ("-path" if src[2] else ""), source_type=_src_type(sc, src)))
            return
        if ft is not True:
            return
        if src[0] in BORROW_SOURCES or src[0] in ("staticcall",) and False:
            hits.append(Hit(corpus=corpus, unit=path, line=lhs[0].line, shape="field-store", **{"class": cls.name}, member=m.name,
                            target=f.name, target_type=f.typ, source=src[1] or "", source_kind=src[0] + ("-path" if src[2] else ""), source_type=_src_type(sc, src)))
        elif src[0] in ("local", "usingvar", "loopvar", "ownedparam"):
            stats["rejected_shapes"] += 1
        elif src[0] == "scopedparam":
            stats["scoped_stores"] += 1
        elif src[0] == "funcparam":
            pass
        elif src[0] in ("call", "unknown") and src[2] == "path":
            # a borrow returned by a member call on an unknown root; count as unresolved
            stats["unresolved_rhs"] += 1
        return
    # obj.P := rhs — a property store on another object (judged as a field store of the receiver)
    if len(lhs) >= 3 and lhs[-2].text == ".":
        recv = lhs[:-2]
        rk = sc.provenance(recv)
        if rk[0] in ("unknown", "staticfield", "staticcall", "lambda", "value", "null"):
            return
        # resolve the receiver's declared type to find the property's type
        rtype = _expr_type(sc, recv, byname, kinds)
        ptype = None
        if rtype and L.type_head(rtype) in byname:
            pf = byname[L.type_head(rtype)].field(target)
            if pf is not None:
                if "owned" in pf.mods: return
                ptype = pf.typ
        if ptype is None:
            # fall back on the source's declared type
            ptype = _src_type(sc, src)
        if is_class_typed(ptype, kinds) is not True:
            return
        if src[0] in BORROW_SOURCES:
            hits.append(Hit(corpus=corpus, unit=path, line=lhs[0].line, shape="prop-store", **{"class": cls.name}, member=m.name,
                            target=f"{''.join(t.text for t in recv)}.{target}", target_type=ptype or "?", source=src[1] or "", source_kind=src[0] + ("-path" if src[2] else ""), source_type=_src_type(sc, src),
                            notes=f"receiver={rk[0]}"))

def _src_type(sc, src):
    k, name, det = src
    if det == "path":
        return "?"
    if k in ("param", "ownedparam", "scopedparam"):
        return sc.params[name].typ if name in sc.params else sc.primary[name].typ
    if k == "primary":
        return sc.primary[name].typ
    if k in ("field", "ownedfield"):
        f = sc.cls.field(name); return f.typ if f else "?"
    if k == "this":
        return sc.cls.name
    if k in ("local", "usingvar", "loopvar", "outvar"):
        return sc.locals[name][2] or "?"
    return "?"

def _expr_type(sc, expr, byname, kinds):
    """declared type of a simple expression (a name, this, or a one-hop chain) when resolvable."""
    e = [t for t in expr if t.text != "!"]
    if not e: return None
    if e[0].text == "this":
        if len(e) == 1: return sc.cls.name
        if len(e) == 3 and e[1].text == ".":
            f = sc.cls.field(e[2].text); return f.typ if f else None
        return None
    if e[0].kind == "id":
        base = None
        n = e[0].text
        if n in sc.params: base = sc.params[n].typ
        elif n in sc.primary: base = sc.primary[n].typ
        elif n in sc.locals: base = sc.locals[n][2]
        elif sc.cls.field(n) is not None: base = sc.cls.field(n).typ
        if len(e) == 1: return base
        if base and len(e) == 3 and e[1].text == ".":
            h = L.type_head(base)
            if h in byname:
                f = byname[h].field(e[2].text); return f.typ if f else None
        return None
    return None

def _coll_store(corpus, path, cls, m, sc, recv, args, shape, kinds, byname, hits, stats, elem_index=None, method=""):
    rk = sc.provenance(recv)
    rtype = _expr_type(sc, recv, byname, kinds)
    if rtype is None:
        # unresolved receiver (a call result, an unknown chain): the structural check leaves it unclassified → reported
        head = None
    else:
        head = L.type_head(rtype)
    if head is not None and head not in L.BORROWING_COLLECTIONS and head not in L.OWNING_COLLECTIONS:
        return
    if head in L.OWNING_COLLECTIONS:
        # only the key slot of an OwningDictionary is a borrow position
        if head == "OwningDictionary" and method in ("Add", "TryAdd") and args:
            kt = key_type_of(rtype)
            if is_class_typed(kt, kinds) is True:
                src = sc.provenance(args[0])
                if src[0] in BORROW_SOURCES:
                    hits.append(Hit(corpus=corpus, unit=path, line=recv[0].line if recv else 0, shape="coll-store", **{"class": cls.name}, member=m.name,
                                    target=''.join(t.text for t in recv) + ".Add(key)", target_type=rtype, source=src[1] or "", source_kind=src[0] + ("-path" if src[2] else ""), source_type=_src_type(sc, src),
                                    notes=f"receiver={rk[0]};key-slot"))
        if head == "OwningDictionary" and shape == "indexer-set":
            pass
        return
    # which argument positions are element stores
    positions = []
    if shape == "indexer-set":
        positions = [(0, elem_type_of(rtype) if rtype else None)]
    elif method in ("Add", "TryAdd") and head in ("Dictionary", "SortedDictionary", "IDictionary") and len(args) >= 2:
        positions = [(0, key_type_of(rtype) if rtype else None), (1, elem_type_of(rtype) if rtype else None)]
    elif method in ("Add", "Push", "Enqueue", "AddFirst", "AddLast") and len(args) >= 1:
        positions = [(0, elem_type_of(rtype) if rtype else None)]
    elif method in ("Insert",) and len(args) >= 2:
        positions = [(1, elem_type_of(rtype) if rtype else None)]
    elif method in ("AddBefore", "AddAfter") and len(args) >= 2:
        positions = [(1, elem_type_of(rtype) if rtype else None)]
    elif method in ("AddRange",) and len(args) >= 1:
        positions = [(0, elem_type_of(rtype) if rtype else None)]
    elif method in ("GetOrAdd",) and len(args) >= 1:
        positions = [(0, key_type_of(rtype) if rtype else None)]
    for idx, et in positions:
        if idx >= len(args): continue
        a = args[idx]
        if a and a[0].text == "scoped": a = a[1:]
        src = sc.provenance(a)
        ct = is_class_typed(et, kinds) if et else None
        if et is not None and ct is not True:
            continue
        if et is None:
            # unknown element type: fall back on the source's declared type
            st = _src_type(sc, src)
            if is_class_typed(st, kinds) is not True:
                continue
        if src[0] in BORROW_SOURCES:
            receiver_kind = rk[0]
            notes = f"receiver={receiver_kind}"
            # v0.350: a store of a parameter or `this` itself into a purely local collection is not reported
            silent = receiver_kind in ("local", "usingvar") and src[0] in ("param", "this", "primary") and not src[2]
            if silent:
                notes += ";local-collection-silent"
            hits.append(Hit(corpus=corpus, unit=path, line=recv[0].line if recv else a[0].line, shape=shape if shape == "indexer-set" else "coll-store", **{"class": cls.name}, member=m.name,
                            target=''.join(t.text for t in recv) + (f".{method}" if method else "[]"), target_type=rtype or "?", source=src[1] or "", source_kind=src[0] + ("-path" if src[2] else ""), source_type=_src_type(sc, src),
                            notes=notes))
        elif src[0] in ("local", "usingvar", "loopvar", "ownedparam"):
            stats["coll_local_sources"] += 1

def _scan_call(corpus, path, cls, m, sc, chain, args, flat, i0, kinds, byname, hits, stats):
    name = chain[-1].text
    recv = chain[:-2] if len(chain) >= 3 and chain[-2].text in (".", "?.") else []
    # collection stores
    if name in L.STORE_METHODS and recv:
        _coll_store(corpus, path, cls, m, sc, recv, args, "coll-store", kinds, byname, hits, stats, method=name)
        return
    # Task.FireAndForget(expr)
    if name == "FireAndForget" and recv and recv[-1].text == "Task" and args:
        a = args[0]
        srcs = []
        for c2, a2, j0, j1 in find_calls(a):
            r2 = c2[:-2] if len(c2) >= 3 and c2[-2].text in (".", "?.") else []
            if r2:
                pk = sc.provenance(r2)
                if pk[0] in BORROW_SOURCES: srcs.append((pk, "receiver"))
            for aa in a2:
                pk = sc.provenance(aa)
                if pk[0] in BORROW_SOURCES: srcs.append((pk, "argument"))
                if pk[0] == "lambda":
                    srcs.append((("lambda", None, ""), "capture"))
        if not srcs:
            pk = sc.provenance(a)
            if pk[0] in BORROW_SOURCES: srcs.append((pk, "operand"))
        if srcs:
            pk, role = srcs[0]
            hits.append(Hit(corpus=corpus, unit=path, line=chain[0].line, shape="frame-fireandforget", **{"class": cls.name}, member=m.name,
                            target="Task.FireAndForget", target_type="Task", source=pk[1] or "", source_kind=(pk[0] + ("-path" if pk[2] else "")) if pk[0] != "lambda" else "lambda", source_type=_src_type(sc, pk) if pk[0] != "lambda" else "", notes=f"role={role};n={len(srcs)}"))
        return
    # comparer-held: new Dictionary<..>(cmp) / new Set<..>(cmp) / SortedDictionary(cmp) / SortedSet / Counter
    if i0 > 0 and len(chain) >= 1 and chain[0].text in ("Dictionary", "Set", "SortedDictionary", "SortedSet", "Counter", "OwningDictionary", "OwningSet") and (flat[i0 - 1].text == ">" or flat[i0-1].text == chain[0].text):
        # find 'new' before the chain
        k = i0 - 1
        d = 0
        while k >= 0 and flat[k].text != "new" and k > i0 - 40:
            k -= 1
        if k >= 0 and flat[k].text == "new" and args:
            for a in args:
                pk = sc.provenance(a)
                if pk[0] in BORROW_SOURCES or pk[0] in ("local", "usingvar"):
                    st = _src_type(sc, pk)
                    if is_class_typed(st, kinds) is True or st == "?":
                        hits.append(Hit(corpus=corpus, unit=path, line=chain[0].line, shape="comparer", **{"class": cls.name}, member=m.name,
                                        target="new " + chain[0].text, target_type=chain[0].text, source=pk[1] or "", source_kind=pk[0] + ("-path" if pk[2] else ""), source_type=st, notes="comparer argument"))

def _scan_slot_displaced(corpus, path, cls, m, sc, flat, hits):
    # var b := x.f;  …  move x.f with …;  … b …
    n = len(flat)
    reads = {}
    for i in range(n - 4):
        if flat[i].text == "var" and flat[i+1].kind == "id" and flat[i+2].text == ":=" and flat[i+3].kind in ("id",) and flat[i+4].text == "." and i + 6 < n and flat[i+5].kind == "id" and flat[i+6].text == ";":
            reads[(flat[i+3].text, flat[i+5].text)] = (flat[i+1].text, i)
        if flat[i].text == "var" and flat[i+1].kind == "id" and flat[i+2].text == ":=" and flat[i+3].kind == "id" and flat[i+4].text == ";":
            # var b := _f; (a bare field read)
            reads[("this", flat[i+3].text)] = (flat[i+1].text, i)
    for i in range(n - 2):
        if flat[i].text == "move":
            # move x.f with / move _f
            if flat[i+1].kind == "id" and i + 3 < n and flat[i+2].text == "." and flat[i+3].kind == "id":
                key = (flat[i+1].text, flat[i+3].text)
            elif flat[i+1].kind == "id":
                key = ("this", flat[i+1].text)
            else:
                continue
            if key in reads:
                b, ri = reads[key]
                if ri < i and any(t.text == b for t in flat[i+1:]):
                    hits.append(Hit(corpus=corpus, unit=path, line=flat[i].line, shape="slot-displaced", **{"class": cls.name}, member=m.name,
                                    target=".".join(key), target_type="", source=b, source_kind="local-slot-borrow", source_type="", kind="S", evidence="0210 shape (transfer not verified)"))

# ----------------------------------------------------------------------------- classification by call sites
def classify(h, classes, byname, kinds):
    """Assign kind 1/2/3/4 (+U) from the call sites of the storing member visible in the unit."""
    cls = byname.get(getattr(h, "class"))
    sk = h.source_kind
    if h.shape in ("frame-fireandforget", "frame-store"):
        h.kind = "F"; h.evidence = "frame-held (not kind-classified)"; return
    if h.shape == "comparer":
        h.kind = "C"; h.evidence = "comparer-held (not kind-classified)"; return
    if sk.startswith("this"):
        # storing `this` (or a path through it) into another object: who holds the holder?
        tgt = h.target
        if h.shape in ("coll-store", "indexer-set"):
            rk = re.search(r"receiver=(\w+)", h.notes)
            rk = rk.group(1) if rk else "?"
            if rk in ("param", "primary"):
                h.kind = "2"; h.evidence = "this stored into a caller-owned collection (registry shape)"; return
            if rk in ("field", "ownedfield"):
                h.kind = "2"; h.evidence = "this stored into a collection this holds (self-registry)"; return
            if rk in ("local", "usingvar"):
                h.kind = "3"; h.evidence = "this stored into a method-local collection"; return
            h.kind = "2"; h.evidence = f"this stored into a collection (receiver={rk})"; return
        if h.shape == "prop-store":
            rk = re.search(r"receiver=(\w+)", h.notes); rk = rk.group(1) if rk else "?"
            if rk in ("ownedfield",):
                h.kind = "1"; h.evidence = "this stored into an owned child's field (back-pointer to owner)"; return
            if rk in ("param", "primary"):
                h.kind = "2"; h.evidence = "this stored into a parameter's field (holder owned elsewhere)"; return
            if rk in ("local", "usingvar"):
                h.kind = "3"; h.evidence = "this stored into a method-local holder's field"; return
            h.kind = "2"; h.evidence = f"this stored into another object's field (receiver={rk})"; return
        if h.shape == "field-store":
            h.kind = "2"; h.evidence = "this stored into own field (self-reference)"; return
    if sk.startswith("field") or sk.startswith("ownedfield"):
        # a sibling: the source is a field of this very object (owned or borrowed by this)
        if h.shape in ("coll-store", "indexer-set"):
            h.kind = "2"; h.evidence = "a field's value stored into a collection (index of own children)"; return
        if h.shape == "prop-store":
            rk = re.search(r"receiver=(\w+)", h.notes); rk = rk.group(1) if rk else "?"
            if rk in ("param", "primary", "field"):
                h.kind = "2"; h.evidence = f"a field of this stored into another object's property (receiver={rk}: holder owned elsewhere)"; return
            if rk in ("local", "usingvar"):
                h.kind = "3"; h.evidence = "a field of this stored into a method-local holder's property"; return
        h.kind = "1"; h.evidence = "sibling: a field of this stored into another field of this (same owner; disposal order decides)"; return
    if not (sk.startswith("param") or sk.startswith("primary")):
        h.kind = "U"; h.evidence = f"source kind {sk} not classified"; return
    # the stored source is a parameter of member `h.member` of class `cls`
    member_name = h.member if h.member not in ("<init>",) else "constructor"
    pname = h.source
    sites = []
    seen = set()
    _walk_callers(cls, member_name, pname, classes, byname, kinds, sites, seen, depth=0)
    # aggregate
    kinds_found = [s[0] for s in sites]
    h.callsites = ";".join(f"{s[1]}:{s[0]}" for s in sites)[:400]
    sigs = {s[2] for s in sites if s[2]}
    h.virality = 1 + len(sigs)
    dispatch = _is_dispatched(cls, member_name, byname)
    if not sites:
        if dispatch:
            h.kind = "4"; h.evidence = "no in-unit call site; member is interface/virtual (unknown-body boundary)"
        else:
            h.kind = "U"; h.evidence = "no in-unit call site (fragment / library)"
        return
    order = {"N": -2, "V": -1, "1": 0, "3": 1, "2": 2, "U": 2.5, "4": 3}
    worst = max(kinds_found, key=lambda k: order.get(k, 3))
    h.kind = worst
    ev = Counter(kinds_found)
    h.evidence = ("dispatched; " if dispatch else "") + ", ".join(f"{k}:{v}" for k, v in sorted(ev.items()))
    if dispatch and worst != "4":
        h.notes += ";dispatched-member"

def _is_dispatched(cls, member_name, byname):
    if cls is None: return False
    for m in cls.members:
        if m.name == member_name and ({"virtual", "override", "abstract"} & m.mods):
            return True
    # implements an interface member of the same name?
    for b in cls.bases:
        bc = byname.get(b)
        if bc is not None and bc.kind == "interface":
            for m in bc.members:
                if m.name == member_name: return True
    return False

def _walk_callers(cls, member_name, pname, classes, byname, kinds, sites, seen, depth):
    """find call sites of cls.member_name in the unit; classify the argument bound to pname at each."""
    if depth > 6 or cls is None:
        return
    # parameter index
    target = None
    for m in cls.members:
        if (m.kind == "ctor" and member_name == "constructor") or (m.kind == "method" and m.name == member_name) or (m.kind == "prop" and m.name == member_name):
            target = m; break
    if member_name == "constructor" and target is None and cls.primary:
        target = L.Member("ctor", "constructor", cls, cls.line); target.params = cls.primary
    if target is None:
        return
    idx = None
    for i, p in enumerate(target.params):
        if p.name == pname: idx = i; break
    if pname in {p.name for p in cls.primary} and idx is None:
        for i, p in enumerate(cls.primary):
            if p.name == pname: idx = i; break
    if idx is None and target.kind == "prop":
        idx = 0
    if idx is None:
        return
    for c2 in classes:
        for m2 in c2.members:
            if not m2.body: continue
            sc2 = Scope(c2, m2, classes); sc2.declare_locals(m2.body)
            flat = m2.body
            # statements, to find what holds a constructor result
            for chain, args, i0, i1 in find_calls(flat):
                nm = chain[-1].text
                is_ctor_call = member_name == "constructor" and nm == cls.name and i0 > 0 and _prev_is_new(flat, chain, i0)
                is_method_call = member_name != "constructor" and nm == member_name and len(chain) >= 3
                is_bare_call = member_name != "constructor" and nm == member_name and len(chain) == 1 and c2 is cls
                if not (is_ctor_call or is_method_call or is_bare_call):
                    continue
                if is_method_call:
                    recv = chain[:-2]
                    rtype = _expr_type(sc2, recv, byname, kinds)
                    if rtype is not None and L.type_head(rtype) != cls.name and L.type_head(rtype) not in cls.bases and not _derives(byname.get(L.type_head(rtype)), cls.name, byname):
                        # a call of a same-named member on another type
                        if L.type_head(rtype) in byname: continue
                if idx >= len(args):
                    continue
                a = args[idx]
                if a and a[0].text in ("scoped", "move"): a = a[1:]
                pk = sc2.provenance(a)
                key = (c2.name, m2.name, i0)
                if key in seen: continue
                seen.add(key)
                holder = _holder_of_call(flat, i0, i1, sc2, chain, is_ctor_call, byname, kinds)
                where = f"{c2.name}.{m2.name}@{chain[0].line}"
                k, why = _kind_at_site(pk, holder, sc2, c2, is_ctor_call, chain, byname, kinds)
                if k == "fwd":
                    # the argument is a plain parameter of the caller: recurse upward
                    sig = f"{c2.name}.{m2.name}({pk[1]})"
                    before = len(sites)
                    _walk_callers(c2, m2.name if m2.kind != "ctor" else "constructor", pk[1], classes, byname, kinds, sites, seen, depth + 1)
                    added = sites[before:]
                    if not added:
                        dispatch = _is_dispatched(c2, m2.name if m2.kind != "ctor" else "constructor", byname)
                        sites.append(("4" if dispatch else "U", where + f"[fwd {sig}; no caller]", sig))
                    else:
                        # record the forwarding signature on each
                        for i in range(before, len(sites)):
                            s = sites[i]; sites[i] = (s[0], s[1] + f"<-{sig}", s[2] if s[2] else sig)
                        sites.append((added[-1][0], where + f"[fwd {sig}]", sig))
                else:
                    sites.append((k, where + "[" + why + "]", ""))

def _derives(c, base, byname, depth=0):
    if c is None or depth > 5: return False
    if base in c.bases: return True
    return any(_derives(byname.get(b), base, byname, depth + 1) for b in c.bases)

def _prev_is_new(flat, chain, i0):
    k = i0 - len(chain) - 1
    # skip generic args between name and (
    j = i0 - 1
    if flat[j].text == ">":
        d = 0
        while j >= 0:
            if flat[j].text == ">": d += 1
            elif flat[j].text == "<":
                d -= 1
                if d == 0: j -= 1; break
            j -= 1
        return j >= 1 and flat[j-1].text == "new" if flat[j].kind == "id" else False
    return k >= 0 and flat[k].text == "new"

def _holder_of_call(flat, i0, i1, sc, chain, is_ctor, byname, kinds):
    """what holds the result of the call (ctor) or what the receiver is (method): returns (holderkind, name)."""
    # look backwards from the start of `new C(` for `var x :=` / `using var x :=` / `_f :=` / `return` / `Add(`
    # walk back from '(' over generic args and the chain to the token before `new`
    q = i0 - 1
    if q >= 0 and flat[q].text == ">":
        d = 0
        while q >= 0:
            if flat[q].text == ">": d += 1
            elif flat[q].text == "<":
                d -= 1
                if d == 0: q -= 1; break
            q -= 1
    q -= (len(chain) - 1)
    j = q - 1
    if j >= 0 and flat[j].text == "new": j -= 1
    if is_ctor:
        if j >= 0 and flat[j].text == ":=":
            k = j - 1
            # a typed local `var h: T<..> := new …` / `using var h: T := new …`
            if k >= 0 and (flat[k].text in (">", "?") or flat[k].kind == "id"):
                q = k; d = 0
                while q >= 0 and flat[q].text != ";":
                    if flat[q].text == ">": d += 1
                    elif flat[q].text == "<": d -= 1
                    if d == 0 and flat[q].text == ":" and q >= 2 and flat[q-2].text == "var" and flat[q-1].kind == "id":
                        name = flat[q-1].text
                        if q >= 3 and flat[q-3].text == "using": return ("usingvar", name)
                        return ("local", name)
                    q -= 1
            if k >= 0 and flat[k].kind == "id":
                name = flat[k].text
                if k >= 1 and flat[k-1].text == "var":
                    if k >= 2 and flat[k-2].text == "using": return ("usingvar", name)
                    return ("local", name)
                if k >= 1 and flat[k-1].text == "const": return ("local", name)
                pk = sc.provenance([flat[k]])
                if pk[0] in ("ownedfield",): return ("ownedfield", name)
                if pk[0] in ("field",): return ("field", name)
                if pk[0] in ("local", "usingvar"): return (pk[0], name)
                if pk[0] == "staticfield": return ("static", name)
                return ("assign:" + pk[0], name)
            if k >= 2 and flat[k].kind == "id" and flat[k-1].text == "." and flat[k-2].text == "this":
                pk = sc.provenance([flat[k]])
                return (pk[0] if pk[0] in ("ownedfield", "field") else "assign", flat[k].text)
            if k >= 0 and flat[k].text == "]":
                return ("slot", "")
        if j >= 0 and flat[j].text == "return": return ("return", "")
        if j >= 0 and flat[j].text == "(":
            # an argument to a call: owned-collection Add / owned param / plain param
            # find the call name before '('
            k = j - 1
            if k >= 0 and flat[k].kind == "id":
                nm = flat[k].text
                if nm in ("Add", "Push", "Enqueue", "Insert", "AddFirst", "AddLast"):
                    r = k - 2
                    recv = []
                    while r >= 0 and (flat[r].kind == "id" or flat[r].text in (".", "this")):
                        recv.insert(0, flat[r]); r -= 1
                    pk = sc.provenance(recv) if recv else ("unknown", None, "")
                    return ("coll:" + pk[0], ''.join(t.text for t in recv))
                return ("arg:" + nm, "")
        if j >= 0 and flat[j].text == ",": return ("arg", "")
        if j >= 0 and flat[j].text == "=>": return ("temp-or-lambda", "")
        if j >= 0 and flat[j].text in (";", "then", "do", "else") or j < 0: return ("temp", "")
        return ("other:" + flat[j].text, "")
    else:
        recv = chain[:-2]
        pk = sc.provenance(recv)
        return ("recv:" + pk[0], ''.join(t.text for t in recv))

def _kind_at_site(pk, holder, sc, c2, is_ctor, chain, byname, kinds):
    """kind of one call site given the argument's provenance and the holder of the result/receiver."""
    hk, hn = holder
    src = pk[0]
    if src in ("param", "primary") and not pk[2]:
        return ("fwd", "forwarded parameter")
    if src in ("param", "primary") and pk[2]:
        # a path through a parameter (owner.Child): the source is owned by something the caller borrows
        return ("2", f"path through parameter {pk[1]} → holder {hk}") if hk not in ("local", "usingvar", "temp") else ("3", f"path through parameter; holder {hk}")
    if src == "this":
        if is_ctor:
            if hk in ("ownedfield", "coll:ownedfield", "coll:field") or hk.startswith("coll:ownedfield"):
                return ("1", f"new {chain[-1].text}(this) held in {hk} {hn}")
            if hk in ("return",):
                return ("2", f"new {chain[-1].text}(this) returned owned (child may outlive this)")
            if hk in ("local", "usingvar", "temp"):
                return ("3", f"new {chain[-1].text}(this) bound to a local {hk}")
            return ("2", f"new {chain[-1].text}(this) held in {hk}")
        else:
            if hk in ("recv:ownedfield",):
                return ("1", f"{hn}.{chain[-1].text}(this): receiver is an owned field (child back-pointer)")
            if hk in ("recv:local", "recv:usingvar"):
                return ("3", f"{hn}.{chain[-1].text}(this): local receiver")
            if hk in ("recv:param", "recv:primary", "recv:field"):
                return ("2", f"{hn}.{chain[-1].text}(this): receiver {hk[5:]} (registry/observer shape)")
            return ("2", f"{chain[-1].text}(this): receiver {hk}")
    if src in ("field", "ownedfield"):
        # the source is a field of the caller's object
        if is_ctor:
            if hk in ("ownedfield",) or hk.startswith("coll:ownedfield"):
                return ("1", f"sibling: field {pk[1]} of {c2.name} into holder in {hk}")
            if hk in ("local", "usingvar", "temp"):
                return ("3", f"field source {pk[1]}; holder is a {hk}")
            if hk == "return":
                return ("2", f"field source {pk[1]}; holder returned owned")
            return ("2", f"field source {pk[1]}; holder {hk}")
        else:
            if hk in ("recv:ownedfield",):
                return ("1", f"sibling: field {pk[1]} registered on owned field {hn}")
            if hk in ("recv:local", "recv:usingvar"):
                return ("3", f"field source into local receiver {hn}")
            return ("2", f"field source {pk[1]} into receiver {hk[5:]}")
    if src in ("local", "usingvar", "loopvar", "outvar", "ownedparam"):
        if is_ctor:
            if hk in ("local", "usingvar", "temp"):
                return ("3", f"local source {pk[1]}; holder is a {hk}" + (f" {hn}" if hn else ""))
            if hk in ("ownedfield", "field") or hk.startswith("coll:ownedfield") or hk.startswith("coll:field"):
                return ("2", f"local source {pk[1]}; holder stored in {hk}")
            if hk == "return":
                return ("2", f"local source {pk[1]}; holder returned (escapes the scope)")
            if hk.startswith("coll:local") or hk.startswith("coll:usingvar"):
                return ("3", f"local source {pk[1]}; holder in a local collection")
            if hk.startswith("arg:"):
                return ("4", f"local source {pk[1]}; holder passed to {hk[4:]} (unknown retention)")
            return ("3", f"local source {pk[1]}; holder {hk}")
        else:
            if hk in ("recv:local", "recv:usingvar"):
                return ("3", f"local source {pk[1]} into local receiver {hn}")
            if hk in ("recv:param", "recv:primary"):
                return ("2", f"local source {pk[1]} into parameter receiver {hn} (caller-owned registry)")
            if hk in ("recv:field", "recv:ownedfield"):
                return ("2", f"local source {pk[1]} into field receiver {hn}")
            return ("2", f"local source {pk[1]} into receiver {hk}")
    if src in ("staticfield",):
        return ("N", "static source (always-live; no trust)")
    if src in ("fresh",):
        return ("3", "fresh temporary argument (K8 boundary; dangles unless holder dies in the expression)")
    if src in ("call", "staticcall", "unknown"):
        if pk[2] == "path":
            return ("4", f"source is a borrow returned through {pk[1]} (provenance unknown)")
        return ("4", f"source provenance unknown ({src} {pk[1]})")
    if src == "scopedparam":
        return ("4", f"forwarded scoped parameter {pk[1]}")
    if src in ("null", "value", "lambda", "move"):
        return ("V", f"non-borrow argument ({src})")
    return ("U", f"{src}")

# ----------------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--head", required=True)
    ap.add_argument("--docs", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", default=None, help="only this corpus")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    mp = os.path.join(a.docs, "tests/conformance/manifest.tsv")
    if os.path.exists(mp):
        for line in open(mp):
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 5 and parts[0] != "case-id":
                DOC_MANIFEST[parts[0]] = parts[3] + "/" + parts[4]
    units = collect_units(a.head, a.docs)
    if a.only:
        units = [u for u in units if u[0] == a.only]
    stats = Counter()
    hits = []
    per_unit = Counter(); unit_counts = Counter()
    for corpus, path, text in units:
        unit_counts[corpus] += 1
        before = len(hits)
        scan_unit(corpus, path, text, hits, stats)
        per_unit[corpus] += (len(hits) > before)
    with open(os.path.join(a.out, "hits.tsv"), "w") as f:
        f.write("\t".join(Hit.FIELDS) + "\n")
        for h in hits:
            f.write(h.row() + "\n")
    with open(os.path.join(a.out, "summary.txt"), "w") as f:
        f.write(f"units: {dict(unit_counts)}\nunits with hits: {dict(per_unit)}\nstats: {dict(stats)}\n")
        by = Counter((h.corpus, h.shape) for h in hits)
        f.write("\nhits by corpus/shape:\n")
        for k, v in sorted(by.items()): f.write(f"  {k[0]:6} {k[1]:20} {v}\n")
        bk = Counter((h.shape, h.kind) for h in hits)
        f.write("\nhits by shape/kind:\n")
        for k, v in sorted(bk.items()): f.write(f"  {k[0]:20} {k[1]:3} {v}\n")
        ke = Counter((h.corpus, h.expect) for h in hits)
        f.write("\nhits by corpus/expect:\n")
        for k, v in sorted(ke.items()): f.write(f"  {k[0]:6} {k[1]:24} {v}\n")
        ACC = ("accept", "run", "warn", "quiet", "fragment/compiles", "program/compiles", "program/runs", "?")
        f.write("\naccepted-tier (rules/docs/lit) store hits by kind:\n")
        acc = [h for h in hits if h.corpus != "gtest" and h.expect in ACC and h.shape in ("field-store", "field-init", "primary-capture", "prop-store", "coll-store", "indexer-set")]
        f.write(f"  total {len(acc)}: " + ", ".join(f"{k}={v}" for k, v in sorted(Counter(h.kind for h in acc).items())) + "\n")
        f.write("  name hints among U: " + str(dict(Counter(h.name_hint for h in acc if h.kind == 'U'))) + "\n")
        g = [h for h in hits if h.corpus == "gtest" and h.shape in ("field-store", "field-init", "primary-capture", "prop-store", "coll-store", "indexer-set")]
        f.write(f"gtest store hits by kind: total {len(g)}: " + ", ".join(f"{k}={v}" for k, v in sorted(Counter(h.kind for h in g).items())) + "\n")
        gd = {}
        for h in g:
            gd.setdefault((h.unit, getattr(h, "class"), h.member, h.target, h.source), h)
        f.write(f"gtest distinct sites (unit,class.member,target,source): {len(gd)}: " + ", ".join(f"{k}={v}" for k, v in sorted(Counter(h.kind for h in gd.values()).items())) + "\n")
        allst = [h for h in hits if h.shape in ("field-store", "field-init", "primary-capture", "prop-store", "coll-store", "indexer-set")]
        f.write(f"all store hits (every corpus, every tier): {len(allst)}: " + ", ".join(f"{k}={v}" for k, v in sorted(Counter(h.kind for h in allst).items())) + "\n")
        kk = Counter(h.kind for h in hits)
        f.write(f"\nkinds: {dict(kk)}\n")
        vir = Counter(h.virality for h in hits if h.virality != "")
        f.write(f"virality (signatures needing `kept` per hit): {dict(sorted(vir.items()))}\n")
    print(open(os.path.join(a.out, "summary.txt")).read())

if __name__ == "__main__":
    main()
