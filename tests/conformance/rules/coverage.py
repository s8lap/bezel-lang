#!/usr/bin/env python3
"""The coverage matrix: what the documents say, against what the corpus covers.

    coverage.py [--docs DIR] [--out PATH] [--summary] [--gaps] [--top N]

Without a matrix, "go over every section" produces cases in the sections a
writer finds easiest and silently skips the hard ones. This enumerates the unit
of coverage — every numbered subsection of the spec and the stdlib — annotates
each with a *rule-density* estimate (how many times the section states a
compiler-enforced consequence), and joins that against the corpus.

Rule density is a proxy, not a count of rules: it tallies the phrases the
documents use when they state a consequence ("is a compile error", "is
rejected", "must", "may not", …). It sizes a section's work; it does not define
it. The authoritative rule index is Appendix C, reported separately.
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus  # noqa: E402

HEADING_RE = re.compile(r'^(#{2,4})\s+(?:(\d+(?:\.\d+)*)\.?\s+)?(.*)$')
SECTION_REF_RE = re.compile(r'§(\d+(?:\.\d+)*)')

# The phrases the documents use to state a compiler-enforced consequence.
DENSITY_RE = re.compile(
    r'\b(?:is a compile error|are a compile error|compile error|is rejected|'
    r'are rejected|is forbidden|may not|must not|must be|must have|must match|'
    r'is not admitted|is not permitted|cannot be|raises)\b', re.IGNORECASE)

COLUMNS = ("doc", "section", "title", "density", "appendix-c", "cases",
           "accept", "reject", "run", "state")


def parse_sections(path, doc_tag):
    """Every numbered heading in a document, with its body text."""
    with open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")
    sections = []
    current = None
    for line in lines:
        m = HEADING_RE.match(line)
        if m and m.group(2):
            current = {"doc": doc_tag, "section": m.group(2),
                       "title": m.group(3).strip().replace("`", ""), "body": []}
            sections.append(current)
        elif m and not m.group(2):
            current = None  # an unnumbered heading (Appendix, Contents) ends the run
        elif current is not None:
            current["body"].append(line)
    for s in sections:
        text = "\n".join(s["body"])
        s["density"] = len(DENSITY_RE.findall(text))
        del s["body"]
    return sections


def parse_appendix_c(path):
    """Appendix C's invariant bullets, each with the sections it cites."""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    start = text.find("## Appendix C")
    if start < 0:
        return []
    end = text.find("\n## ", start + 10)
    body = text[start:end if end > 0 else len(text)]
    rows = []
    group = ""
    for line in body.split("\n"):
        m = re.match(r'^### (C\.\d+)\s+(.*)$', line)
        if m:
            group = m.group(1)
            continue
        if line.startswith("- "):
            refs = sorted(set(SECTION_REF_RE.findall(line)))
            rows.append({"group": group, "text": line[2:].strip(), "refs": refs})
    return rows


def parse_sentences(path, prefix):
    """The rule-stating sentences of a section and its subsections, in order.

    A sentence qualifies when it uses one of the phrases the documents reach for
    to state a compiler-enforced consequence. It is a checklist, not a rule
    count: one sentence can carry several rules and a few carry none."""
    with open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")
    out = []
    current = None
    for line in lines:
        m = HEADING_RE.match(line)
        if m and m.group(2):
            num = m.group(2)
            if num == prefix or num.startswith(prefix + "."):
                current = {"section": num,
                           "title": m.group(3).strip().replace("`", ""),
                           "body": []}
                out.append(current)
            else:
                current = None
            continue
        if m:
            current = None
            continue
        if current is not None:
            current["body"].append(line)
    for sec in out:
        text = "\n".join(sec["body"])
        text = re.sub(r'```.*?```', ' [code] ', text, flags=re.DOTALL)
        text = re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', text)   # strip md links
        text = re.sub(r'\s+', ' ', text)
        sentences = re.split(r'(?<=[.:;])\s+(?=[A-Z`*(\u00a7])', text)
        sec["rules"] = [s.strip() for s in sentences
                        if DENSITY_RE.search(s) and len(s.strip()) > 30]
        del sec["body"]
    return [s for s in out if s["rules"]]


def build(docs_dir, base_dir):
    spec = os.path.join(docs_dir, "Bezel_Language_Spec.md")
    stdlib = os.path.join(docs_dir, "Bezel_Standard_Library.md")
    sections = parse_sections(spec, "spec") + parse_sections(stdlib, "stdlib")
    invariants = parse_appendix_c(spec)

    c_per_section = {}
    for row in invariants:
        for ref in row["refs"]:
            c_per_section.setdefault(("spec", ref), 0)
            c_per_section[("spec", ref)] += 1

    rows, problems = corpus.load_cases(os.path.join(base_dir, "cases"))
    cases_per_section = {}
    for case in rows:
        key = (case["doc"], case["section"])
        bucket = cases_per_section.setdefault(
            key, {"cases": 0, "accept": 0, "reject": 0, "run": 0})
        bucket["cases"] += 1
        bucket[case["expect"]] += 1

    out = []
    for s in sections:
        key = (s["doc"], s["section"])
        counts = cases_per_section.get(
            key, {"cases": 0, "accept": 0, "reject": 0, "run": 0})
        c_rows = c_per_section.get(key, 0)
        out.append({
            "doc": s["doc"], "section": s["section"], "title": s["title"],
            "density": s["density"], "appendix-c": c_rows,
            "cases": counts["cases"], "accept": counts["accept"],
            "reject": counts["reject"], "run": counts["run"],
            "state": state_of(s["density"], c_rows, counts),
        })
    # Cases in a section the documents do not have: a citation error worth seeing.
    known = {(s["doc"], s["section"]) for s in sections}
    for key in sorted(cases_per_section):
        if key not in known:
            counts = cases_per_section[key]
            out.append({"doc": key[0], "section": key[1],
                        "title": "(no such section in the document)",
                        "density": 0, "appendix-c": 0, "cases": counts["cases"],
                        "accept": counts["accept"], "reject": counts["reject"],
                        "run": counts["run"], "state": "ORPHAN"})
    return out, invariants, c_per_section, problems


