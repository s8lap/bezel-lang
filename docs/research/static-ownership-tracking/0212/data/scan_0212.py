#!/usr/bin/env python3
"""scan_0212.py — proposal 0212's corpus measurement.

It RE-RUNS the static-ownership-tracking note's scanner (../../data/scan_static.py, imported unmodified, with its
bzlex.py and scan_trust.py) over the same corpora, and adds the passes the note did not have, one per rule 0212
adds or settles:

  R1  interfaces that are borrow-bearing by derivation (a class- or interface-typed `scoped` parameter in a member);
      borrow-bearing classes that list an interface or base that is not borrow-bearing
  R4  stores into static-held storage; later stores by the member kind (re-read from the note's stores.tsv)
  R5  owned erasure in a GENERIC body: every member of a generic declaration that takes an `owned` parameter typed at a
      type parameter, classified by where the parameter goes (kept at a type that mentions the parameter / erased to a
      type that does not / forwarded / thrown / only used); the instantiations of each erasing declaration at a class;
      every generic instantiation, user or standard, whose type argument is a borrow-bearing class of the unit
  R5  `throw` of a freshly built borrow-bearing instance that carries an argument
  R6  the destructor window: borrow-bearing classes with a destructor, whether the destructor body names a borrow
      field, self-sourced constructions (`this` among the arguments) and constructions held in a field (sibling order)
  R7  async: a local instance of a borrow-bearing class in an `async` body with an `await` between its declaration
      and a later use; `async` members with a parameter or a task result of borrow-bearing class type

Revision 2 (after the adversarial critique of the first draft) adds one pass per finding:
  F1  `out owned` parameters, and those declared at a borrow-bearing class
  F2  owning choices and owning inline arrays that own a borrow-bearing class; `Result<…>` named at one
  F3  `owned` parameters of borrow-bearing class type, by member kind, and whether the body hands the parameter to a
      member of another parameter's object (the critic's c04)
  F4  a `move` of a field of an object other than `this` (a replacing or bare take from outside a member)
  F6  constructions of the comparer-retaining collections with an argument, by where the argument comes from
  F7  `Clone()` / `Downgrade()` / `TryUpgrade()` calls in units that name `Shared<C>` or `Weak<C>` at a borrow-bearing C
  N10 a constructor that hands `this` to a member of one of its parameters (observer self-registration)
  N11 exception classes that are borrow-bearing; the doc-extracted code blocks holding a store whose verdict changes

Outputs (in --out): base/ (the note's scanner, re-run), generic_owned_T.tsv, generic_inst_at_bb.tsv, throws.tsv,
async.tsv, dtor.tsv, interfaces.tsv, rollup_0212.tsv, summary_0212.txt.

Usage:  python3 -I scan_0212.py --head <repo-root> --docs <exported-root> --out <dir>

Syntactic and per unit, like the scanner it extends: declared types are resolved inside one unit, an unknown type name
is read as a class, and control flow is text order. Every accepted-tier program this script newly rejects is listed by
name in summary_0212.txt for a hand check.
"""
import argparse, os, re, subprocess, sys
from collections import Counter, defaultdict
HERE = os.path.dirname(os.path.abspath(__file__))
NOTE = os.path.normpath(os.path.join(HERE, "..", "..", "data"))
sys.path.insert(0, NOTE)
import bzlex as L
import scan_trust as S
import scan_static as SS

ACC = SS.ACC
VALUE_HEADS = set(L.VALUE_TYPES)

def tparams(text):
    """(class type params by class name, method type params by method name) read from declaration headers."""
    cls = {}; meth = defaultdict(set)
    for m in re.finditer(r"\b(?:class|interface|record)\s+(\w+)\s*<([^<>()]*)>", text):
        cls[m.group(1)] = {p.strip().split()[-1] for p in m.group(2).split(",") if p.strip()}
    for m in re.finditer(r"\bmethod\s+(\w+)\s*<([^<>()]*)>\s*\(", text):
        meth[m.group(1)] |= {p.strip().split()[-1] for p in m.group(2).split(",") if p.strip()}
    return cls, meth

def mentions(typ, names):
    return any(re.search(r"(?<![\w])" + re.escape(n) + r"(?![\w])", typ or "") for n in names)

def ref_typed(typ, kinds):
    """a declared type that can hold a class instance: not a value type, not a function type."""
    t = SS.strip_null(typ or "")
    t = re.sub(r"^owned\s+", "", t)
    if not t or SS.is_fn_type(t): return False
    h = L.type_head(t)
    if h in VALUE_HEADS or kinds.get(h) in ("record", "enum", "choice"): return False
    return True

