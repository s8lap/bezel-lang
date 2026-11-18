#!/usr/bin/env python3
"""Catch arms of the four Bezel program corpora, by the type they name and by what the arm
body does, with each type's v0.357 kind.

Input: catch_arms.tsv, written by proposal 0211's census
(docs/research/error-model/probes-0211/data/catch_arms.py <repo> <export> <out-dir>; the
file is about 400 KB and is not committed). Body classes are that script's:
  handles   handling completes in the arm      convert   throws a different exception
  reraise   contains a bare `throw`            suppress  the body is `()`

An arm naming several types (`A | B`) counts once for each.

Usage: corpus_arms.py <catch_arms.tsv> <out-dir>
Writes corpus_arms_by_type.tsv and corpus_arms_summary.json.
"""
import collections, csv, json, os, sys

src, out = sys.argv[1:3]
FAILURE = {"FormatException", "IOException", "FileNotFoundException", "DirectoryNotFoundException",
           "EndOfStreamException", "PathTooLongException", "SerializationException", "TimeoutException",
           "ChannelClosedException", "ChannelFullException", "ParseRangeException", "DomainFailedException"}
DEFECT = {"ContractViolationException", "ArgumentException", "ArgumentNullException", "ArithmeticException",
          "OverflowException", "DivideByZeroException", "InvalidOperationException", "DisposedAccessException",
          "NotImplementedException", "NullReferenceException", "IndexOutOfRangeException", "MatchFailureException",
          "TypeInitializationException", "ArgumentOutOfRangeException", "KeyNotFoundException", "DomainFaultedException"}
def kind(t):
    if t == "Exception": return "catch-all"
    if t == "IFailure": return "marker"
    if t == "OperationCanceledException": return "cancellation"
    if t in FAILURE: return "failure"
    if t in DEFECT: return "defect"
    if t == "?": return "unparsed"
    return "user class or interface"

CORPORA = ["rules", "docex", "lit", "gtest"]
BODIES = ["handles", "convert", "reraise", "suppress"]
by_type = collections.defaultdict(collections.Counter)
by_kind = collections.defaultdict(collections.Counter)
arms = 0
for r in csv.DictReader(open(src, encoding="utf-8"), delimiter="\t"):
    arms += 1
    for t in r["types"].split("|"):
        k = kind(t)
        if k == "unparsed": continue
        by_type[t][r["corpus"]] += 1; by_type[t][r["body_class"]] += 1; by_type[t]["n"] += 1
        by_kind[k][r["corpus"]] += 1; by_kind[k][r["body_class"]] += 1; by_kind[k]["n"] += 1

os.makedirs(out, exist_ok=True)
with open(os.path.join(out, "corpus_arms_by_type.tsv"), "w", encoding="utf-8") as f:
    f.write("type\tkind_v0357\tarms\t" + "\t".join(CORPORA) + "\t" + "\t".join(BODIES) + "\n")
    named = [t for t in by_type if kind(t) != "user class or interface"]
    for t in sorted(named, key=lambda t: (-by_type[t]["n"], t)):
        c = by_type[t]
        f.write("\t".join([t, kind(t), str(c["n"])] + [str(c[x]) for x in CORPORA + BODIES]) + "\n")
    c = by_kind["user class or interface"]
    f.write("\t".join(["(all user classes and interfaces)", "user", str(c["n"])] + [str(c[x]) for x in CORPORA + BODIES]) + "\n")

S = collections.OrderedDict()
S["arms"] = arms
S["type_mentions_by_kind"] = {k: by_kind[k]["n"] for k in ["defect", "failure", "cancellation", "catch-all", "marker", "user class or interface"]}
S["body_class_by_kind"] = {k: {b: by_kind[k][b] for b in BODIES if by_kind[k][b]} for k in S["type_mentions_by_kind"]}
S["by_corpus_and_kind"] = {c: {k: by_kind[k][c] for k in S["type_mentions_by_kind"] if by_kind[k][c]} for c in CORPORA}
d, fl = by_kind["defect"], by_kind["failure"]
S["share_of_named_stdlib_type_mentions_that_are_defect_types"] = round(d["n"] / (d["n"] + fl["n"]), 3)
S["defect_type_arms_that_complete_handling"] = d["handles"]
S["defect_type_arms_that_convert_or_reraise"] = d["convert"] + d["reraise"]
S["docex_defect_type_arms_that_complete_handling"] = sum(
    1 for r in csv.DictReader(open(src, encoding="utf-8"), delimiter="\t")
    if r["corpus"] == "docex" and r["body_class"] == "handles" and any(kind(t) == "defect" for t in r["types"].split("|")))
json.dump(S, open(os.path.join(out, "corpus_arms_summary.json"), "w"), indent=1)
print(json.dumps(S, indent=1))
