#!/usr/bin/env python3
"""Audit of the sentences proposal 0211 affects, over the eleven documents the docs gate
covers and the Tour (the changelog is pinned history and is not read).

Every line that matches one of the groups below is a *site*. A site is **edited** when an
edit of edits.py replaces or extends text on that line, and **left** when LEFT below gives
a reason for it. The script fails if a site is neither.

  tier      a gloss of the throwing tier as "a bug" / "assert-shaped"
  join      a mention of DomainFaultedException
  range     OverflowException on a line that also names Parse / Convert / TryParse
  contract  the phrase "FormatException/OverflowException"
  catchall  a catch arm naming Exception alone, in any fenced block
  words     "boundary fault" / "defect exception" / "format fault"

Usage: doc_sites.py <repo-root> [--tsv]
"""
import collections, importlib.util, io, contextlib, os, re, sys

root = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("edits", os.path.join(here, "edits.py"))
ed = importlib.util.module_from_spec(spec)
argv = sys.argv; sys.argv = [argv[0], "none"]
with contextlib.redirect_stdout(io.StringIO()):
    try: spec.loader.exec_module(ed)
    except SystemExit: pass
sys.argv = argv

DOCS = ["docs/Bezel_Language_Spec.md", "docs/Bezel_Standard_Library.md", "docs/Bezel_Design_Rationale.md",
        "docs/Bezel_For_Code_Generation.md", "docs/Bezel_Tour.md"] + \
       [f"docs/Bezel_Guide_0{i}_{n}.md" for i, n in enumerate(
           ["Memory_and_Ownership", "Errors_and_Nullability", "Modeling_Data", "Concurrency",
            "Collections_and_Iteration", "Contracts_and_Purity", "Style"], 1)]
GROUPS = [
    ("tier", re.compile(r"assert-shaped|as a bug\b|is a bug at|absence is a bug|is a bug here")),
    ("join", re.compile(r"DomainFaultedException")),
    ("range", re.compile(r"OverflowException")),
    ("contract", re.compile(r"FormatException/OverflowException")),
    ("catchall", re.compile(r"^\s*(?:catch\s+)?(?:var \w+: )?Exception(?: when [^=]*)? =>")),
    ("words", re.compile(r"boundary fault|defect exception|format fault")),
]
RANGE_CTX = re.compile(r"Parse|Convert\b|TryParse")

