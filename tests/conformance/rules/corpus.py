#!/usr/bin/env python3
"""The rule corpus: parsing, linting, and manifest generation.

The corpus under `cases/` is HAND-AUTHORED, one case per normative rule, in
contrast to the sibling doc-example corpus (`tests/conformance/`), which is
EXTRACTED from the documents and regenerated wholesale. The two are deliberately
separate trees: an extraction overwrites, and a hand-authored case must survive
one.

Every case is a complete `.bzl` source file carrying its own metadata in a
`//!` header. `manifest.tsv` is DERIVED from those headers (`build` below), so
an authoring lane creates files and never edits a shared file — parallel lanes
cannot conflict, and the manifest cannot desync from the cases.

Header grammar (the leading run of `//!` lines, before any other content):

    //! case: s28_3_0010                   <- must equal the filename stem
    //! doc: spec                          <- spec | stdlib
    //! section: 28.3                      <- must agree with the case id
    //! title: one line, imperative or declarative, unique within the section
    //! rule: the normative sentence this case makes executable
    //!   (continuation lines start with `//!` and two or more spaces)
    //! cites: 28.3, C.3                   <- optional extra sections
    //! expect: reject                     <- accept | reject | warn | run
    //! diagnostic: disposed on every path <- required iff expect: reject|warn
    //! stdout: 42                         <- required iff expect: run
    //! issue: 4154                        <- optional; a tracker this case pins

`expect` is the verdict the SPEC requires, never the compiler's current
behaviour: a case whose verdict the compiler does not yet produce is a recorded
divergence (see runner.py's baseline ratchet), not a case to be edited.
"""

import os
import re
import sys

HEADER_RE = re.compile(r'^//!\s(?!\s)([a-z][a-z0-9_]*):\s?(.*)$')
CONT_RE = re.compile(r'^//!\s\s+(\S.*)$')
CASE_ID_RE = re.compile(r'^(s|l)(\d+)(?:_(\d+(?:_\d+)?))?_(\d{4})$')
MAIN_RE = re.compile(r'\bmethod\s+Main\b')

REQUIRED = ("case", "doc", "section", "title", "rule", "expect")
KNOWN = REQUIRED + ("cites", "diagnostic", "stdout", "output", "issue", "notes",
                    "flags")

# Compiler switches a case may ask for. An allowlist, not a passthrough: a case
# is data in a repository, and the runner executes what it says.
ALLOWED_FLAGS = ("--checked-disposal", "--experimental-scoped-await-bind")
EXPECTS = ("accept", "quiet", "reject", "warn", "run")
DOCS = ("spec", "stdlib")

MANIFEST_COLUMNS = ("case-id", "doc", "section", "expect", "shape",
                    "title", "rule", "cites", "check", "issue", "deps", "flags")


class CaseError(Exception):
    pass


def parse_header(text, where="<case>"):
    """Parse the leading `//!` header. Returns a dict of field -> value.
    Raises CaseError on a malformed header. Pure."""
    fields = {}
    order = []
    last = None
    seen_any = False
    for lineno, raw in enumerate(text.split("\n"), 1):
        line = raw.rstrip()
        if not line.startswith("//!"):
            if seen_any:
                break
            if line.strip() == "":
                continue
            raise CaseError("{}: line {}: content before the //! header"
                            .format(where, lineno))
        seen_any = True
        m = HEADER_RE.match(line)
        if m:
            key, value = m.group(1), m.group(2).strip()
            if key in fields:
                raise CaseError("{}: line {}: duplicate header field '{}'"
                                .format(where, lineno, key))
            fields[key] = value
            order.append(key)
            last = key
            continue
        c = CONT_RE.match(line)
        if c:
            if last is None:
                raise CaseError("{}: line {}: continuation with no field"
                                .format(where, lineno))
            # `output:` is a multi-LINE expectation, so its continuations are
            # lines; every other field's continuation is a wrapped sentence.
            join = "\n" if last == "output" else " "
            fields[last] = (fields[last] + join + c.group(1).rstrip()).strip()
            continue
        raise CaseError("{}: line {}: malformed header line: {!r}"
                        .format(where, lineno, line))
    if not seen_any:
        raise CaseError("{}: no //! header".format(where))
    return fields


