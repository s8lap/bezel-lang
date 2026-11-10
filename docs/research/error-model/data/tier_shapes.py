#!/usr/bin/env python3
"""Classify every stdlib Try member by its stdlib §1 shape, mark which value-tier
members have a throwing twin on the same type (so a call-site "to null" form
could derive them), and count the corpus call sites per member name.

Shapes (stdlib §1): (a) bool + nullable/generic out, (b) nullable return,
(c) bool + non-nullable value out, (d) bool, no out.

Usage: tier_shapes.py <repo-root> <export-root> <data-dir>
"""
import collections, csv, glob, json, os, re, sys
root, export, data = sys.argv[1:4]
sys.argv = [sys.argv[0], root, export, data]
lines = open(os.path.join(root, "docs/Bezel_Standard_Library.md"), encoding="utf-8").read().split("\n")
members = list(csv.DictReader(open(os.path.join(data, "stdlib_members.tsv"), encoding="utf-8"), delimiter="\t"))

def shape(m):
    decl = lines[int(m["line"]) - 1]
    # a declaration may continue on following lines
    k = int(m["line"])
    while ")" not in decl.split("//")[0] and k < len(lines):
        decl += " " + lines[k].split("//")[0]; k += 1
    decl = decl.split("//")[0]
    out = re.search(r"\bout\s+(?:owned\s+)?\w+\s*:\s*([^,)]+)", decl)
    ret = m["ret"]
    if out:
        t = out.group(1).strip()
        t = re.sub(r"\s+when\s+true$", "", t)
        return "a" if t.endswith("?") or re.fullmatch(r"[TKV]\??", t) else "c"
    if ret.rstrip().endswith("?") or re.search(r"\?>?$", ret):
        return "b"
    return "d"

base = collections.defaultdict(set)
for m in members:
    if m["tier"] in ("plain", "nullable"):
        base[m["type"]].add(m["name"])
INDEXER_TWIN = {"TryGetValue", "GetOrNull"}          # twin is the `[key]` indexer
rows = []
for m in members:
    if m["tier"] not in ("try", "ornull"):
        continue
    stem = re.sub(r"^Try", "", m["name"]).replace("OrNull", "")
    twin = stem in base[m["type"]] or (m["name"] in INDEXER_TWIN and m["type"].split("<")[0] in
            ("Dictionary", "OwningDictionary", "SortedDictionary"))
    sh = shape(m) if m["tier"] == "try" else "ornull"
    rows.append(dict(type=m["type"], name=m["name"], tier=m["tier"], shape=sh, throwing_twin=int(twin)))

# corpus call sites per member name (reuse the scanner's text preparation)
src = open(os.path.join(data, "corpus_usage.py"), encoding="utf-8").read()
ns = {}
exec(src.split("# ---------------------------------------------------------------- patterns")[0]
     .replace("root, export, out = sys.argv[1:4]", ""), ns)
blank, cpp_literals = ns["blank_bezel"], ns["cpp_literals"]
names = {r["name"] for r in rows}
calls = collections.Counter()
rx = re.compile(r"\b(Try[A-Z]\w*|\w*OrNull)\s*(?:<[^<>()]*>)?\s*\(")
files = [("bzl", p) for p in glob.glob(os.path.join(root, "tests/conformance/rules/cases/**/*.bzl"), recursive=True)]
files += [("bzl", p) for p in glob.glob(os.path.join(export, "tests/conformance/cases/*.bzl"))]
files += [("bzl", p) for p in glob.glob(os.path.join(root, "tests/lit/**/*.bzl"), recursive=True)]
files += [("cpp", p) for p in glob.glob(os.path.join(root, "tests/**/*.cpp"), recursive=True)
          if "/conformance/" not in p and "/lit/" not in p]
for kind, p in files:
    s = open(p, encoding="utf-8", errors="replace").read()
    if kind == "cpp":
        s = cpp_literals(s)
    for n in rx.findall(blank(s)):
        calls[n] += 1

by = collections.defaultdict(lambda: dict(decls=0, with_twin=0, names=set()))
for r in rows:
    b = by[r["shape"]]
    b["decls"] += 1; b["with_twin"] += r["throwing_twin"]; b["names"].add(r["name"])
with open(os.path.join(data, "stdlib_tier_shapes.tsv"), "w") as f:
    f.write("type\tname\ttier\tshape\tthrowing_twin\tcorpus_calls_of_name\n")
    for r in rows:
        f.write(f'{r["type"]}\t{r["name"]}\t{r["tier"]}\t{r["shape"]}\t{r["throwing_twin"]}\t{calls[r["name"]]}\n')
summ = {}
for sh, b in sorted(by.items()):
    summ[sh] = dict(declarations=b["decls"], with_throwing_twin=b["with_twin"], distinct_names=len(b["names"]),
                    corpus_call_sites=sum(calls[n] for n in b["names"]))
summ["corpus_calls_to_names_not_in_stdlib_listing"] = sum(v for n, v in calls.items() if n not in names)
json.dump(summ, open(os.path.join(data, "stdlib_tier_shapes.json"), "w"), indent=1)
print(json.dumps(summ, indent=1))
print(sorted(((v, n) for n, v in calls.items() if n not in names), reverse=True)[:15])
