#!/usr/bin/env python3
"""Measure how much documentation and decision history the failure-signal tiers cost.

Usage: doc_cost.py <repo-root> <out-dir>
"""
import collections
import json
import os
import re
import sys

root, out = sys.argv[1], sys.argv[2]
def rd(p):
    return open(os.path.join(root, p), encoding="utf-8").read()
def words(s):
    return len(re.findall(r"\S+", s))

res = {}

# 1. Teaching text
g2 = rd("docs/Bezel_Guide_02_Errors_and_Nullability.md")
secs = re.split(r"(?m)^## ", g2)
res["guide2_words_total"] = words(g2)
res["guide2_sections"] = {s.split("\n", 1)[0][:70]: words(s) for s in secs[1:]}
m = re.search(r"\| Situation \| Use \|.*?\n\n", g2, re.S)
rows = [l for l in m.group(0).split("\n") if l.startswith("|")][2:]
res["guide2_choosing_table_rows"] = len(rows)

std = rd("docs/Bezel_Standard_Library.md")
i = std.index("**The Try convention")
j = std.index("**Statics-and-sharing facts")
res["stdlib_s1_try_and_failure_rule_words"] = words(std[i:j])
cg = rd("docs/Bezel_For_Code_Generation.md")
i = cg.index("### 5.1 The Try shapes")
j = cg.index("### 5.2 ")
res["codegen_s5_1_words"] = words(cg[i:j])
rat = rd("docs/Bezel_Design_Rationale.md")
i = rat.index("### 11.4 One rule for failure signals")
j = rat.index("### 11.5 ")
res["rationale_s11_4_words"] = words(rat[i:j])
spec = rd("docs/Bezel_Language_Spec.md")
i = spec.index("## 31. Exception Handling")
j = spec.index("## 32. Foreign Function Interface")
res["spec_s31_words"] = words(spec[i:j])

# 2. Mentions of the tiers across the normative documents
for name, text in (("spec", spec), ("stdlib", std), ("rationale", rat), ("codegen", cg), ("guide2", g2)):
    res[f"mentions_{name}"] = {
        "TryX_names": len(re.findall(r"\bTry[A-Z]\w*", text)),
        "OrNull_names": len(re.findall(r"\b\w*OrNull\b", text)),
        "Result<": len(re.findall(r"\bResult<", text)),
        "Exception_type_names": len(re.findall(r"\b[A-Z]\w*Exception\b", text)),
        "failure-signal rule": len(re.findall(r"failure-signal rule", text)),
    }

# 3. Decision history: decline-index rows and changelog revisions about the tiers
dec = rd("docs/proposals/DECLINED.md")
rows = [l for l in dec.split("\n") if l.startswith("| ") and not l.startswith("| Class") and not l.startswith("|---")]
def count(rx):
    return sum(1 for l in rows if re.search(rx, l.split("|")[1]))
res["declined_index_rows_total"] = len(rows)
res["declined_rows_subject_names_Try_or_OrNull"] = count(r"\bTry[A-Z]\w*|OrNull")
res["declined_rows_subject_names_Exception"] = count(r"Exception|exception|`throw`|`catch`|`try`|`finally`")
res["declined_rows_subject_names_Result"] = count(r"\bResult\b")
ch = rd("docs/Bezel_Changelog.md")
revs = re.split(r"(?m)^#{2,3} (?=v?0\.\d+)", ch)
def revcount(rx):
    return sum(1 for r in revs[1:] if re.search(rx, r))
res["changelog_revisions"] = len(revs) - 1
res["changelog_revisions_touching_Try_shape_or_OrNull"] = revcount(r"\bTry[A-Z]\w*|OrNull")
res["changelog_revisions_touching_exception_text"] = revcount(r"\b[A-Z]\w*Exception\b")

# 4. Proposals whose title is about a failure-signal tier
props = sorted(os.listdir(os.path.join(root, "docs/proposals")))
hits = []
for p in props:
    if not re.match(r"\d{4}-", p):
        continue
    head = rd("docs/proposals/" + p).split("\n", 1)[0]
    body = rd("docs/proposals/" + p)
    n = len(re.findall(r"\bTry[A-Z]\w*|\w+OrNull\b|failure-signal", body))
    if n >= 10:
        hits.append((p, n))
res["proposals_total"] = len([p for p in props if re.match(r"\d{4}-", p)])
res["proposals_with_10_or_more_tier_mentions"] = len(hits)
res["proposals_with_10_or_more_tier_mentions_list"] = hits

json.dump(res, open(os.path.join(out, "doc_cost.json"), "w"), indent=1)
print(json.dumps(res, indent=1))