# Sites left unedited, by (document basename fragment, distinctive text on the line) -> reason.
LEFT = [
    # tier
    ("Spec", "`E(n)` treats an undeclared value as a bug", "a cast is the program's own operation: a defect, as the gloss says"),
    ("Spec", "The checked cast is the assert-shaped tier", "a cast is the program's own operation: a defect, as the gloss says"),
    ("Standard_Library", "the `Try` twins (`TryPop`", "names the branch-shaped tier only"),
    ("Standard_Library", "an out-of-range index is the assert-shaped tier, a bug like the throwing indexer's", "an index is the program's: a defect, as the gloss says"),
    ("Standard_Library", "`E(n)` is the assert-shaped form", "a cast is the program's own operation: a defect"),
    ("Guide_05", "absence is a bug here", "a dictionary is the program's state: a defect, as the gloss says"),
    ("Guide_05", "the assert-shaped tier. The branch-shaped tier is `MinOrNull()`", "an empty collection is the program's state: a defect"),
    ("Guide_05", "absence is a bug at that call site", "a collection is the program's state: a defect"),
    ("Rationale", "treat an unchecked `malloc` as a bug", "not about exceptions"),
    # join: true of the subclass too
    ("Spec", "**Async entries.**", "true as written: DomainFailedException is a DomainFaultedException"),
    ("Spec", "surfaces here as DomainFaultedException (§30.14)", "a comment in an example; true of the subclass too"),
    ("Standard_Library", "(`FaultStackTrace` — [§6.8](#sec-domain))", "names the class that declares the member"),
    ("Standard_Library", "**Async entries.** The nine async rows", "true as written: the subclass is a DomainFaultedException"),
    ("Standard_Library", "A fault in the entry surfaces at the join as `DomainFaultedException`:", "introduces the declaration block, which L15 extends"),
    ("Standard_Library", "public class DomainFaultedException: Exception", "the declaration; L15 adds the subclass after it"),
    ("Standard_Library", "end DomainFaultedException;", "edited by L15 (the anchor spans two lines)"),
    ("Standard_Library", "join; faults surface as DomainFaultedException", "a comment in an example; true of the subclass too"),
    ("Rationale", "**Faults cross as `DomainFaultedException`, not as the original exception object.**", "the paragraph R2 edits"),
    ("Rationale", "faults already cross as `DomainFaultedException`", "true as written"),
    # range: arithmetic and conversion sites, not Parse input
    ("Standard_Library", "FormatException/OverflowException", "the contract's name; true of the subclass, as L7 states"),
    ("Standard_Library", "//   FormatException/OverflowException", "a listing comment; true of the subclass, as L7 states"),
    ("Standard_Library", "rounds `double.Parse`'s input", "the JSON reader raises SerializationException here; OverflowException is not raised"),
    ("Standard_Library", "`decimal.Parse` grammar", "the JSON reader raises SerializationException here"),
    ("For_Code_Generation", "Hierarchy: `Exception`", "the class tree; C3 adds the kinds after it"),
    ("For_Code_Generation", "never a deferred `OverflowException`", "a cast, not Parse"),
    ("Guide_02", "available without `import`", "the class list of §4; G2-2 adds the kinds after it"),
    # catch-all arms that re-raise, are compile errors already, or are deliberate
    ("Spec", "var ex: Exception => throw", "re-raises"),
    ("Spec", "    var ex: Exception => do", "logs and re-raises (§30.9's example)"),
    ("Spec", "  var ex: Exception => do", "logs and re-raises, or (§31.1) shows closure confinement; the arm type is not the example's subject"),
    ("Spec", "    Exception => throw", "re-raises"),
    ("Spec", "catch var ex: Exception => do", "§31.1's closure example; the arm type is not its subject"),
    ("Spec", "  Exception => ()", "§31.2.5's deliberate suppression; S7 adds the failures-only form beside it"),
    ("Standard_Library", "Handler failed", "a handler that isolates itself from the raise on purpose (§6.5)"),
    ("Rationale", "var ex: Exception => throw", "re-raises"),
    ("Rationale", "do throw; end", "re-raises"),
    ("Rationale", "  Exception => throw", "re-raises"),
    ("Rationale", "swallow per-handler failures", "§13.5's deliberate per-handler isolation"),
    ("Guide_02", "ERROR: ex is a borrow", "an example of a compile error"),
    ("For_Code_Generation", "ERROR: `ex` is a borrow", "an example of a compile error"),
    ("Standard_Library", "`Reinterpret<TTo>(value)` is the general", "a cast, not Parse"),
    ("Rationale", "`double.Parse` and `float.Parse` read a numeral at run time", "true of the subclass; the paragraph's own ground is that such input 'is a data problem'"),
    ("Rationale", "The survey (the owner's, 2026-09", "a survey of other languages"),
    ("Rationale", "**JSON.** `ReadFloat` takes `Parse`'s reading and range", "true of the subclass"),
    ("For_Code_Generation", "the first fault surfaces as `DomainFaultedException`", "true as written; C3 and C4 name the subclass"),
    ("Guide_06", "it mirrors Guide 2 §7's table exactly", "names band 1's defect exceptions; consistent"),
    # words
    ("Standard_Library", "the residual defect exceptions", "the band-1 sentence L1 edits"),
    ("Guide_02", "the boundary-and-defect tool", "names the two bands; consistent"),
    ("Guide_06", "pre-contract defect residual", "consistent with band 1"),
]

olds = collections.defaultdict(list)
for e in ed.E:
    for ln in e["old"].split("\n"):
        if ln.strip(): olds[e["doc"]].append((e["id"], ln))

sites = []; missing = 0
for d in DOCS:
    fence = False
    for n, line in enumerate(open(os.path.join(root, d), encoding="utf-8").read().split("\n"), 1):
        if line.startswith("```"): fence = not fence
        for g, rx in GROUPS:
            if not rx.search(line): continue
            if g == "range" and not RANGE_CTX.search(line): continue
            if g == "catchall" and not fence: continue
            ids = sorted({i for i, o in olds[d] if o in line})
            if ids: how, why = "edited", ",".join(ids)
            else:
                why = next((r for frag, text, r in LEFT if frag in d and text in line), None)
                how = "left" if why else "UNACCOUNTED"
                if not why: missing += 1; why = ""
            sites.append((g, d, n, how, why, line.strip()[:90]))

if "--tsv" in sys.argv:
    print("group\tdocument\tline\tdisposition\tedit_or_reason\ttext")
    for s in sites: print("\t".join(str(x) for x in s))
else:
    c = collections.Counter((s[0], s[3]) for s in sites)
    for g, _ in GROUPS:
        print(f"{g:9} sites {sum(v for (gg, h), v in c.items() if gg == g):3}  edited {c[(g, 'edited')]:3}  left {c[(g, 'left')]:3}  unaccounted {c[(g, 'UNACCOUNTED')]:3}")
    for s in sites:
        if s[3] == "UNACCOUNTED": print("UNACCOUNTED", s[1], s[2], s[5])
sys.exit(1 if missing else 0)
