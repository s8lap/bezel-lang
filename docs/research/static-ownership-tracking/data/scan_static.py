#!/usr/bin/env python3
"""scan_static.py — measure the static-ownership-tracking candidates on the Bezel corpora.

Built on the prior lane's scanner (bzlex.py and scan_trust.py beside this file are unmodified COPIES of
docs/research/trust-admissions-by-kind/data/). For every unit it computes:

  classes.tsv   every class, with its borrow-bearing classification under candidate A (a derived type property:
                a plain class- or interface-typed field, a borrowing collection of class elements, a `scoped`
                callback field, an owned field of a borrow-bearing class, a borrow-bearing base), and why;
  stores.tsv    the prior scanner's K4.3 store sites, with the storing member's kind (constructor / method /
                setter / initializer) — what decides whether candidate A needs an announcement there;
  holders.tsv   every construction site `new C(args)` of a borrow-bearing class: where the new instance is held
                (local, using var, owned field, returned, owned argument, collection, temporary ...), where each
                carried argument comes from, the kills of a carried source while the holder is live, and the
                verdict each candidate gives the site;
  kills.tsv     every explicit `x.Dispose()` / `_ := x` on a local while a local alias of it is used later
                (candidate D's population);
  summary.txt   the counts the report quotes.

Usage:  python3 -I scan_static.py --head <repo-root> --docs <exported-root> --out <dir>

Syntactic and per-unit, like the scanner it extends: see README §3 for the precision check.
"""
import argparse, os, re, sys
from collections import Counter, defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bzlex as L
import scan_trust as S

ACC = ("accept", "run", "warn", "quiet", "fragment/compiles", "program/compiles", "program/runs", "?")
TYPE_PARAM = re.compile(r"^(T|U|V|K|E|R|S|[A-Z]|T[A-Z][A-Za-z0-9]*)$")
WRAPPERS = {"Shared", "Weak", "Locked", "RwLocked", "OwningTask", "Task"}

def strip_null(t):
    return (t or "").strip().rstrip("?")

def field_borrow_kind(m, kinds, bb):
    """How a non-static field / auto-property contributes to its class holding borrows.
    Returns (kind, detail): direct | collection | scoped-callback | owned-of-bb | generic | None."""
    typ = strip_null(m.typ)
    if not typ:
        return None
    owned = "owned" in m.mods
    h = L.type_head(typ)
    if h == "->":
        return ("scoped-callback", typ) if "scoped" in m.mods else None
    if h == "[]":
        mm = re.match(r"\[[^\]]*\](.*)", typ)
        el = strip_null(mm.group(1)) if mm else ""
        if "owned]" in typ.replace(" ", "") or "owned" in typ.split("]")[0]:
            return ("owned-of-bb", typ) if L.type_head(el) in bb else None
        return ("direct", typ) if S.is_class_typed(el, kinds) is True and not TYPE_PARAM.match(L.type_head(el)) else None
    if h in ("Task", "OwningTask"):
        return None                                     # the frame-held clause: not candidate A's subject
    if h in L.VALUE_TYPES or kinds.get(h) in ("record", "enum", "choice"):
        return None
    if TYPE_PARAM.match(h) and h not in kinds:
        return ("generic", typ)
    if h in L.BORROWING_COLLECTIONS:
        if not owned:
            return ("direct", typ)                      # a borrow of a collection object owned elsewhere
        el = strip_null(S.elem_type_of(typ) or ""); ky = strip_null(S.key_type_of(typ) or "")
        for e in (el, ky):
            if not e: continue
            eh = L.type_head(e)
            if TYPE_PARAM.match(eh) and eh not in kinds:
                return ("generic", typ)
            if S.is_class_typed(e, kinds) is True:
                return ("collection", typ)
        return None
    if h in L.OWNING_COLLECTIONS or h in WRAPPERS:
        if not owned and h not in L.OWNING_COLLECTIONS:
            return ("direct", typ)
        el = strip_null(S.elem_type_of(typ) or ""); ky = strip_null(S.key_type_of(typ) or "")
        if L.type_head(el) in bb:
            return ("owned-of-bb", typ)
        if h == "OwningDictionary" and ky and S.is_class_typed(ky, kinds) is True and not TYPE_PARAM.match(L.type_head(ky)):
            return ("collection", typ)                  # an owning map's keys are borrows (§28.11)
        return None
    if owned:
        return ("owned-of-bb", typ) if h in bb else None
    if S.is_class_typed(typ, kinds) is True:
        return ("direct", typ)
    return None

def storage_members(c):
    for m in c.members:
        if m.static: continue
        if m.kind == "field" or (m.kind == "prop" and not m.body):
            yield m

