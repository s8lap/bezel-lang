#!/usr/bin/env python3
"""Check the corpus against the documented standard-library surface.

    names.py [--docs DIR] [--case ID] [--strict]

A recurring failure mode in generated Bezel is code that looks plausible and
uses types or methods the stdlib does not define (`MemoryStream`, `File.Open`,
`ToList`, `Tuple`). CLAUDE.md forbids it; this measures it. The index is built
from the fenced `bezel` blocks of `Bezel_Standard_Library.md` and
`Bezel_For_Code_Generation.md` — the same source a human would check by hand.

Two reports:

* **unknown type** — a PascalCase name used in a type position that is neither
  declared in the case, a type parameter, a primitive, nor documented.
* **unknown member** — `T.m(...)` where `T` IS a documented type and `m` is not
  in its documented surface.

This is a REPORT, not a proof: the extraction is regex-shaped and the member
tier misses inherited and extension members, so a finding is a thing to check,
not a verdict. It is deliberately not wired into the gate for that reason.
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus  # noqa: E402

FENCE_RE = re.compile(r'```bezel\n(.*?)```', re.DOTALL)
TYPE_DECL_RE = re.compile(
    r'^\s*(?:public\s+|internal\s+|private\s+|protected\s+)?'
    r'(?:static\s+|abstract\s+|closed\s+|sealed\s+)*'
    r'(class|interface|record|enum|choice)\s+([A-Za-z_]\w*)')
ALIAS_RE = re.compile(r'^\s*type\s+([A-Za-z_]\w*)')
IMPORT_ALIAS_RE = re.compile(r'^\s*import\s+[\w.]+\s+as\s+([A-Za-z_]\w*)\s*;')
MEMBER_RE = re.compile(
    r'\b(?:method|property|const)\s+(?:operator\s+\S+|([A-Za-z_]\w*))')
ENUM_CONST_RE = re.compile(r'^\s*([A-Z]\w*)\s*(?:,|;|:=|\()')
GENERIC_PARAM_RE = re.compile(r'<([^<>()]*)>')

PRIMITIVES = {
    "bool", "byte", "sbyte", "short", "ushort", "int", "uint", "long", "ulong",
    "float", "double", "decimal", "char", "string", "Object", "Boolean", "Byte",
    "SByte", "Int16", "UInt16", "Int32", "UInt32", "Int64", "UInt64", "Single",
    "Double", "Decimal", "Char", "String",
}
# Names the language itself provides, outside any stdlib block.
LANGUAGE = {"Task", "OwningTask", "CancellationToken", "Subscription", "Main"}

# Members the language provides on types that declare them nowhere: Object's
# virtuals (spec §8), IDisposable's generated Dispose (§28.4), and the members
# the compiler puts on EVERY enum (§10.4, §5.4, v0.293).
UNIVERSAL_MEMBERS = {
    "ToString", "Equals", "GetHashCode", "Dispose",
    "NameOf", "FromUnderlyingOrNull", "FromNameOrNull",
}

TYPE_POSITION_RES = (
    re.compile(r':\s*(?:owned\s+|scoped\s+)?\[?\d*\s*(?:owned)?\]?\s*([A-Z]\w*)'),
    re.compile(r'\bnew\s+([A-Z]\w*)'),
    re.compile(r'\b(?:is|as)\s+(?:not\s+)?([A-Z]\w*)'),
    # `Array<int>` binds tight; `a.Kg < b` and `x <> y` are not generics.
    re.compile(r'(?<![.\w])([A-Z]\w*)<(?![>=])'),
    re.compile(r'\b(?:typeof|default)\(\s*([A-Z]\w*)'),
    re.compile(r'^\s*(?:public|internal|private|protected)?\s*\w*\s*'
               r'(?:class|interface|record|enum|choice)\s+\w+\s*'
               r'(?:<[^>]*>)?\s*(?:\([^)]*\))?\s*:\s*([A-Z]\w*)'),
)
STATIC_USE_RE = re.compile(r'\b([A-Z]\w*)\.([A-Za-z_]\w*)')


# The stdlib document is organised as `## N. `Module.Name`` headings, and an
# auto-imported module says so in its own heading. Spec §2.3 then carves out
# the handful of types that are always available despite living in a module that
# is not: "Importing `System.Threading` is not required to use `Subscription`,
# `CancellationToken`, or any of the six event types; it is required for
# `CancellationTokenSource`, `Mutex`, and the other threading types."
MODULE_HEADING_RE = re.compile(r'^##\s+\d+\.\s+`([A-Za-z][\w.]*)`(.*)$')
ANY_TOP_HEADING_RE = re.compile(r'^##\s+\d+\.')
# `### 10.5 `System.Interop`: `Ptr<T>`` — a subsection that switches module
# inside a chapter whose own heading is not a module name.
SUB_MODULE_HEADING_RE = re.compile(r'^###\s+[\d.]+\s+`([A-Za-z][\w.]+\.[\w.]+)`\s*:')

ALWAYS_AVAILABLE_DESPITE_MODULE = frozenset({
    # spec §2.3, "Events, subscription, cancellation" and "Task family".
    "Subscription", "CancellationToken",
    "IEvent", "IAsyncEvent", "EventSource", "AsyncEventSource",
    "ISharedEvent", "SharedEventSource",
    "Task", "OwningTask",
})

# Auto-imported modules whose stdlib heading does not say so. Spec §2.3 and the
# stdlib's own §5 body both state System.Collections.Linq is auto-imported
# (v0.278) while its `## 5.` heading omits the marker every other auto-imported
# chapter carries. Reported as #4412; until the heading is fixed, this list
# keeps the check from demanding an import the language does not want.
AUTO_IMPORTED_DESPITE_HEADING = frozenset({"System.Collections.Linq"})


def build_module_index(docs_dir):
    """type name -> the module that declares it, for modules needing an import.

    Six §28.12 cases named `Task` with no `import System.Threading;` and would
    have failed to resolve it before reaching any rule under test. The check is
    mechanical, so it should not have been a reader's job."""
    path = os.path.join(docs_dir, "Bezel_Standard_Library.md")
    modules, of_type = {}, {}
    if not os.path.exists(path):
        return of_type
    with open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")
    current, fenced, block = None, False, []
    for line in lines:
        head = MODULE_HEADING_RE.match(line) if not fenced else None
        if head:
            current = head.group(1)
            modules[current] = ("auto-imported" not in head.group(2).lower()
                                and current not in AUTO_IMPORTED_DESPITE_HEADING)
            continue
        if not fenced and ANY_TOP_HEADING_RE.match(line):
            # A chapter that is not a module ("9. Primitive type members",
            # "10. FFI-boundary types") must CLEAR the module, or its types
            # inherit the previous chapter's and the check demands an import
            # for them. `Ptr<T>` landed in System.Diagnostics this way.
            current = None
            continue
        if not fenced:
            sub = SUB_MODULE_HEADING_RE.match(line)
            if sub:
                current = sub.group(1)
                modules.setdefault(
                    current, current not in AUTO_IMPORTED_DESPITE_HEADING)
                continue
        if line.startswith("```"):
            if fenced:
                for bline in block:
                    decl = TYPE_DECL_RE.match(bline)
                    if decl and current and modules.get(current):
                        of_type.setdefault(decl.group(2), current)
                block = []
            fenced = not fenced
            continue
        if fenced:
            block.append(line)
    for name in ALWAYS_AVAILABLE_DESPITE_MODULE:
        of_type.pop(name, None)
    return of_type


