#!/usr/bin/env python3
"""table.py — render out/compare_*.txt as the table of proposal 0212 §5.2 (one row per sweep), and print the figures
the proposal quotes from each candidate. Usage: python3 -I table.py"""
import os, re, sys
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
ROWS = [("v3l4", "4 statements"), ("v3l4_kdtor", "4, holder destructor reads"), ("v3l4_qdtor", "4, child destructor reads (§28.12 timing)"),
        ("v3l4_kq", "4, both destructors"), ("v3l4_kq_hidden", "4, both destructors, children linked in constructors"),
        ("v2l4_if", "4, 2 variables, `if`"), ("v3l4_if", "4, `if`"), ("v3l4_while", "4, `while`"),
        ("v3l4_block", "4, inner `do … end`"), ("v3l4_block_kq", "4, inner `do … end`, both destructors"),
        ("v3l5", "5 statements"), ("v3l5_kq", "5, both destructors"), ("v3l5_kq_hidden", "5, both destructors, children linked in constructors")]
EXTRA = [("v3l4_qdtor_r40", "4, child destructor reads, r40 flag timing"), ("v3l5_qdtor_r40", "5, child destructor reads, r40 flag timing")]
def n(x): return f"{int(x):,}"
def parse(tag):
    p = os.path.join(OUT, f"compare_{tag}.txt")
    if not os.path.exists(p): return None
    t = open(p).read()
    if "features=p0212r" not in t: return None          # an output of the first draft's feature set: not comparable
    m = re.search(r"baseline: (\d+) programs, (\d+) accepted", t)
    s = re.search(r"STORED BORROW\): (\d+) = race (\d+) \+ fault on every run (\d+) \+ fault on some run (\d+)", t)
    f = re.search(r"any cause\): (\d+) / (\d+)", t)
    if not (m and s): return None
    d = dict(programs=m.group(1), accepted=m.group(2), surface=s.groups(), flag=f.groups() if f else None, cands={})
    for line in t.split("\n"):
        if line.startswith("| ") and not line.startswith("| candidate") and not line.startswith("|---"):
            c = [x.strip() for x in line.strip().strip("|").split("|")]
            if len(c) < 12: continue
            name = c[0].split("  ")[0]
            eh, el = c[6].split("/"); sh, sl = c[7].split("/"); dg, cl = [x.strip() for x in c[8].split("/")]
            le, ls = c[10].split("/")
            d["cands"][name] = dict(acc=c[1], newacc=c[2], unsound=c[3], rej=c[4], race=c[5], every=int(eh) + int(el), some=int(sh) + int(sl), dang=dg, clean=cl, lrace=c[9], levery=le, lsome=ls)
    return d if len(d["cands"]) == 4 else None
print("| bounds (3 variables unless stated) | programs | accepted | trust surface = race + fault every + fault some | first draft: left (race; fault every / some) | revised: newly rejected = race + fault every + fault some + dangling never read + clean | revised: left (race; fault every / some) |")
print("|---|---|---|---|---|---|---|")
for tag, label in ROWS:
    d = parse(tag)
    if not d:
        print(f"| {label} | (sweep not finished) | | | | | |"); continue
    c = d["cands"]["0212"]; d1 = d["cands"]["0212-draft1"]; s = d["surface"]
    print(f"| {label} | {n(d['programs'])} | {n(d['accepted'])} | {n(s[0])} = {n(s[1])} + {n(s[2])} + {n(s[3])} | {n(d1['lrace'])}; {n(d1['levery'])} / {n(d1['lsome'])} | {n(c['rej'])} = {n(c['race'])} + {n(c['every'])} + {n(c['some'])} + {n(c['dang'])} + {n(c['clean'])} | {n(c['lrace'])}; {n(c['levery'])} / {n(c['lsome'])} |")
print()
for tag, label in ROWS + EXTRA:
    d = parse(tag)
    if not d: continue
    a = d["cands"]["A"]; d1 = d["cands"]["0212-draft1"]; z = d["cands"]["0212"]; e = d["cands"]["0212-explicit"]
    print(f"{label}: baseline faults every/some {d['flag'][0]}/{d['flag'][1]}; A: rej {a['rej']} left {a['lrace']};{a['levery']}/{a['lsome']} | draft1: acc {d1['acc']} rej {d1['rej']} clean {d1['clean']} left {d1['lrace']};{d1['levery']}/{d1['lsome']} | revised: acc {z['acc']} rej {z['rej']} clean {z['clean']} left {z['lrace']};{z['levery']}/{z['lsome']} | explicit: acc {e['acc']} rej {e['rej']} clean {e['clean']} left {e['lrace']};{e['levery']}/{e['lsome']} | newly accepted/unsound (all four): {a['newacc']},{d1['newacc']},{z['newacc']},{e['newacc']} / {a['unsound']},{d1['unsound']},{z['unsound']},{e['unsound']}")