def classify_owned_T(m, c, tp, kinds):
    """Where does each `owned` parameter typed at a type parameter go? -> list of (param, verdict, detail)."""
    out = []
    body = m.body or []
    texts = [t.text for t in body]
    for p in m.params:
        if "owned" not in p.mods: continue
        pt = SS.strip_null(p.typ or "")
        if not mentions(pt, tp): continue
        verdicts = []
        n = len(texts)
        for i, tx in enumerate(texts):
            if tx != p.name or (i > 0 and texts[i-1] == "."): continue
            prev = texts[i-1] if i > 0 else ""
            prev2 = texts[i-2] if i > 1 else ""
            nxt = texts[i+1] if i + 1 < n else ""
            if nxt in (".", "?.", "!"):                      # a member access through it: a use
                verdicts.append(("use", "")); continue
            # `f := p` / `f := move p`
            j = i - 1
            if prev == "move": j -= 1
            if j >= 1 and texts[j] == ":=" and body[j-1].kind == "id":
                f = c.field(texts[j-1])
                if f is not None:
                    if mentions(f.typ or "", tp): verdicts.append(("keep-field", f"{f.name}: {f.typ}"))
                    elif ref_typed(f.typ, kinds): verdicts.append(("ERASE-field", f"{f.name}: {f.typ}"))
                    else: verdicts.append(("use", ""))
                    continue
            if prev == "throw" or (prev == "move" and prev2 == "throw"):
                verdicts.append(("ERASE-throw", "")); continue
            ret = (prev == "return") or (prev == "move" and prev2 == "return") or (len(texts) == 1) or (len(texts) == 2 and texts[0] == "move")
            if ret and (nxt in (";", "") ):
                rt = m.typ or ""
                if mentions(rt, tp): verdicts.append(("keep-return", rt))
                elif ref_typed(rt, kinds): verdicts.append(("ERASE-return", rt))
                else: verdicts.append(("use", ""))
                continue
            if prev == "move":
                verdicts.append(("forward", " ".join(texts[max(0, i-6):i+2]))); continue
            verdicts.append(("use", ""))
        if not verdicts: verdicts = [("unused", "")]
        rank = {"ERASE-field": 0, "ERASE-return": 0, "ERASE-throw": 0, "forward": 1, "keep-field": 2, "keep-return": 2, "use": 3, "unused": 4}
        verdicts.sort(key=lambda v: rank[v[0]])
        out.append((p, verdicts[0][0], verdicts[0][1]))
    return out

