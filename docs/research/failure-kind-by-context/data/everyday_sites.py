#!/usr/bin/env python3
"""Census of the call sites in the ergonomics study's 40 everyday programs
(docs/research/ergonomics-study/programs/*/final.bzl) where a condition of the scenario
table can arise, and of the stance the program takes there.

A site is a line matching one of the patterns below. For each site the script records the
form used (`throwing` or `query`) and whether the line sits inside a `try` body. The
operand's origin and what a raise would mean at that site cannot be read mechanically:
they are hand annotations in everyday_sites_annot.tsv, joined here by program and line; the
script fails if an annotated line's text has changed or a site has no annotation.

Usage: everyday_sites.py <repo-root> <out-dir>
Writes <out-dir>/everyday_sites.tsv and prints the summary (also written to
everyday_sites_summary.json).
"""
import collections, csv, glob, json, os, re, sys

root, out = sys.argv[1:3]
here = os.path.dirname(os.path.abspath(__file__))
PAT = [
    ("parse", "throwing", r"\b(?:int|long|double|decimal|DateTime|DateOnly|TimeOnly|Guid|bool|byte|short|uint|ulong|float)\.Parse\("),
    ("parse", "query", r"\.TryParse\(|\.ParseOrNull\("),
    ("io", "throwing", r"\bFile\.(?:ReadAll\w+|WriteAll\w+|Delete|Copy|Move)\(|new FileStream\(|\bDirectory\.(?:CreateDirectory|Delete|GetFiles|GetDirectories)\("),
    ("io", "query", r"\b(?:File|Directory)\.Exists\("),
    ("decode", "throwing", r"\.Deserialize\(|new JsonDeserializer\("),
    ("state", "query", r"\.GetOrNull\(|\.TryGetValue\(|OrNull\(\)|\.TryPop\(|\.TryDequeue\(|\.TryPeek\(|\.TryReceive\("),
    ("state", "throwing", r"\.First\(\)|\.Last\(\)|\.Single\(\)|\.Pop\(\)|\.Dequeue\(\)|\.Peek\(\)|\.Average\(|\.Max\(\)|\.Min\(\)"),
]

def strip_comment(line):
    i = line.find("//")
    return line if i < 0 else line[:i]

rows = []
for p in sorted(glob.glob(os.path.join(root, "docs/research/ergonomics-study/programs/*/final.bzl"))):
    prog = os.path.basename(os.path.dirname(p))
    lines = open(p, encoding="utf-8").read().split("\n")
    # a line is "in try" when a `try` opened above it has not yet reached its `catch`/`finally`
    depth = 0; in_try = []
    for ln in lines:
        code = strip_comment(ln).strip()
        if re.match(r"(catch|finally)\b", code) and depth > 0: depth -= 1
        in_try.append(depth > 0)
        if re.search(r"\btry\b\s*$", code) or code == "try": depth += 1
    for n, ln in enumerate(lines, 1):
        code = strip_comment(ln)
        for group, form, rx in PAT:
            if group == "state" and form == "query" and re.search(r"\.ParseOrNull\(", code): continue
            if re.search(rx, code):
                rows.append([prog, n, group, form, "yes" if in_try[n - 1] else "no", code.strip()])

annot = {}
for r in csv.DictReader(open(os.path.join(here, "everyday_sites_annot.tsv"), encoding="utf-8"), delimiter="\t"):
    annot[(r["program"], int(r["line"]), r["group"])] = r
missing = [r for r in rows if (r[0], r[1], r[2]) not in annot]
if missing:
    for r in missing: print("NO ANNOTATION:", "\t".join(str(x) for x in r))
    sys.exit(1)
os.makedirs(out, exist_ok=True)
with open(os.path.join(out, "everyday_sites.tsv"), "w", encoding="utf-8") as f:
    f.write("program\tline\tgroup\tform\tin_try\toperand\ta_raise_here_means\ttext\n")
    for r in rows:
        a = annot[(r[0], r[1], r[2])]
        if a["text"] != r[5]:
            print("TEXT CHANGED:", r[0], r[1]); sys.exit(1)
        f.write("\t".join([r[0], str(r[1]), r[2], r[3], r[4], a["operand"], a["a_raise_here_means"], r[5]]) + "\n")
        r += [a["operand"], a["a_raise_here_means"]]

S = collections.OrderedDict()
S["programs"] = len(glob.glob(os.path.join(root, "docs/research/ergonomics-study/programs/*/final.bzl")))
S["sites"] = len(rows)
S["sites_by_group_and_form"] = {f"{g}/{f}": n for (g, f), n in sorted(collections.Counter((r[2], r[3]) for r in rows).items())}
S["throwing_sites_in_a_try"] = sum(1 for r in rows if r[3] == "throwing" and r[4] == "yes")
S["throwing_sites_outside_any_try"] = sum(1 for r in rows if r[3] == "throwing" and r[4] == "no")
fail = [r for r in rows if r[2] in ("parse", "io", "decode") and r[3] == "throwing"]
S["failure_typed_throwing_sites"] = len(fail)
S["failure_typed_throwing_sites_by_what_a_raise_means"] = dict(collections.Counter(r[7] for r in fail))
S["failure_typed_throwing_sites_by_operand"] = dict(collections.Counter(r[6] for r in fail))
S["parse_sites_by_form_and_operand"] = {f"{f}/{o}": n for (f, o), n in sorted(collections.Counter((r[3], r[6]) for r in rows if r[2] == "parse").items())}
S["state_sites_by_form"] = dict(collections.Counter(r[3] for r in rows if r[2] == "state"))
json.dump(S, open(os.path.join(out, "everyday_sites_summary.json"), "w"), indent=1)
print(json.dumps(S, indent=1))
