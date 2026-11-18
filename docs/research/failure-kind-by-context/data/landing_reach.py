#!/usr/bin/env python3
"""What v0.356's landing put into the tree, counted, to price the options of README section 6.

  - lines of each normative document that name `IFailure`, `ParseRangeException` or
    `DomainFailedException`;
  - rule-corpus cases that name `IFailure`, split by whether the case relies on a library
    class being a failure (it would change if the library stopped classifying) or only on
    classes the case declares itself;
  - mentions of the marker in the compiler sources.

Usage: landing_reach.py <repo-root> [out.json]
"""
import collections, glob, json, os, re, subprocess, sys

root = sys.argv[1]
DOCS = ["Bezel_Language_Spec.md", "Bezel_Standard_Library.md", "Bezel_Design_Rationale.md",
        "Bezel_Guide_02_Errors_and_Nullability.md", "Bezel_Guide_04_Concurrency.md",
        "Bezel_Guide_06_Contracts_and_Purity.md", "Bezel_For_Code_Generation.md", "Bezel_Tour.md"]
NAMES = ["IFailure", "ParseRangeException", "DomainFailedException"]
LIB_FAILURE = ["FormatException", "IOException", "FileNotFoundException", "DirectoryNotFoundException",
               "EndOfStreamException", "PathTooLongException", "SerializationException", "TimeoutException",
               "ChannelClosedException", "ChannelFullException", "ParseRangeException", "DomainFailedException"]
R = collections.OrderedDict()
R["document_lines_naming"] = {}
for dname in DOCS:
    text = open(os.path.join(root, "docs", dname), encoding="utf-8").read().split("\n")
    R["document_lines_naming"][dname] = {n: sum(1 for ln in text if n in ln) for n in NAMES}
R["document_lines_naming_total"] = {n: sum(v[n] for v in R["document_lines_naming"].values()) for n in NAMES}

cases = sorted(glob.glob(os.path.join(root, "tests/conformance/rules/cases/**/*.bzl"), recursive=True))
decl = re.compile(r"\b(?:class|interface)\s+\w+[^;\n]*\bIFailure\b")
lib = re.compile(r"\b(?:" + "|".join(LIB_FAILURE) + r")\b|\.Parse\(|Convert\.To\w+\(")
c = collections.Counter()
for p in cases:
    s = open(p, encoding="utf-8").read()
    if "IFailure" not in s: continue
    c["cases_naming_IFailure"] += 1
    relies = bool(lib.search(s)); own = bool(decl.search(s))
    c["relies_on_a_library_class_being_a_failure" if relies else
      ("uses_only_classes_the_case_declares" if own else "names_the_marker_without_either")] += 1
R["rule_corpus_cases"] = len(cases)
R["rule_corpus"] = dict(c)
R["rule_corpus_cases_naming"] = {n: sum(1 for p in cases if n in open(p, encoding="utf-8").read()) for n in NAMES[1:]}
try:
    out = subprocess.run(["git", "-C", root, "grep", "-l", "IFailure", "--", "src"], capture_output=True, text=True).stdout
    R["compiler_source_files_naming_IFailure"] = len([x for x in out.split("\n") if x])
except OSError:
    R["compiler_source_files_naming_IFailure"] = None
print(json.dumps(R, indent=1))
if len(sys.argv) > 2: json.dump(R, open(sys.argv[2], "w"), indent=1)
