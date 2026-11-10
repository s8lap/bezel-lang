#!/usr/bin/env python3
"""scan_collection_transfer.py — proposal 0212, R2 clause 2 with K5.5: a store of a local owned binding into a local
borrowing collection makes the collection carry it, so a later transfer of that binding is frozen while the collection
is still named. Today that transfer is admitted and reported (proposal 0157's entry). This heuristic counts the shape:
in one member body, `C.Add(v)` / `C.Insert(…, v)` / `C.Push(v)` / `C.Enqueue(v)` / `C[k] := v` / `C.Add(k, v)` with `v`
a local declared `var v := new …` (or `using var`), then `move v` later in the body, then a later mention of `C`.

Usage: python3 -I scan_collection_transfer.py --head <repo> --docs <exported> [--out FILE]
Syntactic, per unit, text order; the collection must be a local declared in the body with a borrowing collection type
or initializer (Array, Dictionary, Set, Stack, Queue, LinkedList, SortedDictionary, SortedSet)."""
import argparse, os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..", "..", "data")))
import bzlex as L
import scan_trust as S
import scan_static as SS
BORROWING = {"Array", "Dictionary", "Set", "Stack", "Queue", "LinkedList", "SortedDictionary", "SortedSet"}
STORES = {"Add", "Insert", "Push", "Enqueue", "AddLast", "AddFirst"}

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--head", required=True); ap.add_argument("--docs", required=True); ap.add_argument("--out")
    a = ap.parse_args()
    mp = os.path.join(a.docs, "tests/conformance/manifest.tsv")
    if os.path.exists(mp):
        for line in open(mp):
            p = line.rstrip("\n").split("\t")
            if len(p) >= 5 and p[0] != "case-id": S.DOC_MANIFEST[p[0]] = p[3] + "/" + p[4]
    rows = []; nunits = 0
    for corpus, path, text in S.collect_units(a.head, a.docs):
        nunits += 1
        expect = S.unit_expect(corpus, path, text)
        try: classes, toks = L.parse_unit(text)
        except Exception: continue
        for c in classes:
            for m in c.members:
                body = m.body or []; bt = [t.text for t in body]; n = len(bt)
                if n < 8: continue
                # local declarations
                owned_locals = {}; colls = {}
                for i in range(n - 3):
                    if bt[i] == "var" and body[i+1].kind == "id":
                        name = bt[i+1]
                        j = i + 2; typ = None
                        if bt[j] == ":":
                            typ = bt[j+1]; 
                            while j < n and bt[j] != ":=" and bt[j] != ";": j += 1
                        if j < n and bt[j] == ":=":
                            if j + 2 < n and bt[j+1] == "new":
                                head = bt[j+2]
                                if head in BORROWING: colls[name] = i
                                else: owned_locals[name] = i
                            elif typ in BORROWING: colls[name] = i
                        elif typ in BORROWING: colls[name] = i
                if not owned_locals or not colls: continue
                for i in range(n - 4):
                    cname = bt[i]
                    if cname not in colls or i < colls[cname]: continue
                    stored = None
                    if bt[i+1] == "." and bt[i+2] in STORES and bt[i+3] == "(":
                        d = 0; e = i + 3; args = []
                        while e < n:
                            if bt[e] == "(": d += 1
                            elif bt[e] == ")":
                                d -= 1
                                if d == 0: break
                            elif d == 1 and body[e].kind == "id" and bt[e] in owned_locals and bt[e-1] in ("(", ",") and bt[e+1] in (")", ","): args.append(bt[e])
                            e += 1
                        stored = args
                    elif bt[i+1] == "[":
                        e = i + 1; d = 0
                        while e < n:
                            if bt[e] == "[": d += 1
                            elif bt[e] == "]":
                                d -= 1
                                if d == 0: break
                            e += 1
                        if e + 3 < n and bt[e+1] == ":=" and bt[e+2] in owned_locals and bt[e+3] == ";": stored = [bt[e+2]]
                    if not stored: continue
                    for v in stored:
                        if owned_locals[v] > i: continue
                        mv = [k for k in range(i + 4, n - 1) if bt[k] == "move" and bt[k+1] == v]
                        if not mv: continue
                        later = any(bt[k] == cname for k in range(mv[0] + 2, n))
                        rows.append((corpus, os.path.relpath(path, a.head) if path.startswith(a.head) else os.path.relpath(path, a.docs), expect, body[i].line, f"{c.name}.{m.name}", cname, v, "collection named after the transfer" if later else "collection not named after the transfer"))
    out = [f"units scanned: {nunits}", f"store of a local owned binding into a local borrowing collection, then a `move` of that binding: {len(rows)} site(s)",
           f"  of which the collection is named again after the transfer (rejected by K5.1 through K5.5 under 0212): {sum(1 for r in rows if r[7].startswith('collection named'))}",
           f"  in the accepted tier (rules, docs, lit; expect in {SS.ACC}): {sum(1 for r in rows if r[0] != 'gtest' and r[2] in SS.ACC and r[7].startswith('collection named'))}"]
    for r in rows: out.append("  " + "\t".join(str(x) for x in r))
    txt = "\n".join(out) + "\n"
    if a.out: open(a.out, "w").write(txt)
    print(txt)
if __name__ == "__main__":
    main()
