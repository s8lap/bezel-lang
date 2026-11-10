#!/usr/bin/env python3
"""Inventory the failure-signal surface of docs/Bezel_Standard_Library.md.

Reads every fenced ```bezel block, tracks the enclosing type declaration, and
records each member declaration (method / constructor / property / indexer).
Each member is put in a failure-signal tier by its spelling:

  try      name starts with Try + capital        (stdlib §1 Try convention)
  ornull   name contains OrNull                  (stdlib §1 *OrNull tier)
  choice   return type is Result<...> or a choice declared in the document
  nullable return type ends in '?' (and not try/ornull)
  plain    everything else

A member is "documented-throwing" when its declaration line or its trailing
`//` comment lines name an exception type or say raises/throws/faults.  That is
a LOWER BOUND on the throwing surface: most fault contracts are stated in the
prose beneath the block, which this script counts separately (per section, the
distinct exception type names mentioned).

Usage: stdlib_inventory.py <repo-root> <out-dir>
"""
import collections
import json
import os
import re
import sys

root, out = sys.argv[1], sys.argv[2]
path = os.path.join(root, "docs/Bezel_Standard_Library.md")
lines = open(path, encoding="utf-8").read().split("\n")

SEC = re.compile(r"^(#{2,4}) (\d+(?:\.\d+)*)\.? ")
TYPE = re.compile(
    r"^\s*(?:public |internal |private |sealed |abstract |static |open |partial )*"
    r"(class|record|interface|choice|enum|extension)\s+([A-Za-z_][\w]*(?:<[^>]*>)?)")
MEMBER = re.compile(
    r"^\s*(?:public |internal |protected |private |static |pure |async |override |virtual |"
    r"abstract |sealed |readonly |owned |new )*"
    r"(method|constructor|property|operator|indexer)\b\s*([A-Za-z_][\w]*)?")
EXC = re.compile(r"\b([A-Z]\w*Exception)\b")
FAULTWORD = re.compile(r"\b(raises?|throws?|faults?)\b")

sections = []          # (lineno, number)
members = []
choices = set()
exc_by_section = collections.defaultdict(collections.Counter)

in_block = False
cur_sec = "0"
cur_type = None
pending = None         # member whose trailing comment lines are still being read

def close_pending():
    global pending
    if pending is not None:
        members.append(pending)
        pending = None

for i, line in enumerate(lines, 1):
    m = SEC.match(line)
    if m and not in_block:
        cur_sec = m.group(2)
        sections.append((i, cur_sec))
    if line.strip().startswith("```"):
        if not in_block and line.strip() == "```bezel":
            in_block = True
            cur_type = None
        elif in_block:
            close_pending()
            in_block = False
        continue
    if not in_block:
        for e in EXC.findall(line):
            exc_by_section[cur_sec][e] += 1
        continue
    code, _, comment = line.partition("//")
    if not code.strip():                       # comment-only continuation line
        if pending is not None:
            pending["comment"] += " " + comment.strip()
        continue
    close_pending()
    t = TYPE.match(code)
    if t:
        cur_type = t.group(2)
        if t.group(1) == "choice":
            choices.add(re.sub(r"<.*", "", t.group(2)))
        continue
    if re.match(r"^\s*end\b", code):
        continue
    mm = MEMBER.match(code)
    if not mm:
        continue
    kind, name = mm.group(1), mm.group(2) or ""
    if kind == "constructor":
        name = "constructor"
    ret = ""
    r = re.search(r"\)\s*:\s*([^;=]+?)\s*(?:;|=>|$| requires| ensures| where)", code)
    if r:
        ret = r.group(1).strip()
    elif kind == "property":
        r = re.search(r"property\s+\w+\s*:\s*([^;{=]+)", code)
        ret = r.group(1).strip() if r else ""
    pending = dict(line=i, section=cur_sec, type=cur_type or "", kind=kind,
                   name=name, ret=ret, decl=code.strip(), comment=comment.strip())
close_pending()

