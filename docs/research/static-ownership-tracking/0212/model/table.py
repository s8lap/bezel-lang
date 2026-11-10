#!/usr/bin/env python3
"""table.py — render out/compare_*.txt as the table of proposal 0212 §5.2 (one row per sweep, the 0212 candidate),
and print the figures the proposal quotes from the other two candidates. Usage: python3 -I table.py"""
import os, re, sys
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
ROWS = [("v3l4", "4 statements"), ("v3l4_kdtor", "4, holder destructor reads"), ("v3l4_qdtor", "4, child destructor reads (§28.12 timing)"),
        ("v3l4_kq", "4, both destructors"), ("v2l4_if", "4, 2 variables, `if`"), ("v3l4_if", "4, `if`"), ("v3l4_while", "4, `while`"),
        ("v3l4_block", "4, inner `do … end`"), ("v3l4_block_kq", "4, inner `do … end`, both destructors"),
        ("v3l5", "5 statements"), ("v3l5_kq", "5, both destructors")]
def n(x): return f"{int(x):,}"
def parse(tag):
    p = os.path.join(OUT, f"compare_{tag}.txt")
    if not os.path.exists(p): return None
    t = open(p).read()
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
    return d
print("| bounds (3 variables unless stated) | programs | accepted | trust surface | 0212: newly rejected = race + fault every + fault some + dangling never read + clean | 0212: left (race; fault every / some) | newly accepted / unsound |")
print("|---|---|---|---|---|---|---|")
for tag, label in ROWS:
    d = parse(tag)
    if not d or "0212" not in d["cands"]:
        print(f"| {label} | (sweep not finished) | | | | | |"); continue
    c = d["cands"]["0212"]; s = d["surface"]
    print(f"| {label} | {n(d['programs'])} | {n(d['accepted'])} | {n(s[0])} = {n(s[1])} + {n(s[2])} + {n(s[3])} | {n(c['rej'])} = {n(c['race'])} + {n(c['every'])} + {n(c['some'])} + {n(c['dang'])} + {n(c['clean'])} | {n(c['lrace'])}; {n(c['levery'])} / {n(c['lsome'])} | {c['newacc']} / {c['unsound']} |")
print()
for tag, label in ROWS + [("v3l4_qdtor_r40", "4, child destructor reads, r40 flag timing"), ("v3l5_qdtor_r40", "5, child destructor reads, r40 flag timing")]:
    d = parse(tag)
    if not d: continue
    a = d["cands"].get("A"); z = d["cands"].get("0212"); e = d["cands"].get("0212-explicit")
    if not (a and z and e): continue
    print(f"{label}: baseline faults every/some {d['flag'][0]}/{d['flag'][1]}; note's A: rejected {a['rej']}, left race {a['lrace']} faults {a['levery']}/{a['lsome']}; 0212: accepted {z['acc']} rejected {z['rej']} clean {z['clean']} left race {z['lrace']} faults {z['levery']}/{z['lsome']}; explicit: accepted {e['acc']} rejected {e['rej']} clean {e['clean']} left faults {e['levery']}/{e['lsome']}; unsound A/0212/explicit {a['unsound']}/{z['unsound']}/{e['unsound']}; newly accepted {a['newacc']}/{z['newacc']}/{e['newacc']}")
