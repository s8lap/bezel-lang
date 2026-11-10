#!/usr/bin/env python3
"""check_anchors.py — proposal 0212, §8: every `old` anchor the proposal quotes must occur EXACTLY ONCE in its target
document at the base the proposal was written against, every range must have its start before its end, and every
section anchor a proposed text links to must exist (or be one of the anchors the proposal itself introduces).

Usage:  python3 -I check_anchors.py [--repo <root>] [--mentions]
  --mentions   also list, by document and line, each occurrence of the clause names stage 3 retires (§8.13).
Exit status 0 when every anchor resolves.
"""
import argparse, os, re, sys

SPEC = "docs/Bezel_Language_Spec.md"; STD = "docs/Bezel_Standard_Library.md"; RAT = "docs/Bezel_Design_Rationale.md"
G1 = "docs/Bezel_Guide_01_Memory_and_Ownership.md"; CG = "docs/Bezel_For_Code_Generation.md"; DEC = "docs/proposals/DECLINED.md"
G5 = "docs/Bezel_Guide_05_Collections_and_Iteration.md"

# (edit id, stage, document, start anchor, end anchor or None for a single-phrase anchor / insertion point)
ANCHORS = [
    ("8.1 §3.6 row",                  "S1", SPEC, "| **auto-hashable** | [§9.2](#sec-body-records) |", None),
    ("8.1 chapter table row",         "S1", SPEC, "| Letting a callee keep a callback or borrowed object (`scoped`) | [§28.14](#sec-scoped-callback-parameters) |", None),
    ("8.2 §28.15 insertion point",    "S1", SPEC, '<a id="sec-iteration-2"></a>', None),
    ("8.3 K2.3",                      "S3", SPEC, "Where they are not, an admission is made only under K4's posture:", "[§28.11](#sec-collection-element-ownership))."),
    ("8.3 K2 clause 5 insertion",     "S3", SPEC, "*Example.* A plain return must name a source every caller re-establishes (K2.4)", None),
    ("8.3 K4.3",                      "S3", SPEC, "3. **Recorded trust, or nothing.**", "*Scope* clause 1 states apart."),
    ("8.3 K4 second example",         "S3", SPEC, "*Example.* A plain field that holds a parameter's borrow is admitted on the field-held trust clause (K4.3)", "    _owner := owner;                // allowed (K4.3): the field-held trust clause; the report mode names this store"),
    ("8.3 K5.5",                      "S3", SPEC, "'s holds-borrows property), and locals — a `for` desugaring's hidden `__source`", None),
    ("8.3 K8.4",                      "S3", SPEC, "A plain parameter announces none: a callee that stores it holds the borrow under the borrowed-field rule's", "and the temporary is still disposed at its boundary (K8.1)."),
    ("8.4 contributions item 1",      "S3", SPEC, "1. a class- or interface-typed argument contributes its source set;", None),
    ("8.4 contributions item 5",      "S3", SPEC, "5. an `owned` argument, and a `null`-literal, other value-typed or other runtime-managed argument, contributes nothing.", None),
    ("8.4 closure provenance clause 1","S3", SPEC, "a captured function value its own set, transitively", None),
    ("8.5 field-store bullet",        "S3", SPEC, "- A borrow may be stored in a field of an object only if the borrow's source provably outlives that object. In clauses:", "upgrades the admission to a fully static bound at participating call sites."),
    ("8.5 report clause 1",           "S3", SPEC, "1. **The clauses:** the **field-held** clause", "each stated at its home."),
    ("8.5 report clause 4 insertion", "S1", SPEC, "The report is informative: it changes no verdict and no generated code, and outside the mode it is not emitted.", None),
    ("8.5 closing paragraph",         "S3", SPEC, "For *field-held* and *collection-held* borrows (", "backstopped by checked disposal mode."),
    ("8.6 §28.11 Storing a borrow",   "S3", SPEC, "**Storing a borrow (derives: [K2.3](#sec-kernel-k2)).**", "**Value-copy members.**"),
    ("8.7 §28.14 callee side",        "S2", SPEC, "- **Callee side:** the callee may store the borrow in a direct non-owned field of the bounding object", "remains legal with the trust residual."),
    ("8.7 §28.14 caller side",        "S2", SPEC, "and is admitted under the bounding-object forms above with *the argument's borrow source*", "and it is frozen until the bound's scope exits."),
    ("8.7 §28.14 closing paragraph",  "S3", SPEC, "The asymmetry with ordinary borrows is deliberate:", "(rationale [§5.25](Bezel_Design_Rationale.md#sec-deferred-activation-capture))."),
    ("8.8 §28.2 holder list",         "S3", SPEC, "a closure that captures such a holder, or an iterator, enumerator or lazy-query frame that carries it", None),
    ("8.8 §9.7 instance-field",       "S3", SPEC, "A class instance's lifetime is not bounded by any borrow source's scope, and a **direct** class-typed borrow field", "flag as its backstop."),
    ("8.8 §30.1 insertion",           "S3", SPEC, "*Example.* Awaiting before the source's scope ends discharges the settlement obligation; abandoning the task leaves it to the caller's trust:", None),
    ("8.8 Appendix C.3 insertion",    "S3", SPEC, "- A choice that holds borrows ([§35.4](#sec-choice-classification)'s property, whatever the choice's classification) is subject to", None),
    ("8.8 stdlib §1 insertion",       "S1", STD,  '<a id="sec-sendability-statements"></a>', None),
    ("8.8 stdlib comparer paragraph", "S3", STD,  "**A retained comparer is a borrow.**", "The comparerless constructors retain nothing."),
    ("8.9 rationale §4.2.6",          "S3", RAT,  "a callee-side no-store rule for every `Object` or interface parameter, which retires the back-reference idiom;", None),
    ("8.9 rationale §5.1",            "S3", RAT,  "**Where neither applies, the trust is recorded.**", "keeps the set closed."),
    ("8.9 rationale §5.31",           "S3", RAT,  "field-held, collection-held, comparer-held ([proposal 0138](proposals/0138-compiler-known-sendability.md) §7) and frame-held borrows, plus §28.14's two residuals", None),
    ("8.9 rationale §5.32 insertion", "S1", RAT,  "## 6. Nullability", None),
    ("8.10 DECLINED callee-side row", "S3", DEC,  "| A callee-side rule forbidding the store of every plain `Object` or interface parameter", None),
    ("8.10 DECLINED K8.4 row",        "S3", DEC,  "| Rejecting an unbound temporary or a scope-bound local passed to a plain parameter because the callee's body stores the parameter", None),
    ("8.10 DECLINED warning row",     "S3", DEC,  "| A caller-side warning when a fresh temporary reaches a plain parameter that the callee's body stores", None),
    ("8.10 DECLINED always-on row",   "S2", DEC,  "| An always-on trust-admission warning (the report mode in the default build)", None),
    ("8.10 DECLINED scoped Object",   "S3", DEC,  "| `scoped` on a parameter declared `Object` (or `Object?`)", None),
    ("8.10 DECLINED span field row",  "S3", DEC,  "| Retiring §9.7's instance-field ban for buffer- and array-backed spans", None),
    ("8.11 Guide 01 §3",              "S3", G1,   "A stashed reference is not proved safe.", None),
    ("8.11 Guide 01 §5 bullet",       "S3", G1,   "- **Back-references are not `owned`.**", None),
    ("8.11 Guide 01 §5 paragraphs",   "S3", G1,   "**The back-reference's guarantee is yours — unless you say `scoped`.**", "never returned or stored in another object (spec §28.14)."),
    ("8.11 Guide 05 §1 insertion",    "S3", G5,   "**Dictionaries in practice.**", "checked against the dictionary at every call as `Add`'s arguments are (spec §28.11)."),
    ("8.12 code-gen §4.4",            "S3", CG,   "- A borrow of a **scope-bound local** may not be stored in a field or in a longer-lived collection.", "so it is scope-bound — §28.3)."),
]