def note_tokens(notes):
    """`notes` is a `;`-separated token list, exact-match — the same shape the
    doc-example harness uses, so prose containing a keyword switches nothing on."""
    return [t.strip() for t in (notes or "").split(";") if t.strip()]


def section_of_case_id(case_id):
    """`s28_3_0010` -> ('spec', '28.3'); `l4_9_0003` -> ('stdlib', '4.9').
    A section with no subsections takes the short form: `s34_0003` -> '34'."""
    m = CASE_ID_RE.match(case_id)
    if not m:
        return None
    doc = "spec" if m.group(1) == "s" else "stdlib"
    section = m.group(2)
    if m.group(3):
        section += "." + m.group(3).replace("_", ".")
    return doc, section


def shape_of(body):
    """Which compile shape the case takes: a program (has `method Main`, links
    and runs) or a library (`-c`). Derived from the source, never declared."""
    return "program" if MAIN_RE.search(body) else "lib"


def strip_header(text):
    """The case source with its `//!` header removed, for hashing/shape."""
    out = []
    in_header = True
    for line in text.split("\n"):
        if in_header and (line.startswith("//!") or line.strip() == ""):
            if line.startswith("//!"):
                continue
            if not out:
                continue
        in_header = False
        out.append(line)
    return "\n".join(out)