def classify_classes(classes, kinds):
    """Fixed point of the borrow-bearing property over the unit's classes. bb: name -> list of (member, kind, type)."""
    byname = {c.name: c for c in classes}
    bb = {}
    # pass 1: the reasons that do not depend on other classes
    direct = {}; pending = {}
    for c in classes:
        if c.kind != "class": continue
        why = []; later = []
        for m in storage_members(c):
            k = field_borrow_kind(m, kinds, {})
            if k and k[0] not in ("generic",):
                why.append((m.name, k[0], k[1]))
            elif k is None and "owned" in m.mods or (m.typ and L.type_head(strip_null(m.typ)) in L.OWNING_COLLECTIONS) or (m.typ and L.type_head(strip_null(m.typ)) in WRAPPERS) or (m.typ and strip_null(m.typ).startswith("[")):
                later.append(m)                         # may become borrow-bearing through another class
        names = set()
        for mm in c.members:
            if mm.body: names |= {t.text for t in mm.body if t.kind == "id"}
        for p in c.primary:                             # §22.10: a body reference captures a primary-constructor parameter into a field
            if "owned" in p.mods: continue
            pt = strip_null(p.typ)
            if p.name in names and S.is_class_typed(pt, kinds) is True and not TYPE_PARAM.match(L.type_head(pt)) and L.type_head(pt) != "->":
                why.append((p.name, "direct", p.typ + " (primary capture)"))
        if why: bb[c.name] = why
        pending[c.name] = later
    # pass 2: owned fields of borrow-bearing type, and borrow-bearing bases, to a fixed point
    changed = True
    while changed:
        changed = False
        for c in classes:
            if c.kind != "class": continue
            cur = bb.get(c.name, [])
            have = {(w[0], w[1]) for w in cur}
            add = []
            for m in pending.get(c.name, ()):
                k = field_borrow_kind(m, kinds, bb)
                if k and k[0] == "owned-of-bb" and (m.name, k[0]) not in have:
                    add.append((m.name, k[0], k[1]))
            for b in c.bases:
                if b in bb and ("<base>", "base") not in have:
                    add.append(("<base>", "base", b)); have.add(("<base>", "base"))
            if add:
                bb[c.name] = cur + add; changed = True
    return bb, byname

def block_paths(body):
    """For each token index in a member body, the path of enclosing block ids (a coarse scope tree), by the same
    opener/closer heuristic as bzlex._body_until_end."""
    path = []; out = []; counter = 0
    n = len(body)
    for k, t in enumerate(body):
        tx = t.text
        opened = False
        if tx in ("if", "while", "for", "case", "try") and not (k > 0 and body[k-1].text in ("(", "else", ":=", "=>", "return", ",", "and", "or", "not", "then", "?", "??", "end")):
            opened = True
        elif tx == "do":
            prev = body[k-1].text if k > 0 else ""
            if prev in ("then", "else", "=>", ",", ";", "end", "(", "do") or k == 0:
                opened = True
        if tx in ("else", "elif", "catch", "finally") and path:
            counter += 1; path = path[:-1] + [counter]      # a sibling arm: a new scope at the same depth
        if opened:
            counter += 1; path = path + [counter]
        out.append(tuple(path))
        if tx == "end" and path and not (k + 1 < n and False):
            path = path[:-1]
    return out

def stmt_start(body, i):
    """Walk back from token i to the start of its statement. Returns (start_index, enclosing_open_paren_index|None)."""
    d = 0; j = i - 1
    while j >= 0:
        tx = body[j].text
        if tx in (")", "]", "}"): d += 1
        elif tx in ("(", "[", "{"):
            if d == 0:
                return j + 1, j                         # inside a parenthesised group: an argument or an index
            d -= 1
        elif d == 0 and tx in (";", "then", "else", "do", "elif"):
            return j + 1, None
        elif d == 0 and tx == "=>" :
            return j + 1, None
        j -= 1
    return 0, None

def match_close(body, i):
    d = 0
    for k in range(i, len(body)):
        if body[k].text in ("(", "[", "{"): d += 1
        elif body[k].text in (")", "]", "}"):
            d -= 1
            if d == 0: return k
    return len(body) - 1

def ctor_params(c, nargs):
    for m in c.members:
        if m.kind == "ctor" and not m.static and len(m.params) == nargs:
            return m.params, m
    if c.primary and len(c.primary) == nargs:
        return c.primary, None
    for m in c.members:
        if m.kind == "ctor" and not m.static and len(m.params) >= nargs:
            return m.params, m
    if c.primary:
        return c.primary, None
    return None, None

class Site:
    FIELDS = ["corpus", "unit", "expect", "line", "cls", "in_member", "holder", "holder_name", "holder_type", "upcast", "args",
              "carried_A0", "carried_A1", "kills", "A0", "A1", "B", "notes"]
    def __init__(self, **kw):
        for f in self.FIELDS: setattr(self, f, kw.get(f, ""))
    def row(self):
        return "\t".join(str(getattr(self, f)).replace("\t", " ").replace("\n", " ") for f in self.FIELDS)

KILL_RANK = {"": 0}

def find_kills(body, start, holder, sources, paths):
    """Kills of a carried local source after token `start` while the holder is still mentioned later (textual liveness).
    Returns list of (kind, source, line, live) — live = the holder is named after the kill."""
    out = []
    n = len(body)
    last_use = -1
    if holder:
        for k in range(start, n):
            if body[k].kind == "id" and body[k].text == holder:
                last_use = k
    for k in range(start, n):
        t = body[k]
        if t.kind != "id" or t.text not in sources: continue
        nxt = body[k+1].text if k + 1 < n else ""
        prv = body[k-1].text if k > 0 else ""
        kind = None
        if nxt in (".", "?.", "!") and k + 2 < n:
            j = k + 1
            while j < n and body[j].text in ("!",): j += 1
            if j + 1 < n and body[j].text in (".", "?.") and body[j+1].text in ("Dispose", "DisposeAsync"):
                kind = "dispose"
        if prv == "move" and nxt not in (".",):
            kind = "move"
        if prv == ":=" and k > 1 and body[k-2].text == "_" and nxt == ";":
            kind = "dispose"
        if nxt == ":=" and prv in (";", "then", "else", "do", "end") :
            kind = "overwrite"
        if kind:
            out.append((kind, t.text, t.line, last_use > k))
    return out