def state_of(density, c_rows, counts):
    """A section's coverage state. `n/a` is the honest answer for a section that
    states no enforced consequence — prose, a worked example, a rationale aside."""
    want = density + c_rows
    if want == 0 and counts["cases"] == 0:
        return "n/a"
    if counts["cases"] == 0:
        return "UNCOVERED"
    if counts["cases"] < max(1, want // 4):
        return "thin"
    return "covered"


def render(rows):
    out = ["\t".join(COLUMNS)]
    for row in rows:
        out.append("\t".join(str(row[c]).replace("\t", " ") for c in COLUMNS))
    return "\n".join(out) + "\n"


def main(argv, base_dir=None):
    base = base_dir or os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(os.path.dirname(base)))
    ap = argparse.ArgumentParser(prog="coverage.py")
    ap.add_argument("--docs", default=os.path.join(repo, "docs"))
    ap.add_argument("--out", default=os.path.join(base, "coverage.tsv"))
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--gaps", action="store_true",
                    help="list uncovered sections, densest first")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--rules", default="",
                    help="print the rule-stating sentences of a section (and "
                         "its subsections), numbered, as an authoring checklist")
    ap.add_argument("--invariants", default="",
                    help="print the Appendix C rows citing sections under this "
                         "prefix (e.g. 28, or 28.3) and exit")
    args = ap.parse_args(argv)

    if args.rules:
        want = args.rules.lstrip("\u00a7")
        doc = "Bezel_Standard_Library.md" if want.startswith("lib:") \
            else "Bezel_Language_Spec.md"
        want = want[4:] if want.startswith("lib:") else want
        n = 0
        for sec in parse_sentences(os.path.join(args.docs, doc), want):
            print("\n\u00a7{}  {}".format(sec["section"], sec["title"]))
            for sentence in sec["rules"]:
                n += 1
                print("  {:>3}. {}".format(n, sentence))
        print("\n{} rule-stating sentences under \u00a7{}".format(n, want))
        return 0

    if args.invariants:
        want = args.invariants.lstrip("\u00a7")
        rows = parse_appendix_c(os.path.join(args.docs, "Bezel_Language_Spec.md"))
        hits = [r for r in rows
                if any(ref == want or ref.startswith(want + ".")
                       for ref in r["refs"])]
        for row in hits:
            print("[{}] {}".format(row["group"], row["text"]))
            print()
        print("{} of {} Appendix C invariants cite \u00a7{}"
              .format(len(hits), len(rows), want))
        return 0

    rows, invariants, c_per_section, problems = build(args.docs, base)
    for p in problems:
        print("LINT " + p)

    with open(args.out, "w", encoding="utf-8") as f:
        f.write(render(rows))

    total_cases = sum(r["cases"] for r in rows)
    in_scope = [r for r in rows if r["state"] != "n/a"]
    uncovered = [r for r in in_scope if r["state"] == "UNCOVERED"]
    thin = [r for r in in_scope if r["state"] == "thin"]
    orphans = [r for r in rows if r["state"] == "ORPHAN"]

    print("{} sections in the documents; {} state an enforced consequence"
          .format(len(rows), len(in_scope)))
    print("{} Appendix C invariant rows, citing {} distinct sections"
          .format(len(invariants), len(c_per_section)))
    print("corpus: {} cases — {} sections covered, {} thin, {} UNCOVERED"
          .format(total_cases, len(in_scope) - len(uncovered) - len(thin),
                  len(thin), len(uncovered)))
    if orphans:
        print("{} ORPHAN sections (cases citing a section the documents lack): {}"
              .format(len(orphans), ", ".join(o["section"] for o in orphans)))

    if args.gaps:
        print("\nUncovered, densest first:")
        for row in sorted(uncovered,
                          key=lambda r: -(r["density"] + 3 * r["appendix-c"]))[:args.top]:
            print("  {:<6} {:<9} density {:>3}  C-rows {:>2}  {}".format(
                row["doc"], row["section"], row["density"], row["appendix-c"],
                row["title"][:60]))
    if args.summary:
        print("\nCovered sections:")
        for row in rows:
            if row["cases"]:
                print("  {:<6} {:<9} {:>3} cases ({}/{}/{})  {}".format(
                    row["doc"], row["section"], row["cases"], row["accept"],
                    row["reject"], row["run"], row["title"][:50]))
    print("\nmatrix written to " + args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