def check_case(path, text):
    """Validate one case file. Returns (fields, problems): fields is the parsed
    header (possibly incomplete), problems a list of strings. Pure."""
    problems = []
    stem = os.path.splitext(os.path.basename(path))[0]
    try:
        fields = parse_header(text, where=stem)
    except CaseError as e:
        return {}, [str(e)]

    for key in REQUIRED:
        if key not in fields or fields[key] == "":
            problems.append("{}: missing required header field '{}'".format(stem, key))
    for key in fields:
        if key not in KNOWN:
            problems.append("{}: unknown header field '{}'".format(stem, key))

    case_id = fields.get("case", "")
    if case_id != stem:
        problems.append("{}: header case '{}' does not match the filename"
                        .format(stem, case_id))
    parsed = section_of_case_id(stem)
    if parsed is None:
        problems.append("{}: case id is not <s|l><section>_<NNNN>".format(stem))
    else:
        doc, section = parsed
        if fields.get("doc") not in (None, doc):
            problems.append("{}: doc '{}' disagrees with the id's '{}'"
                            .format(stem, fields.get("doc"), doc))
        if fields.get("section") not in (None, section):
            problems.append("{}: section '{}' disagrees with the id's '{}'"
                            .format(stem, fields.get("section"), section))
    if fields.get("doc") not in (None, "") and fields.get("doc") not in DOCS:
        problems.append("{}: doc must be one of {}".format(stem, "|".join(DOCS)))

    expect = fields.get("expect", "")
    if expect and expect not in EXPECTS:
        problems.append("{}: expect must be one of {}".format(stem, "|".join(EXPECTS)))
    if expect in ("reject", "warn") and not fields.get("diagnostic"):
        problems.append("{}: a {} case must carry `diagnostic:` — the wording the "
                        "spec uses for the rule it names".format(stem, expect))
    if expect not in ("reject", "warn") and fields.get("diagnostic"):
        problems.append("{}: `diagnostic:` belongs only on a reject or warn case"
                        .format(stem))
    if expect == "run" and "stdout" not in fields and "output" not in fields:
        problems.append("{}: a run case must carry `stdout:` (its first line) or "
                        "`output:` (every line)".format(stem))
    if "stdout" in fields and "output" in fields:
        problems.append("{}: carry either `stdout:` or `output:`, not both"
                        .format(stem))
    for key in ("stdout", "output"):
        if expect != "run" and key in fields:
            problems.append("{}: `{}:` belongs only on a run case"
                            .format(stem, key))

    body = strip_header(text)
    # A REJECT case may be about a mismatched label (§1.6 states the rule), so
    # the check runs only where a typo would make the case fail for the wrong
    # reason. `corpus.py audit` lists the reject-case hits for a human.
    # Unconditional: Appendix A.2 has no top-level member alternative, so this
    # is a parse error whatever the case expects, and a REJECT case with one
    # passes for the wrong reason forever rather than failing loudly.
    problems.extend(top_level_member_problems(stem, body))
    if expect != "reject":
        problems.extend(end_label_problems(stem, body))
        problems.extend(implicit_constructor_problems(stem, body))
    if expect == "run" and fields.get("stdout", "").strip() in ("true", "false"):
        problems.append("{}: a `bool` renders `True`/`False` (§8.5's worked "
                        "`IsEmpty: False`), not lowercase".format(stem))
    if body.strip() == "":
        problems.append("{}: no source below the header".format(stem))
    shape = shape_of(body)
    if expect == "run" and shape != "program":
        problems.append("{}: a run case needs a `method Main`".format(stem))
    if expect in ("accept", "quiet") and shape == "program":
        problems.append("{}: this case has a `method Main`, so it can check what it "
                        "prints — use `expect: run` with `stdout:`".format(stem))
    if not re.match(r'^\s*module\s+\w', body):
        problems.append("{}: a case is a whole source file and starts with "
                        "`module Conf;`".format(stem))
    for marker in ("TODO", "FIXME", "XXX", "..."):
        if marker in body:
            problems.append("{}: source contains {!r} — a case is complete code"
                            .format(stem, marker))
    for flag in (fields.get("flags", "") or "").split():
        if flag not in ALLOWED_FLAGS:
            problems.append("{}: `flags:` admits only {} — {} is not on the "
                            "allowlist".format(stem, ", ".join(ALLOWED_FLAGS),
                                               flag))
    title = fields.get("title", "")
    if len(title) > 100:
        problems.append("{}: title is over 100 characters".format(stem))
    return fields, problems


def case_entries(cases_dir):
    """Every case in the directory, as (case_id, main_path, dep_names).

    A case is usually one `.bzl` file. The rules that need two compilation
    units — §2.8's artifact set, §13.4's cross-module visibility, §26.5's
    imported extension blocks — cannot be one, so a case may instead be a
    DIRECTORY holding `main.bzl` (which carries the header) beside its
    dependencies, compiled in lexicographic order and imported by everything
    after them."""
    entries = []
    if not os.path.isdir(cases_dir):
        return entries
    for name in sorted(os.listdir(cases_dir)):
        path = os.path.join(cases_dir, name)
        if name.endswith(".bzl") and os.path.isfile(path):
            entries.append((name[:-len(".bzl")], path, []))
        elif os.path.isdir(path):
            members = sorted(n for n in os.listdir(path) if n.endswith(".bzl"))
            deps = [n for n in members if n != "main.bzl"]
            entries.append((name, os.path.join(path, "main.bzl"), deps))
    return entries


# §1.6: an `end` label that is supplied must match the construct it closes. A
# mismatched label does not parse — and it is invisible to every other check
# here, which is how five cases carrying `end Main;` in a class named `Program`
# with a method named `Run` passed a clean lint. (The rename that produced them
# was itself made to satisfy the `method Main` shape check, whose regex the
# `end Main;` does not match. The dodge blinded the check to the typo.)
_END_LABEL_RE = re.compile(r'^\s*end\s+([A-Za-z_]\w*)\s*;')
_NAME_DECL_RE = re.compile(
    r'\b(?:class|record|interface|enum|choice|method|property|extension)\s+'
    r'([A-Za-z_]\w*)')