# anchors the proposal introduces (so links to them in proposed text are expected not to exist yet)
NEW_ANCHORS = {"sec-borrow-bearing-classes", "sec-borrow-bearing-statements"}
MENTIONS = ["field-held", "collection-held", "comparer-held", "borrowed-field", "recorded residual", "under its residual", "under the residual"]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "..")))
    ap.add_argument("--mentions", action="store_true")
    a = ap.parse_args()
    docs = {}
    def text(d):
        if d not in docs: docs[d] = open(os.path.join(a.repo, d), encoding="utf-8").read()
        return docs[d]
    bad = 0
    for eid, stage, doc, start, end in ANCHORS:
        t = text(doc)
        n = t.count(start)
        msg = ""
        if n != 1:
            msg = f"start anchor occurs {n} times"
        elif end is not None:
            i = t.index(start); j = t.find(end, i)
            if j < 0: msg = "end anchor not found after the start anchor"
            else:
                k = t.count(end, i, j + len(end))
                span = t[i:j + len(end)]
                if "\n## " in span or "\n### " in span: msg = "range crosses a section heading"
                elif len(span) > 20000: msg = f"range is {len(span)} bytes"
        print(f"{'OK  ' if not msg else 'FAIL'} [{stage}] {eid:34s} {os.path.basename(doc)}" + (f"  — {msg}" if msg else (f"  ({len(t[t.index(start):t.find(end, t.index(start)) + len(end)])} bytes)" if end else "")))
        if msg: bad += 1
    # section anchors linked from the proposal's proposed text
    prop = open(os.path.join(a.repo, "docs/proposals/0212-borrow-bearing-classes.md"), encoding="utf-8").read()
    have = {}
    for d in (SPEC, STD, RAT):
        have[d] = set(re.findall(r'<a id="([^"]+)">', text(d)))
    missing = set()
    for m in re.finditer(r"\]\((Bezel_[A-Za-z_]+\.md)?#(sec-[a-z0-9-]+)\)", prop):
        doc = {"Bezel_Language_Spec.md": SPEC, "Bezel_Standard_Library.md": STD, "Bezel_Design_Rationale.md": RAT}.get(m.group(1) or "")
        anc = m.group(2)
        if anc in NEW_ANCHORS: continue
        pool = [have[doc]] if doc else list(have.values())      # a bare `#sec-…` is relative to the document the text lands in
        if not any(anc in p for p in pool): missing.add((m.group(1) or "(same document)", anc))
    for doc, anc in sorted(missing):
        print(f"FAIL link target not found: {doc}#{anc}"); bad += 1
    print(f"\n{len(ANCHORS)} anchors, {bad} failure(s); linked section anchors checked against the spec, the standard library and the rationale")
    if a.mentions:
        print("\nconsequential mentions (§8.13):")
        for d in (SPEC, STD, RAT, CG, G1):
            lines = text(d).split("\n")
            for ph in MENTIONS:
                hits = [i + 1 for i, l in enumerate(lines) for _ in range(l.lower().count(ph))]
                if hits: print(f"  {os.path.basename(d):38s} {ph!r:24s} {len(hits):3d}  lines {sorted(set(hits))}")
    sys.exit(1 if bad else 0)

if __name__ == "__main__":
    main()
