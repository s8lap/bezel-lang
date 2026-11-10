#!/usr/bin/env python3
"""Count failure-signal usage in the Bezel program corpora.

Corpora (each scanned as Bezel source text with comments and string-literal
contents blanked, so `try` in a comment or a string never counts):

  rules   tests/conformance/rules/cases/**/*.bzl      rule-conformance cases
  docex   <export>/tests/conformance/cases/*.bzl      doc examples, extracted by
          scripts/extract_doc_examples.py on an exported copy (non-skip rows)
  lit     tests/lit/**/*.bzl                          lit/FileCheck diagnostics
  gtest   string literals of tests/**/*.cpp           gtest-embedded programs
          (C++ string literals are concatenated per file; a "program" is one
          `module X;` header inside the literal text)

Usage: corpus_usage.py <repo-root> <export-root> <out-dir>
"""
import collections
import csv
import glob
import json
import os
import re
import sys

root, export, out = sys.argv[1:4]

# ---------------------------------------------------------------- text prep
def blank_bezel(src):
    """Blank // and /* */ comments and the contents of string/char literals."""
    o = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            i = j
            continue
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            o.append(" ")
            i = j
            continue
        if c == '"':
            j = i + 1
            while j < n and src[j] != '"':
                if src[j] == "\\":
                    j += 1
                j += 1
            o.append('""')
            i = j + 1
            continue
        if c == "'" and i + 2 < n:
            j = i + 1
            if src[j] == "\\":
                j += 1
            j += 1
            if j < n and src[j] == "'":
                o.append("''")
                i = j + 1
                continue
        o.append(c)
        i += 1
    return "".join(o)

CPP_STR = re.compile(r'R"([A-Za-z_]*)\((.*?)\)\1"|"((?:[^"\\\n]|\\.)*)"', re.S)
def cpp_literals(src):
    src = re.sub(r"^\s*//.*$", "", src, flags=re.M)
    parts = []
    for m in CPP_STR.finditer(src):
        if m.group(2) is not None:
            parts.append(m.group(2))
        else:
            s = m.group(3)
            s = s.replace('\\n', '\n').replace('\\"', '"').replace("\\\\", "\\").replace("\\t", " ")
            parts.append(s)
    return "\n".join(parts)

# ---------------------------------------------------------------- patterns
DEFECT = {
    "ContractViolationException", "ArgumentException", "ArgumentNullException",
    "ArgumentOutOfRangeException", "ArithmeticException", "OverflowException",
    "DivideByZeroException", "InvalidOperationException", "DisposedAccessException",
    "NotImplementedException", "NullReferenceException", "IndexOutOfRangeException",
    "MatchFailureException", "KeyNotFoundException", "TypeInitializationException",
}
RECOVERABLE = {
    "IOException", "FileNotFoundException", "DirectoryNotFoundException",
    "EndOfStreamException", "TimeoutException", "OperationCanceledException",
    "FormatException", "SerializationException", "ChannelClosedException",
    "ChannelFullException", "DomainFaultedException", "JsonParseException",
    "UnauthorizedAccessException", "PathTooLongException",
}