# Labels that name a construct keyword rather than an identifier (§1.6).
_KEYWORD_LABELS = {"if", "while", "for", "case", "try", "do", "constructor",
                   "destructor", "extension", "extern", "operator", "lambda"}


# Appendix A.2: `TopDecl ::= ClassDecl | InterfaceDecl | RecordDecl | EnumDecl
# | ChoiceDecl | ExtensionDecl | TypeAliasDecl | ExternBlock`. `method` is not
# there — Bezel has no free functions. Ten cases declared one anyway, every one
# of them a parse error: the authors were copying the documents' illustrative
# `public method ReportEach<T>(…)` fragments, which are members shown without
# their enclosing class. A helper goes in a `public static class`.
TOP_LEVEL_MEMBER_START = re.compile(
    r'^(?:public |internal |private )?(?:static |pure |async )*'
    r'(?:method|property|constructor|destructor)\b')


def top_level_member_problems(case_id, body):
    """A member declared at module level, outside any type."""
    problems, depth = [], 0
    for line in body.split("\n"):
        stripped = line.strip()
        if not stripped or stripped.startswith("//"):
            continue
        if depth == 0 and TOP_LEVEL_MEMBER_START.match(line):
            problems.append(
                "{}: `{}` is declared at module level — Appendix A.2's TopDecl "
                "has no member alternative; put it in a class"
                .format(case_id, stripped[:48]))
        if re.match(r'^(?:public |internal |private )?(?:static |sealed |'
                    r'abstract |partial )*'
                    r'(?:class|interface|record|choice|enum|extension)\b', line):
            depth += 1
        elif depth and re.match(r'^end\b', line):
            depth -= 1
    return problems


def end_label_problems(case_id, body):
    """Every `end X;` whose X names no construct the file declares, and is not a
    construct keyword or a loop label the file introduces."""
    names = set(_NAME_DECL_RE.findall(body))
    # `label: while … end label;` — a loop label is an identifier before a colon.
    names |= set(re.findall(r'^\s*([A-Za-z_]\w*)\s*:\s*(?:while|for|do)\b',
                            body, re.MULTILINE))
    out = []
    for lineno, line in enumerate(body.split("\n"), 1):
        m = _END_LABEL_RE.match(line.split("//")[0])
        if not m:
            continue
        label = m.group(1)
        if label in _KEYWORD_LABELS or label in names:
            continue
        out.append("{}: line {}: `end {};` names no construct this file "
                   "declares (§1.6 — a supplied label must match)"
                   .format(case_id, lineno, label))
    return out


_CLASS_DECL_RE = re.compile(
    r'^\s*(?:public\s+|internal\s+|private\s+|protected\s+)?'
    r'(?:abstract\s+|closed\s+|sealed\s+|static\s+)*class\s+([A-Za-z_]\w*)'
    r'(?:<[^>]*>)?\s*(\()?', re.MULTILINE)
_NEW_RE = re.compile(r'\bnew\s+([A-Z]\w*)\s*(?:<[^<>]*>)?\s*\(')


def implicit_constructor_problems(case_id, body):
    """`new C()` where C is declared in this file and declares no constructor.

    AUTHORING rule 3: the spec does not state that a class with no declared
    constructor has an implicit parameterless one (#4364), so a case that relies
    on it would be a false divergence the day the corpus runs. Classes the case
    does NOT declare are the stdlib's and are none of this check's business."""
    declared, has_ctor = {}, set()
    current = None
    for line in body.split("\n"):
        m = _CLASS_DECL_RE.match(line)
        if m:
            current = m.group(1)
            declared[current] = True
            if m.group(2):          # a primary constructor
                has_ctor.add(current)
            continue
        if re.match(r'^\s*end\b', line):
            current = None
            continue
        if current and re.search(r'\bconstructor\b', line.split("//")[0]):
            has_ctor.add(current)
    out = []
    for name in sorted(set(_NEW_RE.findall(body))):
        if name in declared and name not in has_ctor:
            out.append("{}: `new {}(…)` on a class this file declares with no "
                       "constructor — declare one (#4364: the spec does not "
                       "state an implicit parameterless constructor exists)"
                       .format(case_id, name))
    return out