def instantiations(toks, name):
    """type-argument lists of every `name<...>` in the unit that is not the declaration header."""
    out = []
    n = len(toks)
    for i, t in enumerate(toks):
        if t.kind != "id" or t.text != name or i + 1 >= n or toks[i+1].text != "<": continue
        if i > 0 and toks[i-1].text in ("class", "end", "record", "interface", "extension", "method"): continue
        d = 0; j = i + 1; parts = []
        while j < n:
            if toks[j].text == "<": d += 1
            elif toks[j].text == ">":
                d -= 1
                if d == 0: break
            parts.append(toks[j].text); j += 1
            if j - i > 40: break
        if j < n and toks[j].text == ">":
            out.append((t.line, L.type_args(name + "".join(parts) + ">")))
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--head", required=True); ap.add_argument("--docs", required=True); ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    base = os.path.join(a.out, "base")
    os.makedirs(base, exist_ok=True)
    subprocess.run([sys.executable, "-I", os.path.join(NOTE, "scan_static.py"), "--head", a.head, "--docs", a.docs, "--out", base], check=True)
    mp = os.path.join(a.docs, "tests/conformance/manifest.tsv")
    if os.path.exists(mp):
        for line in open(mp):
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 5 and parts[0] != "case-id":
                S.DOC_MANIFEST[parts[0]] = parts[3] + "/" + parts[4]
    units = S.collect_units(a.head, a.docs)
    rel = lambda p: os.path.relpath(p, a.head) if p.startswith(a.head) else os.path.relpath(p, a.docs)

    gen_rows = []; inst_rows = []; throw_rows = []; async_rows = []; dtor_rows = []; iface_rows = []
    rev2_rows = []
    static_rows = []; stat = Counter(); new_reject = defaultdict(list)
    for corpus, path, text in units:
        expect = S.unit_expect(corpus, path, text)
        try:
            classes, toks = L.parse_unit(text)
        except Exception:
            stat["parse_errors"] += 1; continue
        SS.fix_owned_returns(classes)
        kinds = S.unit_kinds(classes)
        bb, byname = SS.classify_classes(classes, kinds)
        bbnames = {n for n, why in bb.items() if any(w[1] != "scoped-callback" for w in why)}
        ctp, mtp = tparams(text)
        acc = corpus != "gtest" and expect in ACC
        u = rel(path)
        # ---- R1: interfaces
        derived_if = set()
        for c in classes:
            if c.kind != "interface": continue
            stat["interfaces"] += 1
            why = []
            for m in c.members:
                for p in m.params:
                    if "scoped" in p.mods and not SS.is_fn_type(SS.strip_null(p.typ or "")) and ref_typed(p.typ, kinds):
                        why.append(f"{m.name}(scoped {p.name}: {p.typ})")
            scoped_why = list(why)
            for m in c.members:
                # a settable reference-typed property: bzlex records `readonly` in the member's modifiers; an interface
                # property declares no body, so "not readonly" is read as settable (an over-count, never an under-count)
                if m.kind == "prop" and "readonly" not in m.mods and not SS.is_fn_type(SS.strip_null(m.typ or "")) and ref_typed(m.typ, kinds) \
                        and L.type_head(SS.strip_null(m.typ or "")) not in ("string", "Type", "Object") and kinds.get(L.type_head(SS.strip_null(m.typ or ""))) in ("class", "interface"):
                    why.append(f"property {m.name}: {m.typ} (not readonly)")
            if scoped_why: stat["interfaces_bb_by_scoped_parameter"] += 1
            if len(why) > len(scoped_why): stat["interfaces_bb_by_settable_property"] += 1
            if why:
                derived_if.add(c.name); stat["interfaces_bb_derived"] += 1
                iface_rows.append((corpus, u, expect, c.name, "; ".join(why)))
        for c in classes:
            if c.kind == "class" and c.name in bbnames:
                nb = [b for b in c.bases if b not in bbnames and b not in derived_if and b not in ("Object",)]
                if nb:
                    stat["bb_class_with_non_bb_base_or_interface"] += 1
                    if any(kinds.get(b) == "interface" for b in nb): stat["bb_class_implementing_unit_interface_not_bb"] += 1
        # ---- R5: generic bodies with an owned parameter at a type parameter
        erasing = {}
        for c in classes:
            if c.kind not in ("class", "interface"): continue
            for m in c.members:
                if m.kind not in ("ctor", "method"): continue
                tp = set(ctp.get(c.name, ())) | set(mtp.get(m.name, ()) if m.kind == "method" else ())
                if not tp: continue
                for p, verdict, detail in classify_owned_T(m, c, tp, kinds):
                    own_tp = mentions(SS.strip_null(p.typ or ""), set(mtp.get(m.name, ()))) if m.kind == "method" else False
                    dispatched = own_tp and (c.kind == "interface" or bool({"virtual", "abstract", "override"} & set(m.mods)))
                    gen_rows.append((corpus, u, expect, c.name, m.name, f"owned {p.name}: {p.typ}", verdict, detail, "method-parameter" if own_tp else "type-parameter", int(dispatched)))
                    stat["generic_owned_T_params"] += 1; stat["generic_owned_T:" + verdict.split("-")[0]] += 1
                    if dispatched: stat["generic_owned_T_in_dispatched_generic_method"] += 1
                    if verdict.startswith("ERASE"):
                        erasing[(c.name, m.name, own_tp)] = verdict
        for (cn, mn, own_tp), verdict in erasing.items():
            target = mn if own_tp else cn
            for line, args in instantiations(toks, target):
                heads = [L.type_head(SS.strip_null(x)) for x in args]
                at_bb = [h for h in heads if h in bbnames]
                at_class = [h for h in heads if kinds.get(h) in ("class", "interface")]
                stat["erasing_generic_instantiations"] += 1
                if at_class: stat["erasing_generic_instantiations_at_a_class"] += 1
                if at_bb:
                    stat["erasing_generic_instantiations_at_a_borrow_bearing_class"] += 1
                    if acc: new_reject[u].append(f"R5 generic erasure: {target}<{', '.join(args)}> at line {line} ({verdict} in {cn}.{mn})")
        # ---- every generic instantiation at a borrow-bearing class of the unit
        n = len(toks)
        for i, t in enumerate(toks):
            if t.kind != "id" or i + 1 >= n or toks[i+1].text != "<" or not t.text[:1].isupper(): continue
            if i > 0 and toks[i-1].text in ("class", "end", "record", "interface", "extension"): continue
            d = 0; j = i + 1; parts = []
            while j < n and j - i < 40:
                if toks[j].text == "<": d += 1
                elif toks[j].text == ">":
                    d -= 1
                    if d == 0: break
                elif toks[j].text in (";", ":=", "then", "do", "(", ")") and d <= 1: break
                parts.append(toks[j].text); j += 1
            if not (j < n and toks[j].text == ">" and d == 0): continue
            args = L.type_args(t.text + "".join(parts) + ">")
            hit = [x for x in args if L.type_head(SS.strip_null(x)) in bbnames]
            if hit:
                inst_rows.append((corpus, u, expect, t.line, t.text, ", ".join(args)))
                stat["generic_instantiations_at_a_bb_class"] += 1
                stat["generic_inst_at_bb:" + ("user" if t.text in byname else "standard")] += 1
        # ---- R5: throw of a fresh borrow-bearing instance
        for i, t in enumerate(toks):
            if t.text == "throw" and i + 2 < n and toks[i+1].text == "new" and toks[i+2].text in bbnames:
                k = i + 3
                while k < n and toks[k].text != "(": k += 1
                d = 0; e = k; args = []
                while e < n:
                    if toks[e].text == "(": d += 1
                    elif toks[e].text == ")":
                        d -= 1
                        if d == 0: break
                    elif d >= 1 and toks[e].kind == "id" and toks[e].text not in L.KEYWORDS: args.append(toks[e].text)
                    e += 1
                throw_rows.append((corpus, u, expect, t.line, toks[i+2].text, " ".join(args)))
                stat["throw_of_new_bb_instance"] += 1
                if args:
                    stat["throw_of_new_bb_instance_with_identifier_argument"] += 1
        # ---- R6: destructors
        for c in classes:
            if c.kind != "class" or c.name not in bbnames: continue
            dt = next((m for m in c.members if m.kind == "dtor"), None)
            if dt is None: continue
            bfields = {w[0] for w in bb[c.name] if w[1] in ("direct", "collection")}
            names = {t.text for t in (dt.body or []) if t.kind == "id"}
            reads = sorted(bfields & names)
            calls = sorted({m.name for m in c.members if m.kind == "method" and not m.static} & names)
            dtor_rows.append((corpus, u, expect, c.name, ",".join(sorted(bfields)), ",".join(reads), ",".join(calls)))
            stat["bb_classes_with_destructor"] += 1
            if reads: stat["bb_dtor_names_a_borrow_field"] += 1
            if calls: stat["bb_dtor_calls_an_instance_method"] += 1
        # ---- R7: async
        for c in classes:
            if c.kind != "class": continue
            for m in c.members:
                if m.kind != "method" or "async" not in m.mods: continue
                stat["async_methods"] += 1
                for p in m.params:
                    if L.type_head(SS.strip_null(re.sub(r"^owned\s+", "", p.typ or ""))) in bbnames:
                        async_rows.append((corpus, u, expect, m.line, f"{c.name}.{m.name}", "parameter", f"{' '.join(sorted(p.mods))} {p.name}: {p.typ}".strip(), ""))
                        stat["async_param_of_bb_class:" + ("owned" if "owned" in p.mods else "plain")] += 1
                rt = m.typ or ""
                ra = L.type_args(re.sub(r"^owned\s+", "", rt)) if "<" in rt else []
                if any(L.type_head(SS.strip_null(x)) in bbnames for x in ra):
                    async_rows.append((corpus, u, expect, m.line, f"{c.name}.{m.name}", "result", rt, ""))
                    stat["async_result_of_bb_class"] += 1
                body = m.body or []; bt = [t.text for t in body]
                params = {p.name for p in m.params}
                for i, tx in enumerate(bt):
                    if tx == "new" and i + 1 < len(bt) and bt[i+1] in bbnames:
                        # the local it initializes: `var x := new C(` / `using var x := new C(`
                        if i >= 2 and bt[i-1] == ":=" and body[i-2].kind == "id":
                            name = bt[i-2]
                            aw = [k for k in range(i, len(bt)) if bt[k] == "await"]
                            if not aw: continue
                            use_after = any(bt[k] == name for k in range(aw[0] + 1, len(bt)))
                            # arguments
                            k = i + 2
                            while k < len(bt) and bt[k] != "(": k += 1
                            d = 0; e = k; args = []
                            while e < len(bt):
                                if bt[e] == "(": d += 1
                                elif bt[e] == ")":
                                    d -= 1
                                    if d == 0: break
                                elif d >= 1 and body[e].kind == "id" and bt[e] not in L.KEYWORDS: args.append(bt[e])
                                e += 1
                            src = "caller-sourced" if (set(args) & (params | {"this"})) else "frame-resident-or-none"
                            if use_after:
                                async_rows.append((corpus, u, expect, body[i].line, f"{c.name}.{m.name}", "local-across-await", f"{name} := new {bt[i+1]}({' '.join(args)})", src))
                                stat["async_bb_local_live_across_await"] += 1; stat["async_bb_local_across_await:" + src] += 1
        # ---- revision 2: one pass per critique finding
        def R2(kind, line, where, detail):
            rev2_rows.append((kind, corpus, u, expect, line, where, detail)); stat["rev2:" + kind] += 1
        bt_all = [t.text for t in toks]; nall = len(bt_all)
        for c in classes:
            if c.kind == "choice":
                for m in c.members:
                    pass
            for m in c.members:
                if m.kind not in ("ctor", "method"): continue
                body = m.body or []; bt = [t.text for t in body]
                for p in m.params:
                    ph = L.type_head(SS.strip_null(re.sub(r"^owned\s+", "", p.typ or "")))
                    if "out" in p.mods and "owned" in p.mods:
                        R2("F1 out-owned parameter", m.line, f"{c.name}.{m.name}", f"out owned {p.name}: {p.typ}")
                        if ph in bbnames: R2("F1 out-owned parameter of a borrow-bearing class", m.line, f"{c.name}.{m.name}", f"out owned {p.name}: {p.typ}")
                    if "owned" in p.mods and "out" not in p.mods and ph in bbnames:
                        kind = "constructor" if m.kind == "ctor" else ("static method" if m.static else "instance method")
                        R2("F3 owned parameter of a borrow-bearing class: " + kind, m.line, f"{c.name}.{m.name}", f"owned {p.name}: {p.typ}")
                        others = {q.name for q in m.params if q.name != p.name and "owned" not in q.mods}
                        for i in range(len(bt) - 4):
                            if bt[i] in others and bt[i+1] == "." and bt[i+3] == "(":
                                d = 0; e = i + 3; hit = False
                                while e < len(bt):
                                    if bt[e] == "(": d += 1
                                    elif bt[e] == ")":
                                        d -= 1
                                        if d == 0: break
                                    elif bt[e] == p.name and bt[e-1] in ("move", "(", ","): hit = True
                                    e += 1
                                if hit: R2("F3 owned parameter handed to a member of another parameter's object", body[i].line, f"{c.name}.{m.name}", f"{bt[i]}.{bt[i+2]}(… {p.name} …)")
                # N10: a constructor hands `this` to a member of one of its parameters
                if m.kind == "ctor":
                    pn = {q.name for q in m.params}
                    for i in range(len(bt) - 4):
                        if bt[i] in pn and bt[i+1] == "." and body[i+2].kind == "id":
                            k = i + 3
                            while k < len(bt) and bt[k] in (".",) : k += 2
                            if k < len(bt) and bt[k] == "(":
                                d = 0; e = k; hit = False
                                while e < len(bt):
                                    if bt[e] == "(": d += 1
                                    elif bt[e] == ")":
                                        d -= 1
                                        if d == 0: break
                                    elif bt[e] == "this" and bt[e-1] in ("(", ",", "scoped") and bt[e+1] in (")", ","): hit = True
                                    e += 1
                                if hit: R2("N10 a constructor hands `this` to a member of a parameter", body[i].line, f"{c.name}.constructor", " ".join(bt[i:min(e+1, i+10)]))
        # F2: owning choices / owning inline arrays / Result<…> over a borrow-bearing class
        for i, t in enumerate(toks):
            if t.text == "choice" and i + 1 < nall and toks[i+1].kind == "id":
                j = i + 2; d = 0
                while j < nall and not (bt_all[j] == "end" and j + 1 < nall and bt_all[j+1] == bt_all[i+1]):
                    if bt_all[j] == "owned" and j + 3 < nall and bt_all[j+2] == ":" and L.type_head(SS.strip_null(bt_all[j+3])) in bbnames:
                        R2("F2 owning choice with an owned payload of a borrow-bearing class", toks[j].line, bt_all[i+1], f"owned {bt_all[j+1]}: {bt_all[j+3]}")
                    j += 1
            if t.text == "[" and i + 4 < nall and bt_all[i+2] == "owned" and bt_all[i+3] == "]" and L.type_head(SS.strip_null(bt_all[i+4])) in bbnames:
                R2("F2 owning inline array of a borrow-bearing class", t.line, "", "".join(bt_all[i:i+5]))
            if t.text == "Result" and i + 1 < nall and bt_all[i+1] == "<":
                d = 0; j = i + 1; parts = []
                while j < nall and j - i < 30:
                    if bt_all[j] == "<": d += 1
                    elif bt_all[j] == ">":
                        d -= 1
                        if d == 0: break
                    parts.append(bt_all[j]); j += 1
                args = L.type_args("Result" + "".join(parts) + ">") if j < nall and bt_all[j] == ">" else []
                stat["rev2:F2 Result<…> named"] += 1
                if any(L.type_head(SS.strip_null(x)) in bbnames for x in args):
                    R2("F2 Result<…> named at a borrow-bearing class", t.line, "", "Result<" + ", ".join(args) + ">")
                elif any(kinds.get(L.type_head(SS.strip_null(x))) == "class" for x in args):
                    stat["rev2:F2 Result<…> named at a class of the unit that is not borrow-bearing"] += 1
        # F4: a `move` of a field of an object other than `this`
        for i, t in enumerate(toks):
            if t.text == "move" and i + 3 < nall and toks[i+1].kind == "id" and bt_all[i+1] != "this" and bt_all[i+2] == "." and toks[i+3].kind == "id":
                fld = bt_all[i+3]
                owners = [c2 for c2 in classes if c2.kind == "class" and c2.field(fld) is not None]
                ft = [L.type_head(SS.strip_null(c2.field(fld).typ or "")) for c2 in owners]
                R2("F4 move of a field of an object other than `this`", t.line, "", " ".join(bt_all[i:i+6]))
                if any(x in bbnames for x in ft):
                    R2("F4 … whose field type is a borrow-bearing class", t.line, "", " ".join(bt_all[i:i+6]))
        # F6: constructions of the comparer-retaining collections with an argument
        for i, t in enumerate(toks):
            if t.text == "new" and i + 2 < nall and bt_all[i+1] in ("Dictionary", "OwningDictionary", "Set", "OwningSet") and bt_all[i+2] == "<":
                d = 0; j = i + 2
                while j < nall:
                    if bt_all[j] == "<": d += 1
                    elif bt_all[j] == ">":
                        d -= 1
                        if d == 0: break
                    j += 1
                targs = L.type_args(bt_all[i+1] + "".join(bt_all[i+2:j]) + ">")
                if j + 1 < nall and bt_all[j+1] == "(" and j + 2 < nall and bt_all[j+2] != ")":
                    d = 0; e = j + 1; arg = []
                    while e < nall:
                        if bt_all[e] == "(": d += 1
                        elif bt_all[e] == ")":
                            d -= 1
                            if d == 0: break
                        arg.append(bt_all[e]); e += 1
                    arg = arg[1:]
                    if all(re.fullmatch(r"[0-9_]+", x) or x in (",",) for x in arg): continue      # a capacity
                    if len(arg) == 1 and re.fullmatch(r"[a-z_]\w*", arg[0]) and re.search(r"cap|size|count|^n$", arg[0], re.I): continue
                    src = "a fresh `new`" if "new" in arg else ("a static member (`T.M`)" if (len(arg) >= 3 and arg[-2] == "." and arg[-3][:1].isupper()) else "a local, parameter or field")
                    bbinst = any(L.type_head(SS.strip_null(x)) in bbnames or kinds.get(L.type_head(SS.strip_null(x))) in ("class", "interface") for x in targs)
                    R2("F6 comparer-retaining collection built with an argument: " + src + (" (class-typed instantiation)" if bbinst else " (value instantiation)"), t.line, "", bt_all[i+1] + "<" + ", ".join(targs) + ">(" + " ".join(arg[:8]) + ")")
        # F7: Clone / Downgrade / TryUpgrade in a unit that names Shared<C> or Weak<C> at a borrow-bearing C
        shared_bb = any(r[1] == u and r[4] in ("Shared", "Weak") for r in inst_rows)
        if shared_bb:
            for i, t in enumerate(toks):
                if t.text in ("Clone", "Downgrade", "TryUpgrade") and i > 0 and bt_all[i-1] == "." and i + 1 < nall and bt_all[i+1] == "(":
                    R2("F7 Clone/Downgrade/TryUpgrade in a unit that names Shared<C>/Weak<C> at a borrow-bearing C", t.line, "", "." + t.text + "()")
        # N11: borrow-bearing exception classes
        for c in classes:
            if c.kind == "class" and c.name in bbnames and (any(b.endswith("Exception") or b.endswith("Error") for b in c.bases)):
                R2("N11 exception class that is borrow-bearing", c.line, c.name, ", ".join(f"{w[0]}: {w[2]}" for w in bb[c.name]))

        # ---- R4: static-held stores of a parameter
        for c in classes:
            if c.kind != "class": continue
            sf = {m.name: m for m in c.members if m.static and m.kind in ("field", "prop")}
            for m in c.members:
                if m.kind != "method" or not m.params: continue
                bt = [t.text for t in (m.body or [])]
                pn = {p.name for p in m.params if "owned" not in p.mods and ref_typed(p.typ, kinds)}
                for i, tx in enumerate(bt):
                    if tx in sf and i + 3 < len(bt) and ((bt[i+1] == ":=" and bt[i+2] in pn) or (bt[i+1] == "." and bt[i+2] in ("Add", "Insert", "Push", "Enqueue") and bt[i+3] == "(" and i + 4 < len(bt) and bt[i+4] in pn)):
                        static_rows.append((corpus, u, expect, m.body[i].line, f"{c.name}.{m.name}", tx))
                        stat["static_held_store_of_a_parameter"] += 1
                        if acc: new_reject[u].append(f"R4 static store: {c.name}.{m.name} stores a parameter into static `{tx}`")

    def dump(name, header, rows):
        with open(os.path.join(a.out, name), "w") as f:
            f.write("\t".join(header) + "\n")
            for r in rows: f.write("\t".join(str(x).replace("\t", " ").replace("\n", " ") for x in r) + "\n")
    dump("generic_owned_T.tsv", ["corpus", "unit", "expect", "class", "member", "parameter", "verdict", "detail", "parameter_of", "dispatched"], gen_rows)
    dump("generic_inst_at_bb.tsv", ["corpus", "unit", "expect", "line", "generic", "arguments"], inst_rows)
    dump("throws.tsv", ["corpus", "unit", "expect", "line", "class", "identifier_arguments"], throw_rows)
    dump("async.tsv", ["corpus", "unit", "expect", "line", "member", "what", "detail", "sources"], async_rows)
    dump("dtor.tsv", ["corpus", "unit", "expect", "class", "borrow_fields", "named_in_destructor", "instance_methods_called"], dtor_rows)
    dump("interfaces.tsv", ["corpus", "unit", "expect", "interface", "why"], iface_rows)
    dump("static_stores.tsv", ["corpus", "unit", "expect", "line", "member", "static"], static_rows)
    dump("rev2_findings.tsv", ["finding", "corpus", "unit", "expect", "line", "where", "detail"], rev2_rows)

    # ---- the note's tables, re-read
    def tsv(p):
        rows = [l.rstrip("\n").split("\t") for l in open(p)]
        return [dict(zip(rows[0], r)) for r in rows[1:]]
    stores = tsv(os.path.join(base, "stores.tsv")); holders = tsv(os.path.join(base, "holders.tsv")); roll = tsv(os.path.join(base, "rollup.tsv"))
    classes = tsv(os.path.join(base, "classes.tsv"))
    hand = {r["unit"]: r for r in tsv(os.path.join(NOTE, "hand_check.tsv"))}
    T = [s for s in stores if s["population"] == "T"]
    out = []; P = out.append
    P("proposal 0212 corpus measurement — the note's scanner re-run (base/) plus the 0212 passes")
    P(f"units: {dict(Counter(c for c, _, _ in units))} (total {len(units)}); parse errors {stat['parse_errors']}")
    P("")
    P("== the note's numbers, reproduced at this base (base/summary.txt is byte-identical to ../../data/out/summary.txt when the tree is unchanged)")
    P(f"K4.3 field/collection store sites: {len(T)}; by storing member kind: {dict(Counter(s['member_kind'] for s in T))}")
    P(f"construction sites of a borrow-bearing class: {len(holders)}; by holder position: {dict(Counter(h['holder'] for h in holders))}")
    P(f"borrow-bearing classes: {sum(1 for c in classes if c['status'] == 'bb')} of {len(classes)} class rows in classes.tsv (the note's summary counts every class)")
    def tier(s):
        if s["corpus"] == "gtest": return "gtest"
        return "accepted" if s["expect"] in ACC else "expected-reject"
    for tname in ("accepted", "expected-reject", "gtest"):
        rows = [s for s in T if tier(s) == tname]
        if tname == "gtest":
            seen = {}
            for s in rows: seen.setdefault((s["unit"], s["class"], s["member"], s["target"], s["source"]), s)
            rows = list(seen.values())
        v = Counter()
        for s in rows:
            a0 = s["A0_store"]
            if a0.startswith("ok"): v["admitted as written"] += 1
            elif a0.startswith("edit"): v["needs `scoped` on a member that is not a constructor"] += 1
            elif "does-not-hold" in a0: v["rejected: a store that relates two parameters (rewrite: the storing object is the receiver, `scoped` on the stored one)"] += 1
            elif "Object" in a0: v["rejected: a method keeps an `Object` parameter (rewrite: declare the parameter at the stored type)"] += 1
            else: v["other:" + a0] += 1
        P(f"  [{tname}] {len(rows)} store sites under 0212 R4: {dict(v)}")
    P("")
    P("== R1 interfaces")
    P(f"interfaces declared in the corpora: {stat['interfaces']}; borrow-bearing by derivation: {stat['interfaces_bb_derived']} "
      f"(by a class- or interface-typed `scoped` parameter: {stat['interfaces_bb_by_scoped_parameter']}; by a property of a class or interface type declared in the unit and not marked readonly: {stat['interfaces_bb_by_settable_property']})")
    for r in iface_rows: P(f"    {r[1]} [{r[0]}/{r[2]}] interface {r[3]}: {r[4]}")
    P(f"borrow-bearing classes that list a base or interface that is not borrow-bearing: {stat['bb_class_with_non_bb_base_or_interface']} (of which implement an interface declared in the same unit: {stat['bb_class_implementing_unit_interface_not_bb']})")
    up = [h for h in holders if h.get("upcast")]
    P(f"owned upcasts of a borrow-bearing instance to a type that is not borrow-bearing (the note's count, re-read): {len(up)}")
    P("")
    P("== R5 owned erasure in generic bodies")
    P(f"`owned` parameters typed at a type parameter, in members of generic declarations: {stat['generic_owned_T_params']}")
    P("  by where the parameter goes: " + str({k.split(':')[1]: v for k, v in sorted(stat.items()) if k.startswith('generic_owned_T:')}))
    P(f"  of which in a dispatched generic method (an interface, virtual, abstract or override method with its own type parameter): {stat['generic_owned_T_in_dispatched_generic_method']}")
    er = [g for g in gen_rows if g[6].startswith("ERASE")]
    for g in er: P(f"    ERASING: {g[1]} [{g[0]}/{g[2]}] {g[3]}.{g[4]}({g[5]}) -> {g[6]} {g[7]}")
    P(f"  instantiations of an erasing declaration named in its unit: {stat['erasing_generic_instantiations']}; at a class or interface: {stat['erasing_generic_instantiations_at_a_class']}; at a borrow-bearing class: {stat['erasing_generic_instantiations_at_a_borrow_bearing_class']}")
    P(f"generic instantiations whose type argument is a borrow-bearing class of the unit: {stat['generic_instantiations_at_a_bb_class']} "
      f"(user generic: {stat['generic_inst_at_bb:user']}, standard or unknown generic: {stat['generic_inst_at_bb:standard']})")
    P("  by generic: " + str(dict(Counter(r[4] for r in inst_rows).most_common(20))))
    P(f"`throw new C(...)` of a borrow-bearing class C: {stat['throw_of_new_bb_instance']}; with an identifier argument (a possible carried source): {stat['throw_of_new_bb_instance_with_identifier_argument']}")
    for r in throw_rows: P(f"    {r[1]} [{r[0]}/{r[2]}] line {r[3]}: throw new {r[4]}({r[5]})")
    P("")
    P("== R6 the destructor window")
    P(f"borrow-bearing classes with a destructor: {stat['bb_classes_with_destructor']}; whose destructor body names one of its borrow fields: {stat['bb_dtor_names_a_borrow_field']}; whose destructor calls an instance method of its class: {stat['bb_dtor_calls_an_instance_method']}")
    for r in dtor_rows: P(f"    {r[1]} [{r[0]}/{r[2]}] class {r[3]}: borrow fields {r[4] or '-'}; named in destructor: {r[5] or '-'}; instance methods called: {r[6] or '-'}")
    selfs = [h for h in holders if "this" in (h.get("carried_A0") or "")]
    fld = [h for h in holders if h["holder"] in ("field-owned", "field-init")]
    P(f"constructions that carry `this` (self-sourced): {len(selfs)}; constructions held directly in a field: {len(fld)}")
    for h in selfs + [x for x in fld if x not in selfs]:
        P(f"    {h['unit']} [{h['corpus']}/{h['expect']}] line {h['line']}: new {h['cls']} in {h['in_member']} held as {h['holder']} {h['holder_name']}; carries {h['carried_A0']}")
    k1 = [s for s in T if s["kind"] == "1" or s["source_kind"] in ("this", "this-path", "ownedfield")]
    P(f"stores whose source is the receiver, one of its fields, or kind 1: {len(k1)}")
    P("")
    P("== R7 async")
    P(f"`async` methods: {stat['async_methods']}; with a parameter of borrow-bearing class type: plain {stat['async_param_of_bb_class:plain']}, owned {stat['async_param_of_bb_class:owned']}; with a task result of borrow-bearing class type: {stat['async_result_of_bb_class']}")
    P(f"a local instance of a borrow-bearing class, built in an `async` body, with an `await` between its declaration and a later use: {stat['async_bb_local_live_across_await']} "
      f"(over a parameter or `this`: {stat['async_bb_local_across_await:caller-sourced']}; over frame-resident sources or none: {stat['async_bb_local_across_await:frame-resident-or-none']})")
    for r in async_rows: P(f"    {r[1]} [{r[0]}/{r[2]}] line {r[3]} {r[4]}: {r[5]} {r[6]} {r[7]}")
    P("")
    P("== R4 static-held stores")
    P(f"a method that stores a plain parameter into a static field or a static-held collection: {stat['static_held_store_of_a_parameter']}")
    for r in static_rows: P(f"    {r[1]} [{r[0]}/{r[2]}] line {r[3]} {r[4]} -> static {r[5]}")
    P("")
    P("== REVISION 2: one pass per critique finding (rev2_findings.tsv)")
    for k in sorted(k for k in stat if k.startswith("rev2:")):
        rows = [r for r in rev2_rows if r[0] == k[5:]]
        acc_n = sum(1 for r in rows if r[1] != "gtest" and r[3] in ACC)
        P(f"  {k[5:]}: {stat[k]}" + (f" (accepted tier: {acc_n}; gtest: {sum(1 for r in rows if r[1] == 'gtest')})" if rows else ""))
    show = [r for r in rev2_rows if not r[0].startswith("F1 out-owned parameter") or "borrow-bearing" in r[0]]
    show = [r for r in show if not r[0].startswith("F4 move of a field") and not r[0].startswith("F3 owned parameter of a borrow-bearing class")]
    for r in show[:60]: P(f"    {r[0]} | {r[2]} [{r[1]}/{r[3]}] line {r[4]} {r[5]} {r[6]}")
    P("")
    P("== N11: doc-extracted code blocks that hold a store whose verdict changes at stage 3 (for the spec train's §8.13)")
    doc_sec = {}
    if os.path.exists(mp):
        for line in open(mp):
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 3: doc_sec[parts[0]] = (parts[1], parts[2])
    for srow in stores:
        if srow["corpus"] == "docs" and srow["population"] == "T":
            cid = os.path.basename(srow["unit"])[:-4]
            d_, sec_ = doc_sec.get(cid, ("?", "?"))
            v_ = srow["A0_store"]
            P(f"    {cid} ({d_}, {sec_}) [{srow['expect']}] {srow['class']}.{srow['member']}: " + ("needs `scoped`" if v_.startswith("edit") else ("rejected" if v_.startswith("reject") else "admitted as written; its comment cites the trust clause")))
    P("")
    # ---- roll-up
    P("== PER-PROGRAM ROLL-UP under 0212, accepted tier (rules, docs, lit units the spec expects to compile)")
    v = Counter(); rows = []
    for r in roll:
        u = r["unit"]; a0 = r["A0"]
        h = hand.get(u)
        if h: a0 = h["A0"]
        extra = new_reject.get(u, [])
        if extra: a0 = "reject"
        v[a0] += 1; rows.append((u, a0, "; ".join(extra)))
    only_new = [u for u in new_reject if u not in {r["unit"] for r in roll}]
    for u in only_new:
        v["reject"] += 1; rows.append((u, "reject", "; ".join(new_reject[u])))
    dump("rollup_0212.tsv", ["unit", "verdict_0212", "new_rule_rejections"], rows)
    total_acc = sum(1 for c, p, t in units if c != "gtest" and S.unit_expect(c, p, t) in ACC)
    P(f"{total_acc} accepted-tier programs; {len(rows)} hold a K4.3 store, construct a borrow-bearing class, or meet a 0212 pass")
    P(f"  unchanged {v['ok']}; compile after a `scoped` marker {v['edit']}; rejected, must be rewritten {v['reject']}")
    P(f"  newly rejected by the rules 0212 adds to the note's candidate (generic erasure, static store): {len(new_reject)} programs")
    for u, lst in sorted(new_reject.items()): P(f"    {u}: {'; '.join(lst)}")
    P("  the rejected programs: " + ", ".join(sorted(os.path.basename(r[0]) for r in rows if r[1] == "reject")))
    open(os.path.join(a.out, "summary_0212.txt"), "w").write("\n".join(out) + "\n")
    print("\n".join(out))

if __name__ == "__main__":
    main()