# The sections that state the import and name-resolution rules. A case there may
# be *about* naming a type without its import, which is the same shape this
# check reports — the reserved-name check has the same exception for the same
# reason.
IMPORT_RULE_SECTIONS = frozenset({"2.2", "2.3", "2.7", "2.8.3"})


def missing_imports(source, module_of_type):
    """Types the case names whose module it never imports."""
    imported = set(re.findall(r'^\s*import\s+([A-Za-z][\w.]*)', source,
                              re.MULTILINE))
    declared = declared_in(source)
    missing = {}
    for line in source.split("\n"):
        code = STRING_RE.sub('""', line.split("//")[0])
        if re.match(r'\s*(module|import)\b', code):
            continue
        for rx in TYPE_POSITION_RES:
            for m in rx.finditer(code):
                name = m.group(1)
                module = module_of_type.get(name)
                if module and module not in imported and name not in declared:
                    missing[name] = module
    return sorted(missing.items())


def build_index(docs_dir):
    """type name -> set of member names, from the documents' bezel blocks."""
    index = {}
    for name in ("Bezel_Standard_Library.md", "Bezel_For_Code_Generation.md"):
        path = os.path.join(docs_dir, name)
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8") as f:
            text = f.read()
        for block in FENCE_RE.findall(text):
            current = None
            for line in block.split("\n"):
                decl = TYPE_DECL_RE.match(line)
                if decl:
                    current = decl.group(2)
                    index.setdefault(current, set())
                    continue
                alias = ALIAS_RE.match(line)
                if alias:
                    index.setdefault(alias.group(1), set())
                    continue
                if current is None:
                    continue
                if re.match(r'^\s*end\b', line):
                    current = None
                    continue
                for m in MEMBER_RE.finditer(line):
                    if m.group(1):
                        index[current].add(m.group(1))
                c = ENUM_CONST_RE.match(line)
                if c:
                    # A payload-bearing choice case — `Ok(owned value: T),` —
                    # is a member of its choice. The comma split below cannot
                    # see it: the part is `Ok(owned value: T)`, which is not a
                    # bare identifier, so `Result.Ok` read as an unknown member.
                    index[current].add(c.group(1))
                    # A constant line lists several: `Low, Medium, High;`
                    for part in line.split("//")[0].strip(" ;").split(","):
                        name = part.strip().split(":=")[0].strip()
                        if re.match(r'^[A-Z]\w*$', name):
                            index[current].add(name)
    # The exception hierarchy is stated as prose and tables in spec §31.4 rather
    # than as one declaration block, so pick the names up wherever the documents
    # write them in backticks.
    for name in ("Bezel_Language_Spec.md", "Bezel_Standard_Library.md"):
        path = os.path.join(docs_dir, name)
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8") as f:
            text = f.read()
        for exc in re.findall(r'`([A-Z]\w*Exception)`', text):
            index.setdefault(exc, set())
    return index