def load_cases(cases_dir):
    """Read every case. Returns (rows, problems); rows are dicts in manifest
    column order, sorted by case id."""
    rows = []
    problems = []
    if not os.path.isdir(cases_dir):
        return rows, ["cases directory not found: " + cases_dir]
    titles = {}
    for case_id, path, deps in case_entries(cases_dir):
        if not os.path.isfile(path):
            problems.append("{}: a directory case needs a main.bzl carrying "
                            "the header".format(case_id))
            continue
        with open(path, encoding="utf-8") as f:
            text = f.read()
        fields, errs = check_case(case_id + ".bzl", text)
        for dep in deps:
            dep_path = os.path.join(cases_dir, case_id, dep)
            with open(dep_path, encoding="utf-8") as f:
                dep_text = f.read()
            if dep_text.lstrip().startswith("//!"):
                errs.append("{}/{}: only main.bzl carries the //! header"
                            .format(case_id, dep))
            if not re.match(r'^\s*module\s+\w', dep_text):
                errs.append("{}/{}: a dependency is a whole source file and "
                            "starts with `module …;`".format(case_id, dep))
            if MAIN_RE.search(dep_text) and "dep-main" not in note_tokens(
                    fields.get("notes")):
                errs.append("{}/{}: a dependency is a library — the `method "
                            "Main` belongs in main.bzl. §2.8.4's rules about a "
                            "qualifying Main IN a dependency are the exception: "
                            "opt in with `notes: dep-main`."
                            .format(case_id, dep))
        problems.extend(errs)
        if errs:
            continue
        key = (fields["section"], fields["title"].lower())
        if key in titles:
            problems.append("{}: title duplicates {}'s within {}"
                            .format(fields["case"], titles[key], fields["section"]))
        else:
            titles[key] = fields["case"]
        check = fields.get("diagnostic") or fields.get("stdout", "")
        if "output" in fields:
            check = "all:" + fields["output"]
        rows.append({
            "case-id": fields["case"],
            "doc": fields["doc"],
            "section": fields["section"],
            "expect": fields["expect"],
            "shape": shape_of(strip_header(text)),
            "title": fields["title"],
            "rule": fields["rule"],
            "cites": fields.get("cites", ""),
            "check": check,
            "issue": fields.get("issue", ""),
            "deps": ";".join(deps),
            "flags": fields.get("flags", ""),
        })
    rows.sort(key=lambda r: _sort_key(r["case-id"]))
    return rows, problems


def _sort_key(case_id):
    m = CASE_ID_RE.match(case_id)
    if not m:
        return (9, case_id)
    parts = [int(p) for p in m.group(3).split("_")] if m.group(3) else [0]
    while len(parts) < 2:
        parts.append(0)
    return (0 if m.group(1) == "s" else 1, int(m.group(2)),
            parts[0], parts[1], int(m.group(4)))


def render_manifest(rows):
    out = ["\t".join(MANIFEST_COLUMNS)]
    for row in rows:
        cells = []
        for col in MANIFEST_COLUMNS:
            value = row[col].replace("\t", " ").replace("\n", "\\n")
            cells.append(value)
        out.append("\t".join(cells))
    return "\n".join(out) + "\n"


def read_manifest(path):
    """Read a generated manifest back into row dicts."""
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            cells = line.split("\t")
            if cells[0] == "case-id":
                continue
            while len(cells) < len(MANIFEST_COLUMNS):
                cells.append("")
            rows.append(dict(zip(MANIFEST_COLUMNS, cells)))
    return rows


