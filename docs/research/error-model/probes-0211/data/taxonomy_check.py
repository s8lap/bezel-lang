#!/usr/bin/env python3
"""Check taxonomy.tsv against the standard-library document, and summarize it.

1. Extract every (section, exception type) pair the standard library names, in prose
   or in a fenced block (member names that merely end in `Exception` are dropped).
2. Fail if a pair has no row in taxonomy.tsv, or a row names a pair the document
   does not contain.
3. Print the counts the proposal quotes, and (with --markdown) the table of §4.3.

taxonomy.tsv columns:
  section     standard-library section number
  condition   the condition, in the section's own terms
  type_today  the exception type the section names
  site        raise (the section states the member raises it), decl (it declares the
              type), xref (it only mentions the type)
  kind        the kind of the CONDITION: failure, defect, cancellation, carrier, fatal
  action      none        the type's kind already matches
              marker      the type implements the marker (directly or by inheritance)
              range-class the condition is raised with the new derived class
              join-class  the join raises the new derived class when the original
                          exception was a failure
              misfit      a defect condition raised with a failure type: recorded, not moved
              relabel     the section's band label is reworded; the type is unchanged

Usage: taxonomy_check.py <stdlib.md> <taxonomy.tsv> [--markdown]
"""
import collections, csv, re, sys

doc, tsv = sys.argv[1], sys.argv[2]
NOT_TYPES = {"SetException", "TrySetException", "FromException", "InnerException"}
lines = open(doc, encoding="utf-8").read().split("\n")
sec = "0"; fence = False; pairs = collections.Counter()
for l in lines:
    m = re.match(r"^(#{2,4}) (\d+(?:\.\d+)*)\.? ", l)
    if m and not fence:
        sec = m.group(2); continue
    if l.startswith("```"): fence = not fence
    for t in re.findall(r"\b([A-Z][A-Za-z]*Exception)\b", l):
        if t not in NOT_TYPES: pairs[(sec, t)] += 1

rows = list(csv.DictReader(open(tsv, encoding="utf-8"), delimiter="\t"))
covered = {(r["section"], r["type_today"]) for r in rows}
missing = sorted(set(pairs) - covered); extra = sorted(covered - set(pairs))
if missing or extra:
    for p in missing: print("NO ROW FOR", p)
    for p in extra: print("ROW WITHOUT A MENTION", p)
    sys.exit(1)

def vkey(s): return [int(x) for x in s.split(".")]
if "--markdown" in sys.argv:
    print("| Stdlib § | Condition | Type today | Kind | Action |")
    print("|---|---|---|---|---|")
    for r in sorted((r for r in rows if r["site"] == "raise"), key=lambda r: vkey(r["section"])):
        print(f"| {r['section']} | {r['condition']} | `{r['type_today']}` | {r['kind']} | {r['action']} |")
    sys.exit(0)

print(f"mentions: {sum(pairs.values())}; (section, type) pairs: {len(pairs)}; sections: {len({s for s, _ in pairs})}; types: {len({t for _, t in pairs})}")
print(f"rows: {len(rows)}; all pairs covered")
site = collections.Counter(r["site"] for r in rows)
print("rows by site:", dict(site))
raise_rows = [r for r in rows if r["site"] == "raise"]
print("raise rows by kind:", dict(collections.Counter(r["kind"] for r in raise_rows)))
print("raise rows by action:", dict(collections.Counter(r["action"] for r in raise_rows)))
pk = collections.defaultdict(set)
for r in raise_rows: pk[(r["section"], r["type_today"])].add(r["kind"])
print("raise (section, type) pairs:", len(pk), "by kind:", dict(collections.Counter("+".join(sorted(v)) for v in pk.values())))
bt = collections.defaultdict(set)
for r in raise_rows: bt[r["type_today"]].add(r["kind"])
print("types raised for conditions of two kinds:", sorted(t for t, v in bt.items() if len(v) > 1))
for a in ("range-class", "join-class", "misfit", "relabel"):
    print(f"{a}:", sorted({r["section"] for r in raise_rows if r["action"] == a}, key=vkey))
note = collections.Counter()
for (s, t), n in pairs.items():
    if t in ("ArgumentException", "OverflowException", "InvalidOperationException"): note[t] += 1
print("the note's three straddling types, (section, type) pairs:", dict(note), "total", sum(note.values()))