def fix_owned_returns(classes):
    """bzlex records `method M(): owned T <statements>` with typ == 'owned' and leaves `T` at the head of the body;
    move the type back (the copy of bzlex.py is not edited)."""
    for c in classes:
        for m in c.members:
            if m.kind == "method" and (m.typ or "") == "owned" and m.body and m.body[0].kind == "id":
                typ, k = L._skip_type(m.body, 0)
                m.typ = "owned " + typ; m.body = m.body[k:]

def bare(t):
    t = (t or "").strip()
    if t.startswith("owned"): t = t[5:].strip()
    return t

def scan_unit(corpus, path, text, stats, cls_rows, site_rows, kill_rows, store_hits):
    expect = S.unit_expect(corpus, path, text)
    try:
        classes, toks = L.parse_unit(text)
    except Exception:
        stats["parse_errors"] += 1; return
    fix_owned_returns(classes)
    kinds = S.unit_kinds(classes)
    bb, byname = classify_classes(classes, kinds)
    # the prior scanner's store sites for this unit
    hits = []
    S.scan_unit(corpus, path, text, hits, Counter())
    stored = defaultdict(set)                           # class -> ctor parameter names stored into a borrow field/collection
    for h in hits:
        cname = getattr(h, "class")
        c = byname.get(cname)
        mk = "?"
        if h.member in ("constructor",): mk = "ctor"
        elif h.member in ("<init>", "<decl>"): mk = "init"
        elif c is not None:
            mem = next((m for m in c.members if m.name == h.member), None)
            mk = "setter" if (mem is not None and mem.kind == "prop") else "method"
            if h.shape == "primary-capture": mk = "primary"
        h.member_kind = mk
        STORE = ("field-store", "field-init", "primary-capture", "prop-store", "coll-store", "indexer-set")
        h.A0_store = h.A1_store = h.A_call = ""
        if h.shape in STORE and "local-collection-silent" in (h.notes or ""):
            # a parameter stored into a purely local collection: bounded by the collection's own binding, silent in today's report
            # (v0.350). Under A a returned collection carries the call's plain arguments by its declared type (A0) or a marker (A1).
            h.pop = "L"
            h.A0_store = "ok:local-collection"; h.A1_store = "edit:scoped" if h.kind in ("2", "3") else "ok:local-collection"
            h.A_call = "bounded" if h.kind in ("2", "3") else ("no-call-site" if h.kind == "U" else "")
        elif h.shape in STORE and h.kind in ("V", "N"):
            h.pop = h.kind
        elif h.shape in STORE:
            h.pop = "T"
        else:
            h.pop = ""
        if h.pop == "T":
            src_param = h.source_kind.startswith("param") or h.source_kind.startswith("primary")
            foreign = "receiver=param" in (h.notes or "") or (h.shape == "prop-store" and "receiver=" in (h.notes or "") and "receiver=this" not in (h.notes or "") and "receiver=local" not in (h.notes or ""))
            if foreign:
                h.A0_store = h.A1_store = "reject:store-into-an-object-the-member-does-not-hold"
            elif not src_param:
                h.A0_store = h.A1_store = "ok:self-or-sibling"
            elif mk in ("ctor", "init", "primary"):
                h.A0_store = "ok:announced-by-type"; h.A1_store = "edit:scoped"
            elif mk == "setter":
                h.A0_store = h.A1_store = "ok:property-store"
            else:
                h.A0_store = h.A1_store = "edit:scoped"
            h.A_call = {"3": "bounded", "1": "bounded(self)", "2": "escaping-holder(hand-check)", "4": "unknown-body", "U": "no-call-site"}.get(h.kind, "")
        if h.shape in ("field-store", "field-init", "primary-capture", "coll-store", "prop-store", "indexer-set") and mk in ("ctor", "init", "primary"):
            stored[cname].add(h.source)
        store_hits.append(h)
    for c in classes:
        if c.kind != "class": continue
        stats["classes"] += 1
        acc = corpus != "gtest" and expect in ACC
        why = bb.get(c.name)
        gen = any((field_borrow_kind(m, kinds, bb) or ("",))[0] == "generic" for m in storage_members(c))
        if why:
            stats["bb_classes"] += 1
            kindset = sorted({w[1] for w in why})
            if kindset == ["scoped-callback"]: stats["bb_only_scoped_callback"] += 1
            cls_rows.append((corpus, path, expect, c.name, "bb", ",".join(kindset), "; ".join(f"{w[0]}:{w[2]}" for w in why)[:300],
                             ",".join(c.bases), int(any(m.kind == "dtor" for m in c.members)), int(gen)))
        elif gen:
            stats["generic_conditional_classes"] += 1
            cls_rows.append((corpus, path, expect, c.name, "generic", "", "", ",".join(c.bases), int(any(m.kind == "dtor" for m in c.members)), 1))
    real_bb = {n for n, why in bb.items() if any(w[1] != "scoped-callback" for w in why)}
    # ---- construction sites and kills, per member body
    for c in classes:
        if c.kind not in ("class",): continue
        for m in c.members:
            body = m.body if m.body else m.init
            if not body: continue
            sc = S.Scope(c, m if m.body else None, classes)
            if m.body: sc.declare_locals(m.body)
            paths = block_paths(body)
            decl_at = {}
            for k, t in enumerate(body):
                if t.text in ("var", "const") and k + 1 < len(body) and body[k+1].kind == "id" and body[k+1].text not in decl_at:
                    decl_at[body[k+1].text] = k
            n = len(body)
            for i, t in enumerate(body):
                if t.text != "new" or i + 1 >= n or body[i+1].kind != "id": continue
                cname = body[i+1].text
                j = i + 2
                targs = ""
                if j < n and body[j].text == "<":
                    d = 0; j0 = j
                    while j < n:
                        if body[j].text == "<": d += 1
                        elif body[j].text == ">":
                            d -= 1
                            if d == 0: j += 1; break
                        j += 1
                    targs = "".join(x.text for x in body[j0:j])
                if j >= n or body[j].text != "(": continue
                close = match_close(body, j)
                args = S.split_args(body[j+1:close])
                # is this a borrow-bearing construction? in-unit class, or a generic owner instantiated at one
                inst_bb = None
                if cname in real_bb:
                    inst_bb = cname
                elif targs and any(L.type_head(strip_null(a)) in real_bb for a in L.type_args(cname + targs)) and (cname in L.OWNING_COLLECTIONS or cname in WRAPPERS or cname in L.BORROWING_COLLECTIONS or cname in byname):
                    inst_bb = cname + targs
                if inst_bb is None: continue
                stats["bb_construction_sites"] += 1
                # ---- holder context
                s0, paren = stmt_start(body, i)
                head = [x.text for x in body[s0:i]]
                holder = "other"; hname = ""; htype = ""; notes = []
                after = body[close+1].text if close + 1 < n else ";"
                if paren is not None:
                    # an argument of an enclosing call / constructor / indexer
                    q = paren - 1
                    callee = body[q].text if q >= 0 else ""
                    if q >= 0 and body[q].text == ">":
                        d = 0
                        while q >= 0:
                            if body[q].text == ">": d += 1
                            elif body[q].text == "<":
                                d -= 1
                                if d == 0: q -= 1; break
                            q -= 1
                        callee = body[q].text if q >= 0 else ""
                    pre = body[q-1].text if q >= 1 else ""
                    # argument index
                    ai = len(S.split_args(body[paren+1:i]))
                    if body[paren+1:i] and body[i-1].text not in (",", "(", "scoped", "move"): ai = max(0, ai - 1)
                    if pre == "new":
                        outer = byname.get(callee)
                        ps = ctor_params(outer, len(S.split_args(body[paren+1:match_close(body, paren)])))[0] if outer else None
                        pm = ps[ai] if ps and ai < len(ps) else None
                        if callee in L.OWNING_COLLECTIONS or callee in ("Shared", "Locked", "RwLocked"):
                            holder = "arg-owned"; notes.append(f"into new {callee}")
                        elif pm is not None and "owned" in pm.mods:
                            holder = "arg-owned"; notes.append(f"into new {callee}")
                        elif pm is not None:
                            holder = "arg-plain"; notes.append(f"plain arg of new {callee}")
                        else:
                            holder = "arg-unknown"; notes.append(f"arg of new {callee}")
                    elif callee in L.STORE_METHODS and pre == ".":
                        holder = "coll-add"
                        recv = body[q-2].text if q >= 2 else ""
                        rt = S._expr_type(sc, [body[q-2]], byname, kinds) if q >= 2 else None
                        notes.append(f"{recv}.{callee} recv-type={rt}")
                        hname = recv; htype = rt or ""
                    elif callee in ("Spawn",):
                        holder = "sent"
                    else:
                        # a method in this unit?
                        pm = None
                        for cc in classes:
                            for mm in cc.members:
                                if mm.kind == "method" and mm.name == callee and ai < len(mm.params):
                                    pm = mm.params[ai]
                        if pm is not None and "owned" in pm.mods: holder = "arg-owned"; notes.append(f"owned arg of {callee}")
                        elif pm is not None: holder = "arg-plain"; notes.append(f"plain arg of {callee}")
                        else: holder = "arg-unknown"; notes.append(f"arg of {callee}")
                elif after in (".", "?."):
                    holder = "temp-receiver"
                elif head[:2] == ["using", "var"] or head[:3] == ["await", "using", "var"]:
                    holder = "usingvar"; hname = head[2] if head[0] == "using" else head[3]
                    if ":" in head: htype = "".join(head[head.index(":")+1:head.index(":=")]) if ":=" in head else ""
                elif head[:1] in (["var"], ["const"]):
                    holder = "local"; hname = head[1] if len(head) > 1 else ""
                    if ":" in head and ":=" in head: htype = "".join(head[head.index(":")+1:head.index(":=")])
                elif head[:1] == ["return"] and len(head) == 1:
                    holder = "returned"; htype = bare(m.typ)
                elif head[:2] == ["yield", "return"]:
                    holder = "yielded"
                elif head[:1] == ["throw"]:
                    holder = "thrown"
                elif head[:2] == ["_", ":="]:
                    holder = "discard"
                elif not head and s0 == 0 and m.kind in ("method", "prop") and m.body and len(body) <= close + 2:
                    holder = "returned"; htype = bare(m.typ)
                elif not head and not m.body:
                    holder = "field-init-static" if m.static else "field-init"; hname = m.name; htype = m.typ or ""
                    if "owned" not in m.mods and not m.static: notes.append("non-owned field")
                elif head and head[-1] == ":=":
                    lhs = [x for x in body[s0:i-1]]
                    tgt = lhs[-1].text if lhs else ""
                    if len(lhs) == 1 and (tgt in sc.locals or tgt in sc.params):
                        holder = "reassign-local"; hname = tgt
                    elif len(lhs) == 1 or (len(lhs) == 3 and lhs[0].text == "this"):
                        f = c.field(tgt)
                        if f is not None:
                            holder = "field-static" if f.static else ("field-owned" if "owned" in f.mods else "field-plain"); hname = tgt; htype = f.typ or ""
                    elif len(lhs) >= 3 and lhs[-2].text == ".":
                        holder = "prop-of-other"; hname = "".join(x.text for x in lhs)
                elif head and head[-1] in ("??", "else", "then"):
                    holder = "expr-arm"
                # ---- upcast in an owning declaration position
                upcast = ""
                ht = L.type_head(strip_null(htype))
                if holder in ("field-owned", "field-init", "returned", "field-static", "field-init-static") and ht and ht != L.type_head(inst_bb) and ht not in real_bb and ht not in ("var",):
                    if not (ht in L.OWNING_COLLECTIONS or ht in WRAPPERS):
                        upcast = ht + ("(interface)" if kinds.get(ht) == "interface" else "")
                # ---- arguments
                cdecl = byname.get(cname)
                ps, ctor = ctor_params(cdecl, len(args)) if cdecl else (None, None)
                descs = []; carried0 = []; carried1 = []
                for ai, a in enumerate(args):
                    a2 = [x for x in a if x.text not in ("scoped",)]
                    marked = len(a2) != len(a)
                    pv = sc.provenance(a2)
                    pm = ps[ai] if ps and ai < len(ps) else None
                    ptyp = strip_null(pm.typ) if pm else ""
                    pcls = S.is_class_typed(ptyp, kinds) is True and L.type_head(ptyp) != "->" if pm else None
                    owned_p = bool(pm and "owned" in pm.mods)
                    nested_bb = pv[0] == "fresh" and (pv[1] in real_bb)
                    descs.append(f"{pv[0]}{'-path' if pv[2] else ''}:{pv[1] or ''}" + ("[owned]" if owned_p else "") + ("[scoped]" if marked else ""))
                    if pm is None:
                        # unknown constructor (stdlib wrapper): an owned BB argument is carried through its own site
                        continue
                    if owned_p:
                        continue                         # an ownership transfer; a nested borrow-bearing value brings its own sources (its own site row)
                    if not pcls and not (TYPE_PARAM.match(L.type_head(ptyp) or "") and pv[0] not in ("value", "null")):
                        continue
                    if TYPE_PARAM.match(L.type_head(ptyp) or "") and pv[0] in ("value", "null", "unknown", "call", "staticcall", "staticfield"):
                        continue
                    carried0.append((pv, pm.name))
                    if pm.name in stored.get(cname, ()) or marked or "scoped" in pm.mods:
                        carried1.append((pv, pm.name))
                # ---- verdicts
                def verdict(carried, implicit):
                    """implicit=True: candidate A0 (a construction carries every plain reference argument; an owned return of
                    borrow-bearing type carries the call's plain arguments). False: A1 (only `scoped` parameters carry)."""
                    reasons = []; needs = []
                    for pv, pname in carried:
                        k = pv[0]
                        if k in ("null", "value", "staticfield", "staticcall", "lambda", "unknown", "call", "move", "scopedfield"):
                            continue
                        if k == "fresh":
                            if holder not in ("temp-receiver", "arg-plain", "discard"):
                                reasons.append("temp-source")
                            continue
                        scope_bound = k in ("local", "usingvar", "loopvar", "ownedparam", "outvar")
                        caller_scoped = k in ("param", "primary", "scopedparam", "funcparam", "refparam")
                        selfish = k in ("this", "field", "ownedfield")
                        if holder in ("local", "usingvar", "temp-receiver", "arg-plain", "discard", "expr-arm"):
                            continue
                        if holder == "reassign-local":
                            if scope_bound and hname in decl_at and pv[1] in decl_at:
                                ph = paths[decl_at[hname]]; psrc = paths[decl_at[pv[1]]]
                                if len(psrc) > len(ph) and psrc[:len(ph)] == ph:
                                    reasons.append("holder-outlives-source(scope)")
                            continue
                        if holder in ("field-owned", "field-init", "field-plain", "coll-add", "prop-of-other"):
                            if holder == "coll-add" and hname in sc.locals:
                                continue                # a purely local collection: bounded by its own binding
                            if scope_bound:
                                reasons.append("local-source-into-field")
                            elif caller_scoped:
                                if m.kind == "ctor" and implicit and holder != "prop-of-other":
                                    pass                 # the enclosing construction carries its own plain arguments
                                elif k == "scopedparam":
                                    pass
                                else:
                                    needs.append("scoped@" + c.name + "." + m.name)
                            continue
                        if holder in ("returned", "arg-owned", "yielded"):
                            if scope_bound:
                                if holder == "arg-owned":
                                    needs.append("flow:owned-arg")   # the receiving holder's own site decides
                                else:
                                    reasons.append("returned-over-local")
                            elif caller_scoped and not implicit and k != "scopedparam" and holder == "returned":
                                needs.append("scoped@" + c.name + "." + m.name)
                            continue
                        if holder in ("field-static", "field-init-static"):
                            if not selfish or True:
                                reasons.append("non-static-source-into-static")
                            continue
                        if holder == "thrown":
                            if scope_bound or caller_scoped: reasons.append("thrown-over-scope")
                            continue
                        if holder == "sent":
                            reasons.append("not-sendable(today)")
                    return reasons, needs
                r0, n0 = verdict(carried0, True)
                r1, n1 = verdict(carried1, False)
                # constructor parameters that would need `scoped` under A1
                if ps:
                    for pv, pname in carried1:
                        pmm = next((p for p in ps if p.name == pname), None)
                        if pmm is not None and "scoped" not in pmm.mods:
                            n1.append("scoped@" + cname + ".constructor(" + pname + ")")
                if upcast:
                    if any(pv[0] not in ("null", "value", "staticfield", "staticcall") for pv, _ in carried0): r0.append("owned-upcast:" + upcast)
                    if any(pv[0] not in ("null", "value", "staticfield", "staticcall") for pv, _ in carried1): r1.append("owned-upcast:" + upcast)
                # ---- kills of carried local sources while the holder is live
                kills = []
                if holder in ("local", "usingvar", "reassign-local") and hname:
                    srcs0 = {pv[1] for pv, _ in carried0 if pv[0] in ("local", "usingvar", "ownedparam", "loopvar") and pv[1]}
                    srcs1 = {pv[1] for pv, _ in carried1 if pv[0] in ("local", "usingvar", "ownedparam", "loopvar") and pv[1]}
                    for kind, src, line, live in find_kills(body, close + 1, hname, srcs0 | srcs1, paths):
                        tag = f"{kind}:{src}@{line}:{'live' if live else 'dead'}"
                        kills.append(tag)
                        rule = {"move": "K5.1-through-holder", "overwrite": "K5.2-through-holder", "dispose": "0205-or-K4.2-through-holder"}[kind]
                        if live or kind != "dispose":
                            if src in srcs0: r0.append(rule + ("" if live else "(dead holder)"))
                            if src in srcs1: r1.append(rule + ("" if live else "(dead holder)"))
                # B (derived retention, 0132 as the prior lane modelled it): a direct constructor store, a local holder, a kill under it
                rb = []
                if holder in ("local", "usingvar") and cname in byname:
                    for tag in kills:
                        kind, rest = tag.split(":", 1)
                        src = rest.split("@")[0]
                        live = rest.endswith("live")
                        if any(pv[1] == src for pv, _ in carried1) and (live or kind != "dispose"):
                            rb.append({"move": "K5.1-through-holder", "overwrite": "K5.2-through-holder", "dispose": "0205-or-K4.2-through-holder"}[kind] + ("" if live else "(dead holder)"))
                def fmt(r, nd):
                    if r: return "reject:" + "+".join(sorted(set(r)))
                    if nd: return "edit:" + "+".join(sorted(set(nd)))
                    return "ok"
                site_rows.append(Site(corpus=corpus, unit=path, expect=expect, line=t.line, cls=inst_bb, in_member=f"{c.name}.{m.name}",
                                      holder=holder, holder_name=hname, holder_type=htype, upcast=upcast, args=" | ".join(descs),
                                      carried_A0=",".join(f"{pv[0]}:{pv[1]}" for pv, _ in carried0), carried_A1=",".join(f"{pv[0]}:{pv[1]}" for pv, _ in carried1),
                                      kills=",".join(kills), A0=fmt(r0, n0), A1=fmt(r1, n1), B=("reject:" + "+".join(sorted(set(rb)))) if rb else "ok",
                                      notes="; ".join(notes)))
            # ---- candidate D population: an explicit kill of a local with a local alias used afterwards
            if m.body:
                aliases = defaultdict(list)             # source -> [(alias, decl index)]
                for name, (kind, init, typ) in sc.locals.items():
                    if len(init) == 1 and init[0].kind == "id" and init[0].text in sc.locals and name in decl_at:
                        aliases[init[0].text].append((name, decl_at[name]))
                for k, t in enumerate(body):
                    if t.kind == "id" and t.text in aliases and k + 2 < n and body[k+1].text in (".", "?.") and body[k+2].text in ("Dispose", "DisposeAsync"):
                        for al, d in aliases[t.text]:
                            if d < k and any(body[q].kind == "id" and body[q].text == al for q in range(k + 3, n)):
                                kill_rows.append((corpus, path, expect, t.line, f"{c.name}.{m.name}", t.text, al))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--head", required=True); ap.add_argument("--docs", required=True); ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    mp = os.path.join(a.docs, "tests/conformance/manifest.tsv")
    if os.path.exists(mp):
        for line in open(mp):
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 5 and parts[0] != "case-id":
                S.DOC_MANIFEST[parts[0]] = parts[3] + "/" + parts[4]
    units = S.collect_units(a.head, a.docs)
    global UNITS
    UNITS = units
    stats = Counter(); cls_rows = []; site_rows = []; kill_rows = []; stores = []
    ucount = Counter()
    for corpus, path, text in units:
        ucount[corpus] += 1
        scan_unit(corpus, path, text, stats, cls_rows, site_rows, kill_rows, stores)
    rel = lambda p: os.path.relpath(p, a.head) if p.startswith(a.head) else os.path.relpath(p, a.docs)
    with open(os.path.join(a.out, "classes.tsv"), "w") as f:
        f.write("corpus\tunit\texpect\tclass\tstatus\tkinds\twhy\tbases\thas_dtor\tgeneric\n")
        for r in cls_rows: f.write("\t".join(str(x) if i != 1 else rel(x) for i, x in enumerate(r)) + "\n")
    with open(os.path.join(a.out, "holders.tsv"), "w") as f:
        f.write("\t".join(Site.FIELDS) + "\n")
        for s in site_rows:
            s.unit = rel(s.unit); f.write(s.row() + "\n")
    with open(os.path.join(a.out, "kills.tsv"), "w") as f:
        f.write("corpus\tunit\texpect\tline\tmember\tkilled\talias\n")
        for r in kill_rows: f.write("\t".join(str(x) if i != 1 else rel(x) for i, x in enumerate(r)) + "\n")
    with open(os.path.join(a.out, "stores.tsv"), "w") as f:
        f.write("\t".join(S.Hit.FIELDS) + "\tmember_kind\tpopulation\tA0_store\tA1_store\tA_call\n")
        for h in stores:
            h.unit = rel(h.unit); f.write(h.row() + "\t" + "\t".join((h.member_kind, h.pop, h.A0_store, h.A1_store, h.A_call)) + "\n")
    # ------------------------------------------------------------------ summary
    out = []
    P = out.append
    P(f"units: {dict(ucount)}  (total {sum(ucount.values())})")
    P(f"stats: {dict(stats)}")
    STORE = ("field-store", "field-init", "primary-capture", "prop-store", "coll-store", "indexer-set")
    allst = [h for h in stores if h.shape in STORE]
    P(f"\nstore sites the prior scanner finds, every corpus and tier: {len(allst)}; not a trust store: {sum(1 for h in allst if h.pop in ('V', 'N'))} (V = value instantiation, N = static source), {sum(1 for h in allst if h.pop == 'L')} (L = a store into a purely local collection, silent in the report since v0.350)")
    st = [h for h in allst if h.pop == "T"]
    P(f"K4.3 store sites (field-held + collection-held), every corpus and tier: {len(st)}")
    P("  by shape: " + str(dict(Counter(h.shape for h in st))))
    P("  by storing member kind: " + str(dict(Counter(h.member_kind for h in st))))
    P("  by source kind: " + str(dict(Counter(h.source_kind for h in st))))
    P("  by prior-lane kind: " + str(dict(sorted(Counter(h.kind for h in st).items()))))
    accst = [h for h in st if h.corpus != "gtest" and h.expect in ACC]
    gt = {}
    for h in st:
        if h.corpus == "gtest": gt.setdefault((h.unit, getattr(h, 'class'), h.member, h.target, h.source), h)
    def vt(rows, name):
        P(f"  [{name}] {len(rows)} sites")
        P("     storing member: " + str(dict(Counter(h.member_kind for h in rows))))
        P("     A0 at the store: " + str(dict(Counter(h.A0_store for h in rows).most_common())))
        P("     A1 at the store: " + str(dict(Counter(h.A1_store for h in rows).most_common())))
        P("     A at the visible call sites: " + str(dict(Counter(h.A_call for h in rows).most_common())))
        res = [h for h in rows if h.kind in ("1", "2", "3", "4")]
        P(f"     resolved (kind readable): {len(res)}; bounded as written (kinds 1, 3): {sum(1 for h in res if h.kind in '13')}; escaping holder, hand-checked (kind 2): {sum(1 for h in res if h.kind == '2')}; unknown body (kind 4): {sum(1 for h in res if h.kind == '4')}")
        P(f"     B (derived retention) reaches: kind 3 with a constructor store = {sum(1 for h in rows if h.kind == '3' and h.member_kind in ('ctor', 'init', 'primary'))}; kind 3 with a method store (0132 excludes a mutable field) = {sum(1 for h in rows if h.kind == '3' and h.member_kind not in ('ctor', 'init', 'primary'))}")
    vt(accst, "accepted tier, rules/docs/lit")
    vt(list(gt.values()), "gtest, distinct sites")
    vt([h for h in st if h.corpus != "gtest" and h.expect not in ACC], "expected-reject tier, rules/docs/lit")
    sig = defaultdict(set); calls = Counter()
    for h in st:
        if h.A0_store == "edit:scoped":
            sig[h.corpus != "gtest" and h.expect in ACC].add((h.unit, getattr(h, "class"), h.member, h.source))
            if h.corpus != "gtest" and h.expect in ACC: calls["callsites"] += (h.callsites or "").count("@")
    P(f"  A0 `scoped` markers needed (method stores of a plain parameter): {len(sig[True])} signatures in the accepted tier, {calls['callsites']} visible call sites that would take the call-site marker; {len(sig[False])} signatures elsewhere")
    sig1 = defaultdict(set)
    for h in st:
        if h.A1_store == "edit:scoped": sig1[h.corpus != "gtest" and h.expect in ACC].add((h.unit, getattr(h, "class"), h.member, h.source))
    P(f"  A1 `scoped` markers needed (every stored plain parameter): {len(sig1[True])} signatures in the accepted tier; {len(sig1[False])} elsewhere")
    other = Counter(h.shape for h in stores if h.shape not in STORE)
    P(f"other recorded clauses (not candidate A's subject): {dict(other)}")
    bbrows = [r for r in cls_rows if r[4] == "bb"]
    P(f"\nclasses: {stats['classes']}; borrow-bearing under A: {len(bbrows)} ({100.0*len(bbrows)/max(1,stats['classes']):.1f} %); of which only a `scoped` callback field: {stats['bb_only_scoped_callback']}; generic-conditional (a bare T field): {stats['generic_conditional_classes']}")
    kc = Counter()
    for r in bbrows:
        for k in r[5].split(","): kc[k] += 1
    P("  borrow-bearing classes by reason (a class may have several): " + str(dict(kc)))
    P("  by corpus: " + str(dict(Counter(r[0] for r in bbrows))))
    P("  with a base or interface list: " + str(sum(1 for r in bbrows if r[7])) + "; with a destructor: " + str(sum(1 for r in bbrows if r[8])))
    P(f"\nconstruction sites of a borrow-bearing class: {len(site_rows)}")
    P("  by holder position: " + str(dict(Counter(s.holder for s in site_rows).most_common())))
    def tier(rows, name):
        P(f"\n[{name}] sites: {len(rows)}")
        for cand in ("A0", "A1", "B"):
            c = Counter()
            for s in rows:
                v = getattr(s, cand)
                c[v.split(":")[0]] += 1
            P(f"  {cand}: {dict(c)}")
            rc = Counter()
            for s in rows:
                v = getattr(s, cand)
                if v.startswith("reject:"):
                    for r in v[7:].split("+"): rc[r] += 1
                if v.startswith("edit:"):
                    for r in v[5:].split("+"): rc["edit:" + r.split("@")[0]] += 1
            P(f"     reasons: {dict(rc.most_common())}")
        units = defaultdict(lambda: {"A0": "ok", "A1": "ok", "B": "ok"})
        for s in rows:
            for cand in ("A0", "A1", "B"):
                v = getattr(s, cand).split(":")[0]
                cur = units[s.unit][cand]
                if v == "reject" or (v == "edit" and cur == "ok"): units[s.unit][cand] = v
        P(f"  units with a site: {len(units)}; " + "; ".join(f"{cand}: " + str(dict(Counter(u[cand] for u in units.values()))) for cand in ("A0", "A1", "B")))
    acc_sites = [s for s in site_rows if s.corpus != "gtest" and s.expect in ACC]
    tier(acc_sites, "accepted tier: rules/docs/lit programs the spec expects to compile")
    tier([s for s in site_rows if s.corpus != "gtest" and s.expect not in ACC], "expected-reject tier (rules/docs/lit)")
    tier([s for s in site_rows if s.corpus == "gtest"], "gtest (units are concatenated program variants; verdict tier unknown)")
    # ---- per-program roll-up, accepted tier: construction-site verdicts + store-site verdicts
    roll = defaultdict(lambda: {"A0": "ok", "A1": "ok", "B": "ok"})
    def bump(u, cand, v):
        cur = roll[u][cand]
        if v == "reject" or (v == "edit" and cur == "ok"): roll[u][cand] = v
    touched = set()
    for s in acc_sites:
        touched.add(s.unit)
        for cand in ("A0", "A1", "B"): bump(s.unit, cand, getattr(s, cand).split(":")[0])
    for h in [h for h in allst if h.pop in ("T", "L") and h.corpus != "gtest" and h.expect in ACC]:
        touched.add(h.unit)
        for cand, v in (("A0", h.A0_store), ("A1", h.A1_store)):
            bump(h.unit, cand, v.split(":")[0])
        roll[h.unit]
    # hand-checked verdicts (hand_check.tsv beside this script): every accepted-tier program the scanner rejects or
    # marks as an escaping holder was read; the file records the verdict A gives it and the hazard class
    hc = {}
    hp = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hand_check.tsv")
    if os.path.exists(hp):
        for line in open(hp):
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 4 and parts[0] != "unit": hc[parts[0]] = parts
    need = sorted(u for u in touched if roll[u]["A0"] == "reject" or any(h.unit == u and h.kind == "2" for h in accst))
    missing = [u for u in need if u not in hc]
    for u, parts in hc.items():
        if u in roll:
            roll[u]["A0"] = parts[1]; roll[u]["A1"] = parts[2]
    P(f"\nhand-checked accepted-tier programs (scanner reject or escaping holder): {len(need)}; missing from hand_check.tsv: {missing}")
    P("  hazard class of the hand-confirmed rejections under A: " + str(dict(Counter(parts[3] for u, parts in hc.items() if parts[1] == "reject"))))
    n_acc_units = sum(1 for corpus, path, text in UNITS if corpus != "gtest" and S.unit_expect(corpus, path, text) in ACC)
    P(f"\nPER-PROGRAM ROLL-UP, accepted tier (rules/docs/lit units the spec expects to compile): {n_acc_units} programs; {len(touched)} hold a K4.3 store or construct a borrow-bearing class")
    for cand in ("A0", "A1", "B"):
        c = Counter(roll[u][cand] for u in touched)
        P(f"  {cand}: unchanged {c['ok']}; compile after an edit (a `scoped` marker) {c['edit']}; rejected, must be rewritten {c['reject']}  ({100.0*c['reject']/max(1,n_acc_units):.2f} % of all accepted-tier programs)")
    with open(os.path.join(a.out, "rollup.tsv"), "w") as f:
        f.write("unit\tA0\tA1\tB\n")
        for u in sorted(touched): f.write(f"{u}\t{roll[u]['A0']}\t{roll[u]['A1']}\t{roll[u]['B']}\n")
    up = [s for s in site_rows if s.upcast]
    P(f"\nowned upcast of a borrow-bearing instance to a type that is not borrow-bearing: {len(up)} sites ({sum(1 for s in up if s.corpus != 'gtest' and s.expect in ACC)} in the accepted tier): " + str(dict(Counter(s.upcast for s in up).most_common(8))))
    P(f"\ncandidate D population — an explicit Dispose() of a local with a local alias named afterwards: {len(kill_rows)} sites; by corpus/expect: " + str(dict(Counter((r[0], r[2]) for r in kill_rows))))
    open(os.path.join(a.out, "summary.txt"), "w").write("\n".join(out) + "\n")
    print("\n".join(out))

if __name__ == "__main__":
    main()