def declared_in(source):
    """Type names and type parameters the case declares for itself."""
    names = set()
    for line in source.split("\n"):
        decl = TYPE_DECL_RE.match(line)
        if decl:
            names.add(decl.group(2))
        alias = ALIAS_RE.match(line)
        if alias:
            names.add(alias.group(1))
    for line in source.split("\n"):
        alias = IMPORT_ALIAS_RE.match(line)
        if alias:
            names.add(alias.group(1))
    for group in GENERIC_PARAM_RE.findall(source):
        for part in group.split(","):
            part = part.strip().replace("in ", "").replace("out ", "").strip()
            if re.match(r'^[A-Z]\w{0,2}$', part):
                names.add(part)
    return names


STRING_RE = re.compile(r'@?\$?"(?:\\.|""|[^"\\])*"')

# Appendix B.1's unconditionally reserved words. A parameter named `base` is
# how spec §32.8's own example acquired doc defect #3067, so this is a real
# class of mistake, not a hypothetical one.
RESERVED = set("""
abstract and as async await base bool break byte case catch char choice class
closed const constructor continue decimal default destructor do double elif else
end ensures enum extension extern false finally float for if import in int
interface internal invariant is lambda long method mod module move nameof new
not null or out owned override private property protected public readonly record
ref requires return sbyte sealed short static string then this throw true try
type typeof uint ulong ushort using var virtual when where while with yield
""".split())

DECL_RES = (
    re.compile(r'\bvar\s+([A-Za-z_]\w*)'),
    re.compile(r'\bconst\s+([A-Za-z_]\w*)\s*:'),
    re.compile(r'\b(?:method|property|class|record|interface|choice|enum)\s+'
               r'([A-Za-z_]\w*)'),
)
PARAM_LIST_RE = re.compile(r'\(([^()]*)\)')
PARAM_RE = re.compile(r'^\s*(?:ref\s+|out\s+owned\s+|out\s+|owned\s+|scoped\s+)?'
                      r'([A-Za-z_]\w*)\s*:')
MEMBER_DECL_RE = re.compile(
    r'\b(?:method|property|const)\s+(?:operator\s+\S+|([A-Za-z_]\w*))')


def members_declared_in(source):
    """Members the case declares for itself — including inside an `extension`
    block, which is how a case legitimately puts a new member on a documented
    type. The index cannot know those, so the case's own declarations count."""
    return {m.group(1) for m in MEMBER_DECL_RE.finditer(source) if m.group(1)}


def reserved_declarations(source):
    """Declaration-position identifiers that are unconditionally reserved."""
    found = set()
    for line in source.split("\n"):
        code = STRING_RE.sub('""', line.split("//")[0])
        for rx in DECL_RES:
            for m in rx.finditer(code):
                if m.group(1) in RESERVED:
                    found.add(m.group(1))
        for group in PARAM_LIST_RE.findall(code):
            for part in group.split(","):
                m = PARAM_RE.match(part)
                if m and m.group(1) in RESERVED:
                    found.add(m.group(1))
    return sorted(found)


