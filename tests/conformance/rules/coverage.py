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
           "accept", "reject", "warn", "run", "state")


# A line that is only an HTML anchor or comment is markup, not prose. The spec
# emits `<a id="sec-...">` before every heading, so without this every container
# section looks non-empty and the container rule never fires.
_MARKUP_ONLY_RE = re.compile(r'^\s*(?:<a\s+id="[^"]*"\s*></a>|<!--.*?-->)\s*$')


def _is_prose(line):
    return bool(line.strip()) and not _MARKUP_ONLY_RE.match(line)


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
        # A *container* section is one whose own body is empty — every word under
        # it belongs to a subsection. Appendix C rows still cite it (a row citing
        # §28.3 is attributed to §28 too), so without this it reads as a
        # permanently uncovered high-value gap, and every wave re-investigates it
        # and finds nothing. Spec §1 and §5 are both containers.
        s["container"] = not any(_is_prose(line) for line in s["body"])
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
            # A citation to the OTHER document must not be filed under the
            # spec. Appendix C writes those two ways and both have to go:
            #   `stdlib [§4.14](Bezel_Standard_Library.md#…)` — linked, and
            #   `(stdlib §1)`                                   — bare.
            # Only the linked form was scrubbed, so the three bare ones in
            # Appendix C filed themselves under spec §1 and §6.5. §1 is a
            # *container* — no text of its own — so it then showed up as a gap
            # with two invariants and no cases, permanently, and lanes kept
            # going to look at it. The bare form has to be stripped first:
            # scrubbing the link form leaves the word `stdlib` orphaned.
            spec_only = re.sub(r'stdlib\s+\u00a7[\d.]+', ' ', line)
            spec_only = re.sub(
                r'\[?\u00a7[\d.]+\]?\([^)]*Bezel_Standard_Library[^)]*\)',
                ' ', spec_only)
            refs = sorted(set(SECTION_REF_RE.findall(spec_only)))
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
    acks, ack_problems = parse_acks(
        os.path.join(base_dir, "coverage_acks.tsv"),
        {r["case-id"] for r in rows})
    problems = list(problems) + ack_problems
    cases_per_section = {}
    for case in rows:
        key = (case["doc"], case["section"])
        bucket = cases_per_section.setdefault(
            key, {"cases": 0, "accept": 0, "reject": 0, "warn": 0, "run": 0})
        bucket["cases"] += 1
        bucket[case["expect"]] += 1

    out = []
    for s in sections:
        key = (s["doc"], s["section"])
        counts = cases_per_section.get(
            key, {"cases": 0, "accept": 0, "reject": 0, "warn": 0, "run": 0})
        c_rows = c_per_section.get(key, 0)
        out.append({
            "doc": s["doc"], "section": s["section"], "title": s["title"],
            "density": s["density"], "appendix-c": c_rows,
            "cases": counts["cases"], "accept": counts["accept"],
            "reject": counts["reject"], "warn": counts["warn"],
            "run": counts["run"],
            "state": state_of(s["density"], c_rows, counts,
                              container=s.get("container", False),
                              ack=acks.get(key)),
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
                        "warn": counts["warn"], "run": counts["run"],
                        "state": "ORPHAN"})
    return out, invariants, c_per_section, problems


def state_of(density, c_rows, counts, container=False, ack=None):
    """A section's coverage state. `n/a` is the honest answer for a section that
    states no enforced consequence — prose, a worked example, a rationale aside.

    Two states exist because a gap can be false in two different ways, and both
    had every wave re-investigating the same empty sections:

    - `n/a` for a *container* — a numbered heading whose own body is empty, all
      of whose text is in its subsections. Appendix C rows citing a subsection
      are attributed to the parent as well, so a container reads as a dense
      uncovered section while stating nothing at all.
    - `covered-elsewhere` for a section that does state a rule, where the rule is
      written down and tested under a *different* section. That is a judgement,
      not a measurement, so it is never inferred: it is claimed in
      `coverage_acks.tsv` with the case id that covers it, and the claim is
      checked — an ack naming a case that does not exist is a lint error."""
    want = density + c_rows
    if container and counts["cases"] == 0:
        return "n/a"
    if want == 0 and counts["cases"] == 0:
        return "n/a"
    if ack and counts["cases"] == 0:
        # An empty `covered-by` is the third honest answer: the rule is real and
        # uncovered, and no case can reach it through this harness (a rule about
        # two *versions* of one module, or one the runner never asks the
        # compiler for). Distinct from covered-elsewhere, which claims a case
        # exists, and from UNCOVERED, which says go and write one.
        return "covered-elsewhere" if ack["covered-by"].strip() else "out-of-reach"
    if counts["cases"] == 0:
        return "UNCOVERED"
    if counts["cases"] < max(1, want // 4):
        return "thin"
    return "covered"


ACK_COLUMNS = ("doc", "section", "covered-by", "note")


def parse_acks(path, known_case_ids):
    """`coverage_acks.tsv` — sections whose rules are covered under another
    section. Returns ({(doc, section): row}, [problem, ...])."""
    acks, problems = {}, []
    if not os.path.exists(path):
        return acks, problems
    with open(path, encoding="utf-8") as f:
        lines = [ln.rstrip("\n") for ln in f if ln.strip()
                 and not ln.startswith("#")]
    if not lines:
        return acks, problems
    header = lines[0].split("\t")
    if tuple(header) != ACK_COLUMNS:
        problems.append("coverage_acks.tsv: header is {}, expected {}"
                        .format(header, list(ACK_COLUMNS)))
        return acks, problems
    for ln in lines[1:]:
        parts = ln.split("\t")
        if len(parts) != len(ACK_COLUMNS):
            problems.append("coverage_acks.tsv: {} fields in {!r}"
                            .format(len(parts), ln[:60]))
            continue
        row = dict(zip(ACK_COLUMNS, parts))
        for cid in row["covered-by"].split(","):
            cid = cid.strip()
            if cid and cid not in known_case_ids:
                problems.append(
                    "coverage_acks.tsv: \u00a7{} is acked to {}, which is not a "
                    "case".format(row["section"], cid))
        acks[(row["doc"], row["section"])] = row
    return acks, problems


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

    acked = [r for r in rows if r["state"] == "covered-elsewhere"]
    out_of_reach = [r for r in rows if r["state"] == "out-of-reach"]
    if acked or out_of_reach:
        print("{} sections covered-elsewhere, {} out-of-reach "
              "(see coverage_acks.tsv)".format(len(acked), len(out_of_reach)))

    if args.gaps:
        if acked:
            print("\nCovered elsewhere, by claim:")
            for row in acked:
                print("  {:<6} {:<9} {}".format(
                    row["doc"], row["section"], row["title"][:60]))
        if out_of_reach:
            print("\nOut of reach of this harness:")
            for row in out_of_reach:
                print("  {:<6} {:<9} {}".format(
                    row["doc"], row["section"], row["title"][:60]))
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
                print("  {:<6} {:<9} {:>3} cases ({}/{}/{}/{})  {}".format(
                    row["doc"], row["section"], row["cases"], row["accept"],
                    row["reject"], row["warn"], row["run"], row["title"][:50]))
    print("\nmatrix written to " + args.out)
    return 0


if __name__ == "__main__":
    # `corpus.py summary | head` closes the pipe under us; a traceback there
    # reads as a tool failure.
    try:
        import signal
        signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    except (ImportError, AttributeError, ValueError):
        pass
    sys.exit(main(sys.argv[1:]))