P = {
    "throw_new":      re.compile(r"\bthrow\s+new\s+([A-Za-z_][\w.]*)"),
    "throw_other":    re.compile(r"\bthrow\s+(?!new\b)[A-Za-z_(]"),
    "throw_bare":     re.compile(r"\bthrow\s*(?:;|,|\)|\bend\b|$)", re.M),
    "try_stmt":       re.compile(r"\btry\b"),          # corrected below: minus `end try` closers
    "end_try":        re.compile(r"\bend\s+try\b"),
    "catch_block":    re.compile(r"\bcatch\b"),
    "finally":        re.compile(r"\bfinally\b"),
    "try_call":       re.compile(r"\bTry[A-Z]\w*\s*(?:<[^<>()]*>)?\s*\("),
    "ornull_call":    re.compile(r"\b\w*OrNull\s*(?:<[^<>()]*>)?\s*\("),
    "result_type":    re.compile(r"\bResult\s*<"),
    "ok_error_ctor":  re.compile(r"\b(?:Ok|Error)\s*\("),
    "coalesce_throw": re.compile(r"\?\?\s*throw\b"),
    "requires":       re.compile(r"\brequires\s*\("),
    "ensures":        re.compile(r"\bensures\s*\("),
    "invariant":      re.compile(r"\binvariant\s*\("),
    "using_var":      re.compile(r"\busing\s+var\b"),
    "await":          re.compile(r"\bawait\b"),
    "suppress_all":   re.compile(r"\bException\s*=>\s*\(\s*\)"),
    "choice_decl":    re.compile(r"\bchoice\s+[A-Z]\w*"),
    "exception_decl": re.compile(r"\bclass\s+\w+\s*(?:<[^>]*>)?\s*:\s*\w*Exception\b"),
    "method_decl":    re.compile(r"\b(?:method|constructor)\b"),
    "null_forgive":   re.compile(r"[\w\)\]]!(?=[\s.;,\)\]])"),
}
TYPE_RX = r"[A-Z][\w.]*"
ARM = re.compile(r"(?:\bcatch\b|,)\s*(?:var\s+\w+\s*:\s*)?(" + TYPE_RX + r"(?:\s*\|\s*" + TYPE_RX +
                 r")*)\s*(?:when\b[^=]*?(?:=[^>][^=]*?)*)?=>")

def catch_arm_types(text):
    """Types named by catch arms.

    An arm is `[var x:] Type [| Type ...] [when guard] =>` directly after the
    `catch` keyword or after a `,`, inside the text between `catch` and the
    next `finally` / `try` (or 1500 characters).  A `case` arm nested in a
    catch arm body that matches a bare capitalised name after a comma is
    miscounted as a catch arm; such names land in the "other_or_user" band.
    """
    res = []
    for m in re.finditer(r"\bcatch\b", text):
        seg = text[m.start(): m.end() + 1500]
        stop = re.search(r"\bfinally\b|\btry\b", seg[5:])
        if stop:
            seg = seg[:5 + stop.start()]
        for a in ARM.finditer(seg):
            for t in re.split(r"\s*\|\s*", a.group(1)):
                res.append(t.split(".")[-1])
    return res

def scan(text, counts, files_with, throw_types, arm_types):
    present = set()
    for k, rx in P.items():
        ms = rx.findall(text)
        if ms:
            counts[k] += len(ms)
            present.add(k)
        if k == "throw_new":
            for t in ms:
                throw_types[t.split(".")[-1]] += 1
    if "end_try" in present:
        closers = len(P["end_try"].findall(text))
        counts["try_stmt"] -= closers
        counts["end_try"] -= closers
    arms = catch_arm_types(text)
    for t in arms:
        arm_types[t] += 1
    for k in present:
        files_with[k] += 1
    any_exc = present & {"throw_new", "throw_other", "throw_bare", "try_stmt", "catch_block", "finally"}
    any_val = present & {"try_call", "ornull_call"}
    any_res = present & {"result_type", "ok_error_ctor"}
    if any_exc:
        files_with["_any_exception_syntax"] += 1
    if any_val:
        files_with["_any_try_or_ornull_call"] += 1
    if any_res:
        files_with["_any_result"] += 1
    if any_exc or any_val or any_res:
        files_with["_any_failure_signal"] += 1
    tiers = sum(bool(x) for x in (any_exc, any_val, any_res))
    if tiers >= 2:
        files_with["_two_or_more_mechanisms"] += 1
    if tiers == 3:
        files_with["_all_three_mechanisms"] += 1

# ---------------------------------------------------------------- corpora
def bzl_files(pattern):
    return sorted(glob.glob(pattern, recursive=True))

corpora = {}
corpora["rules"] = [("bzl", p) for p in bzl_files(os.path.join(root, "tests/conformance/rules/cases/**/*.bzl"))]
skip = set()
man = os.path.join(export, "tests/conformance/manifest.tsv")
if os.path.exists(man):
    for row in csv.reader(open(man, encoding="utf-8"), delimiter="\t"):
        if len(row) > 2 and any(c.startswith("skip") for c in row[1:4]):
            skip.add(row[0])
docs = []
for p in bzl_files(os.path.join(export, "tests/conformance/cases/*.bzl")):
    cid = os.path.splitext(os.path.basename(p))[0]
    if cid not in skip:
        docs.append(("bzl", p))