def check_source(source, index):
    own_types = declared_in(source)
    known_types = own_types | PRIMITIVES | LANGUAGE | set(index)
    known_members = UNIVERSAL_MEMBERS | members_declared_in(source)
    unknown_types = set()
    unknown_members = set()
    for line in source.split("\n"):
        code = line.split("//")[0]
        # A string literal holds prose and format specifiers (`{v:F2}`), not
        # type names; a module or import line holds a dotted module path.
        code = STRING_RE.sub('""', code)
        if re.match(r'\s*(module|import)\b', code):
            continue
        for rx in TYPE_POSITION_RES:
            for m in rx.finditer(code):
                name = m.group(1)
                if name in known_types:
                    continue
                # `method Describe<T>(…)` declares a generic METHOD; the name
                # before the angle brackets is the member, not a type.
                if re.search(r'\b(?:method|property|class|record|interface|'
                             r'choice|enum)\s+' + re.escape(name) + r'\b', code):
                    continue
                # `Classify<int>(x)` is a generic method CALL; `new Array<int>()`
                # and a bare `Array<int>` annotation are types.
                if re.search(re.escape(name) + r'<[^<>]*>\s*\(', code) \
                        and not re.search(r'\bnew\s+' + re.escape(name), code):
                    continue
                unknown_types.add(name)
        for m in STATIC_USE_RE.finditer(code):
            owner, member = m.group(1), m.group(2)
            if owner in index and index[owner] \
                    and owner not in own_types \
                    and member not in index[owner] \
                    and member not in known_members:
                unknown_members.add(owner + "." + member)
    return sorted(unknown_types), sorted(unknown_members)


def main(argv, base_dir=None):
    base = base_dir or os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(os.path.dirname(base)))
    ap = argparse.ArgumentParser(prog="names.py")
    ap.add_argument("--docs", default=os.path.join(repo, "docs"))
    ap.add_argument("--case", default="")
    ap.add_argument("--strict", action="store_true",
                    help="exit 1 when anything is reported")
    args = ap.parse_args(argv)

    index = build_index(args.docs)
    module_of_type = build_module_index(args.docs)
    cases_dir = os.path.join(base, "cases")
    expects, sections, diagnostics = {}, {}, {}
    manifest = os.path.join(base, "manifest.tsv")
    if os.path.exists(manifest):
        for row in corpus.read_manifest(manifest):
            expects[row["case-id"]] = row["expect"]
            sections[row["case-id"]] = row["section"]
            diagnostics[row["case-id"]] = row["check"]
    findings = 0
    for name in sorted(os.listdir(cases_dir)):
        if not name.endswith(".bzl"):
            continue
        case_id = name[:-len(".bzl")]
        if args.case and args.case not in case_id:
            continue
        with open(os.path.join(cases_dir, name), encoding="utf-8") as f:
            source = corpus.strip_header(f.read())
        types, members = check_source(source, index)
        imports = [] if sections.get(case_id) in IMPORT_RULE_SECTIONS \
            else missing_imports(source, module_of_type)
        # A reject case may be ABOUT declaring a reserved name; only a case that
        # expects to compile has a problem here.
        reserved = [] if expects.get(case_id) == "reject" \
            else reserved_declarations(source)
        # …and a reject case may be about an unresolvable NAME, in which case the
        # unknown type is the rule under test rather than an invented API. The
        # signature is tight: the case rejects, and its `diagnostic:` — which the
        # contract requires to be the rule's own wording — names that identifier.
        if expects.get(case_id) == "reject":
            frag = diagnostics.get(case_id, "")
            types = [t for t in types if t not in frag.split()]
        if types or members or reserved or imports:
            findings += 1
            print(case_id)
            if types:
                print("  unknown type:   " + ", ".join(types))
            if members:
                print("  unknown member: " + ", ".join(members))
            if reserved:
                print("  reserved name:  " + ", ".join(reserved))
            if imports:
                print("  missing import: " + ", ".join(
                    "{} (import {};)".format(n, m) for n, m in imports))
    print("\n{} documented types indexed, {} of them behind an explicit "
          "import; {} cases with findings"
          .format(len(index), len(module_of_type), findings))
    return 1 if (args.strict and findings) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