def tier(m):
    n = m["name"]
    if re.match(r"^Try[A-Z]", n):
        return "try"
    if "OrNull" in n:
        return "ornull"
    head = re.sub(r"^owned\s+", "", m["ret"])
    head = re.sub(r"^(?:Task|OwningTask)<(.*)>$", r"\1", head)
    base = re.sub(r"<.*", "", head).rstrip("?")
    if base == "Result" or base in choices:
        return "choice"
    if m["ret"].endswith("?"):
        return "nullable"
    return "plain"

for m in members:
    m["tier"] = tier(m)
    text = m["decl"] + " " + m["comment"]
    m["names_exception"] = sorted(set(EXC.findall(text)))
    m["documented_throwing"] = bool(m["names_exception"]) or bool(FAULTWORD.search(m["comment"]))

os.makedirs(out, exist_ok=True)
with open(os.path.join(out, "stdlib_members.tsv"), "w", encoding="utf-8") as f:
    f.write("line\tsection\ttype\tkind\tname\ttier\tdocumented_throwing\texceptions\tret\n")
    for m in members:
        f.write("\t".join([str(m["line"]), m["section"], m["type"], m["kind"], m["name"],
                           m["tier"], "1" if m["documented_throwing"] else "0",
                           ",".join(m["names_exception"]), m["ret"]]) + "\n")

tiers = collections.Counter(m["tier"] for m in members)
callable_members = [m for m in members if m["kind"] in ("method", "constructor", "indexer", "operator")]
summary = {
    "document": "docs/Bezel_Standard_Library.md",
    "member_declarations": len(members),
    "by_kind": dict(collections.Counter(m["kind"] for m in members)),
    "by_tier": dict(tiers),
    "documented_throwing_members_lower_bound": sum(m["documented_throwing"] for m in members),
    "distinct_try_names": len({m["name"] for m in members if m["tier"] == "try"}),
    "distinct_ornull_names": len({m["name"] for m in members if m["tier"] == "ornull"}),
    "choice_types_declared": sorted(choices),
    "choice_returning_members": [f'{m["type"]}.{m["name"]}: {m["ret"]}' for m in members if m["tier"] == "choice"],
}

# exception vocabulary in prose
allexc = collections.Counter()
for sec, c in exc_by_section.items():
    allexc.update(c)
summary["distinct_exception_type_names_in_prose"] = len(allexc)
summary["exception_mentions_in_prose"] = sum(allexc.values())
with open(os.path.join(out, "stdlib_exception_mentions.tsv"), "w", encoding="utf-8") as f:
    f.write("exception\tprose_mentions\tsections\n")
    for e, n in allexc.most_common():
        secs = sorted(s for s, c in exc_by_section.items() if e in c)
        f.write(f"{e}\t{n}\t{len(secs)}\n")

# tier families: one operation spelled in more than one tier on the same type
def stem(name):
    s = re.sub(r"^Try", "", name)
    s = s.replace("OrNull", "")
    return s

fam = collections.defaultdict(lambda: collections.defaultdict(set))
for m in members:
    if m["kind"] not in ("method",):
        continue
    key = (m["type"], stem(m["name"]))
    t = m["tier"] if m["tier"] in ("try", "ornull") else "base"
    fam[key][t].add(m["name"])
rows = []
for (ty, st), d in sorted(fam.items()):
    if len(d) >= 2 and ("try" in d or "ornull" in d):
        rows.append((ty, st, ",".join(sorted(d.get("base", []))), ",".join(sorted(d.get("try", []))),
                     ",".join(sorted(d.get("ornull", [])))))
with open(os.path.join(out, "stdlib_tier_families.tsv"), "w", encoding="utf-8") as f:
    f.write("type\tstem\tbase_form\ttry_form\tornull_form\n")
    for r in rows:
        f.write("\t".join(r) + "\n")
summary["tier_families_same_type"] = len(rows)
summary["tier_families_three_way"] = sum(1 for r in rows if r[2] and r[3] and r[4])
summary["tier_families_base_plus_try"] = sum(1 for r in rows if r[2] and r[3] and not r[4])
summary["tier_families_base_plus_ornull"] = sum(1 for r in rows if r[2] and r[4] and not r[3])
summary["tier_families_try_plus_ornull_only"] = sum(1 for r in rows if not r[2] and r[3] and r[4])
json.dump(summary, open(os.path.join(out, "stdlib_summary.json"), "w"), indent=2)
print(json.dumps(summary, indent=2))
