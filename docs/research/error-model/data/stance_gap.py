#!/usr/bin/env python3
"""Which stdlib sections document an expected (non-bug) failure, and which of
them offer the caller a value-shaped stance (a Try or *OrNull member)?

A section "documents an expected failure" when its text names one of the
exception types the failure-signal rule's bands 2-4 use (input, environment,
lifecycle), listed in EXPECTED below.  This is a text measure over
docs/Bezel_Standard_Library.md; it does not parse fault contracts.

Usage: stance_gap.py <repo-root> <out-dir>
"""
import collections, json, os, re, sys
root, out = sys.argv[1], sys.argv[2]
lines = open(os.path.join(root, "docs/Bezel_Standard_Library.md"), encoding="utf-8").read().split("\n")
EXPECTED = {
    "FormatException", "IOException", "FileNotFoundException", "DirectoryNotFoundException",
    "EndOfStreamException", "TimeoutException", "SerializationException",
    "ChannelClosedException", "ChannelFullException", "DomainFaultedException",
    "KeyNotFoundException", "OperationCanceledException", "UnauthorizedAccessException",
    "InvalidTimeZoneException", "JsonParseException",
}
SEC = re.compile(r"^### (\d+\.\d+) (.*)")
cur = None
secs = collections.OrderedDict()
for l in lines:
    m = SEC.match(l)
    if m:
        cur = m.group(1)
        secs[cur] = dict(title=re.sub(r"[`*]", "", m.group(2))[:60], exc=collections.Counter(), tr=set(), on=set())
        continue
    if cur is None:
        continue
    s = secs[cur]
    for e in re.findall(r"\b([A-Z]\w*Exception)\b", l):
        s["exc"][e] += 1
    for t in re.findall(r"\bmethod (Try[A-Z]\w*)", l):
        s["tr"].add(t)
    for t in re.findall(r"\bmethod (\w*OrNull)\b", l):
        s["on"].add(t)
rows = []
for k, s in secs.items():
    exp = sorted(set(s["exc"]) & EXPECTED)
    if not exp:
        continue
    rows.append((k, s["title"], ",".join(exp), len(s["tr"]), len(s["on"])))
with open(os.path.join(out, "stdlib_stance_gap.tsv"), "w") as f:
    f.write("section\ttitle\texpected_failure_types_named\ttry_members\tornull_members\n")
    for r in rows:
        f.write("\t".join(map(str, r)) + "\n")
summ = dict(
    sections_total=len(secs),
    sections_naming_an_expected_failure=len(rows),
    of_which_with_try_member=sum(1 for r in rows if r[3]),
    of_which_with_ornull_member=sum(1 for r in rows if r[4]),
    of_which_with_both=sum(1 for r in rows if r[3] and r[4]),
    of_which_with_neither=sum(1 for r in rows if not r[3] and not r[4]),
    neither_list=[f"{r[0]} {r[1]}" for r in rows if not r[3] and not r[4]],
)
json.dump(summ, open(os.path.join(out, "stdlib_stance_gap.json"), "w"), indent=1)
print(json.dumps(summ, indent=1))