# An order-or-count CLAIM, as the documents phrase one. `stdout:` checks the
# first line only, so it cannot see a step that ran twice, ran in the wrong
# order, or never ran at all — a case making one of these claims and checking
# one line is the corpus's most-repeated defect (one batch had it in 12 of its
# 15 run cases, two of them unfalsifiable as written).
#
# Deliberately narrow. A broad vocabulary ("first", "each", "count") flagged 225
# of 723 run cases, mostly on incidental words; a report that is two-thirds
# noise gets ignored and then switched off. This one flags ~34 and most are
# real. It is a REPORT, not a lint, because it cannot decide the honest
# exception: a program may encode an order into a single line ("a,b,c", or a sum
# that only one order produces), and then one line is a complete check.
ORDER_CLAIM_RE = re.compile(
    r'(reverse order|in order|order of (?:establishment|declaration|disposal|'
    r'construction|evaluation|registration)|declaration order|evaluation order|'
    r'disposal order|exactly once|exactly twice|more than once|not run again|'
    r'runs again|re-?runs?\b|re-?enumerat\w+|each iteration|per iteration|'
    r'on each|idempotent|twice|both .{0,24}(?:run|dispose|fire|print)|'
    r'never (?:runs|re-?runs|fires)|\bthen\b.{0,60}\bthen\b|'
    # Added after a §31 lane found two claims the vocabulary could not see:
    # "used when the body contains multiple statements" and "one statement
    # among several" are count claims with no trigger word above.
    r'multiple statements|among several|several statements|'
    r'(?:arms?|clauses?|handlers?) after|not considered|'
    r'\bfirst\b.{0,40}\bwins\b|stop(?:s|ping)? at the first)', re.IGNORECASE)


def audit(rows, cases_dir):
    """Quality signals a reader would otherwise have to check by hand. None is a
    lint failure — each is a judgement call the author may have made
    deliberately — so this prints and never fails."""
    def words(text):
        return set(re.findall(r'[a-z]{3,}', text.lower()))

    orphan_diag, no_cites, long_source, twins, reject_labels = [], [], [], {}, []
    order_first_line = []
    for row in rows:
        if row["expect"] in ("reject", "warn") and row["check"]:
            # The contract says a diagnostic fragment is the RULE's own wording.
            frag = words(row["check"])
            # A diagnostic that is an identifier the case declares (`Money`,
            # `Lib`) is the unresolved-name shape and is fine as it stands.
            identifierish = re.fullmatch(r'[A-Z]\w*', row["check"].strip())
            # Compare against `rule:` ONLY, never `rule + title`. A case whose
            # `diagnostic:` invented a word ("redundant", where the rule says
            # "at a marker-free position") had copied the same word into its
            # title, and the check passed: an author who is consistently wrong
            # defeats a check that lets them supply their own vocabulary.
            if frag and not identifierish and not (frag & words(row["rule"])):
                orphan_diag.append((row["case-id"], row["check"]))
        if row["expect"] == "run" and not row["check"].startswith("all:") \
                and ORDER_CLAIM_RE.search(row["rule"] + " " + row["title"]):
            order_first_line.append((row["case-id"], row["title"]))
        if not row["cites"]:
            no_cites.append(row["case-id"])
        key = re.sub(r'[^a-z ]', '', row["rule"].lower())[:120]
        twins.setdefault(key, []).append(row["case-id"])
        path = os.path.join(cases_dir, row["case-id"] + ".bzl")
        if os.path.isfile(path):
            with open(path, encoding="utf-8") as f:
                body = strip_header(f.read())
            n = len([l for l in body.split("\n") if l.strip()])
            if n > 45:
                long_source.append((row["case-id"], n))
            if row["expect"] == "reject":
                reject_labels.extend(end_label_problems(row["case-id"], body))

    print("{} reject/warn cases whose diagnostic shares no word with their rule"
          .format(len(orphan_diag)))
    for case_id, frag in orphan_diag[:30]:
        print("   {}  {!r}".format(case_id, frag))
    # A pair states ONE rule from both sides, so a shared rule text within a
    # section is the pairing discipline working. Across sections it is a
    # duplicate, and the rule's home section owns it.
    dup = {k: v for k, v in twins.items() if len(v) > 1 and k
           and len({section_of_case_id(i)[1] for i in v
                    if section_of_case_id(i)}) > 1}
    print("\n{} rule texts shared ACROSS sections (a pair within one section "
          "is the pairing discipline, not a duplicate)".format(len(dup)))
    for ids in list(dup.values())[:20]:
        print("   " + ", ".join(ids))
    print("\n{} reject cases carrying an unmatched `end` label — deliberate in a "
          "case about §1.6, a second ground anywhere else".format(len(reject_labels)))
    for line in reject_labels[:20]:
        print("   " + line)
    print("\n{} run cases making an order-or-count claim but checking only the "
          "first line".format(len(order_first_line)))
    print("   Each is either an `output:` conversion or a case whose single "
          "line already encodes the order.")
    for case_id, title in order_first_line[:40]:
        print("   {}  {}".format(case_id, title[:74]))

    print("\n{} cases with no `cites:`".format(len(no_cites)))
    print("\n{} cases over 45 non-blank source lines".format(len(long_source)))
    for case_id, n in sorted(long_source, key=lambda t: -t[1])[:15]:
        print("   {}  {} lines".format(case_id, n))
    return 0