corpora["docex"] = docs
corpora["lit"] = [("bzl", p) for p in bzl_files(os.path.join(root, "tests/lit/**/*.bzl"))]
cpps = [p for p in bzl_files(os.path.join(root, "tests/**/*.cpp"))
        if "/conformance/" not in p and "/lit/" not in p]
corpora["gtest"] = [("cpp", p) for p in cpps]

result = {}
allthrow = collections.Counter()
allarms = collections.Counter()
for name, files in corpora.items():
    counts = collections.Counter()
    files_with = collections.Counter()
    throw_types = collections.Counter()
    arm_types = collections.Counter()
    programs = 0
    for kind, p in files:
        try:
            src = open(p, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        if kind == "cpp":
            src = cpp_literals(src)
            if "module " not in src:
                continue
        text = blank_bezel(src)
        programs += max(1, len(re.findall(r"\bmodule\s+[A-Z][\w.]*\s*;", text))) if kind == "cpp" else 1
        scan(text, counts, files_with, throw_types, arm_types)
    allthrow.update(throw_types)
    allarms.update(arm_types)
    def band(counter):
        d = sum(n for t, n in counter.items() if t in DEFECT)
        r = sum(n for t, n in counter.items() if t in RECOVERABLE)
        b = counter.get("Exception", 0)
        o = sum(counter.values()) - d - r - b
        return {"defect_band": d, "recoverable_band": r, "base_Exception": b, "other_or_user": o}
    result[name] = {
        "files": len(files),
        "programs": programs,
        "site_counts": dict(counts),
        "files_with": dict(files_with),
        "throw_new_by_band": band(throw_types),
        "catch_arm_by_band": band(arm_types),
        "throw_new_top": throw_types.most_common(25),
        "catch_arm_top": arm_types.most_common(25),
    }

os.makedirs(out, exist_ok=True)
json.dump(result, open(os.path.join(out, "corpus_usage.json"), "w"), indent=1)
with open(os.path.join(out, "corpus_usage.tsv"), "w") as f:
    keys = sorted({k for r in result.values() for k in r["site_counts"]})
    f.write("metric\t" + "\t".join(result) + "\ttotal\n")
    f.write("files\t" + "\t".join(str(result[c]["files"]) for c in result) + f"\t{sum(r['files'] for r in result.values())}\n")
    f.write("programs\t" + "\t".join(str(result[c]["programs"]) for c in result) + f"\t{sum(r['programs'] for r in result.values())}\n")
    for k in keys:
        vals = [result[c]["site_counts"].get(k, 0) for c in result]
        f.write(f"sites:{k}\t" + "\t".join(map(str, vals)) + f"\t{sum(vals)}\n")
    fkeys = sorted({k for r in result.values() for k in r["files_with"]})
    for k in fkeys:
        vals = [result[c]["files_with"].get(k, 0) for c in result]
        f.write(f"files_with:{k}\t" + "\t".join(map(str, vals)) + f"\t{sum(vals)}\n")
    for b in ("defect_band", "recoverable_band", "base_Exception", "other_or_user"):
        vals = [result[c]["throw_new_by_band"][b] for c in result]
        f.write(f"throw_new:{b}\t" + "\t".join(map(str, vals)) + f"\t{sum(vals)}\n")
    for b in ("defect_band", "recoverable_band", "base_Exception", "other_or_user"):
        vals = [result[c]["catch_arm_by_band"][b] for c in result]
        f.write(f"catch_arm:{b}\t" + "\t".join(map(str, vals)) + f"\t{sum(vals)}\n")
with open(os.path.join(out, "corpus_throw_types.tsv"), "w") as f:
    f.write("exception\tthrow_new_sites\tcatch_arms\tband\n")
    for t in sorted(set(allthrow) | set(allarms), key=lambda t: -(allthrow[t] + allarms[t])):
        b = "defect" if t in DEFECT else "recoverable" if t in RECOVERABLE else "base" if t == "Exception" else "other/user"
        f.write(f"{t}\t{allthrow[t]}\t{allarms[t]}\t{b}\n")
print(open(os.path.join(out, "corpus_usage.tsv")).read())