def _here():
    return os.path.dirname(os.path.abspath(__file__))


def main(argv, base_dir=None):
    base = base_dir or _here()
    cases_dir = os.path.join(base, "cases")
    manifest = os.path.join(base, "manifest.tsv")
    cmd = argv[0] if argv else "lint"

    if cmd not in ("lint", "build", "check", "summary", "audit"):
        print("usage: corpus.py [lint|build|check|summary|audit]", file=sys.stderr)
        return 2

    rows, problems = load_cases(cases_dir)
    if problems:
        for p in problems:
            print("LINT " + p)
    if cmd == "lint":
        print("lint: {} cases, {} problems".format(len(rows), len(problems)))
        return 1 if problems else 0

    if problems:
        print("refusing to {} with {} lint problems".format(cmd, len(problems)))
        return 1

    text = render_manifest(rows)
    if cmd == "build":
        with open(manifest, "w", encoding="utf-8") as f:
            f.write(text)
        print("build: wrote {} rows to {}".format(len(rows), manifest))
        return 0
    if cmd == "check":
        if not os.path.exists(manifest):
            print("check: manifest.tsv missing — run `corpus.py build`")
            return 1
        with open(manifest, encoding="utf-8") as f:
            have = f.read()
        if have != text:
            print("check: manifest.tsv is stale — run `corpus.py build`")
            return 1
        print("check: manifest.tsv is up to date ({} rows)".format(len(rows)))
        return 0

    if cmd == "audit":
        return audit(rows, cases_dir)

    by_section = {}
    for row in rows:
        by_section.setdefault((row["doc"], row["section"]), []).append(row)
    print("{} cases across {} sections".format(len(rows), len(by_section)))
    for expect in EXPECTS:
        n = sum(1 for r in rows if r["expect"] == expect)
        print("  {:<8} {}".format(expect, n))
    for key in sorted(by_section, key=lambda k: _sort_key(by_section[k][0]["case-id"])):
        group = by_section[key]
        counts = {e: sum(1 for r in group if r["expect"] == e) for e in EXPECTS}
        print("  {:<6} {:<8} {:>3}  (accept {}, reject {}, warn {}, run {})".format(
            key[0], key[1], len(group), counts["accept"], counts["reject"],
            counts["warn"], counts["run"]))
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
