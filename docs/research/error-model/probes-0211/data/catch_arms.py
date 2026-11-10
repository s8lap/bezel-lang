#!/usr/bin/env python3
"""Catch-arm census for proposal 0211: every catch arm in the four program corpora,
with its pattern and a classification of its body.

Corpora and text preparation are the error-model note's (../../data/corpus_usage.py):
  rules   tests/conformance/rules/cases/**/*.bzl
  docex   <export>/tests/conformance/cases/*.bzl   (non-skip rows; exported copy)
  lit     tests/lit/**/*.bzl
  gtest   string literals of tests/**/*.cpp
Comments and string contents are blanked before scanning.

Unlike the note's regular expression, arms are found by walking each `catch` block:
an arm is `[var x:] Type [| Type ...] [when guard] => body` (the older `x: Type`
binding spelling, which some gtest programs still carry, is read too), and its body ends at the
`,` that separates arms or at the `finally` / `end` that closes the block, with
parentheses and nested `end`-closed blocks skipped. Limits: a paren-form `(if …)` or
`(case …)` is counted as closed by its `)`; a block opened inside a string hole is
not seen (strings are blanked).

Body classes:
  suppress   the body is the unit value `()`                    (spec §31.2.5)
  reraise    the body contains a bare `throw`                    (spec §31.2.4)
  convert    the body contains `throw <operand>` and no bare `throw`
  handles    anything else: handling completes in this arm

Writes <out-dir>/catch_arms.tsv (one row per arm; about 400 KB, not committed),
catch_arms_summary.json and undivided_lit_files.txt.

Usage: catch_arms.py <repo-root> <export-root> <out-dir>
"""
import collections, csv, glob, json, os, re, sys

root, export, out = sys.argv[1:4]

def blank_bezel(src):
    o = []; i, n = 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            j = src.find("\n", i); i = n if j < 0 else j; continue
        if src.startswith("/*", i):
            j = src.find("*/", i + 2); j = n if j < 0 else j + 2; o.append(" "); i = j; continue
        if c == '"':
            j = i + 1
            while j < n and src[j] != '"':
                if src[j] == "\\": j += 1
                j += 1
            o.append('""'); i = j + 1; continue
        if c == "'" and i + 2 < n:
            j = i + 1
            if src[j] == "\\": j += 1
            j += 1
            if j < n and src[j] == "'":
                o.append("''"); i = j + 1; continue
        o.append(c); i += 1
    return "".join(o)

CPP_STR = re.compile(r'R"([A-Za-z_]*)\((.*?)\)\1"|"((?:[^"\\\n]|\\.)*)"', re.S)
def cpp_literals(src):
    src = re.sub(r"^\s*//.*$", "", src, flags=re.M)
    parts = []
    for m in CPP_STR.finditer(src):
        if m.group(2) is not None: parts.append(m.group(2))
        else:
            s = m.group(3)
            parts.append(s.replace('\\n', '\n').replace('\\"', '"').replace("\\\\", "\\").replace("\\t", " "))
    return "\n".join(parts)

TOK = re.compile(r"[A-Za-z_]\w*|=>|:=|<>|[(){}\[\],;|]|\S")
OPEN_KW = {"do", "case", "try", "class", "record", "interface", "enum", "choice"}
PATTERN = re.compile(r"^(?:(?:var\s+)?(\w+)\s*:\s*)?([A-Z][\w.]*(?:<[^<>]*>)?(?:\s*\|\s*[A-Z][\w.]*(?:<[^<>]*>)?)*)\s*(when\b.*)?$", re.S)

def tokens(text, start):
    for m in TOK.finditer(text, start):
        yield m

def parse_catch(text, pos):
    """pos is just after a `catch` keyword. Returns a list of (pattern_text, body_text)."""
    arms = []
    it = tokens(text, pos)
    toks = []
    for m in it:
        toks.append(m)
        if len(toks) > 6000: break
    i = 0; n = len(toks)
    while i < n:
        # pattern: up to `=>` at paren depth 0
        j = i; par = 0; found = None
        while j < n:
            t = toks[j].group(0)
            if t in "([{": par += 1
            elif t in ")]}": par -= 1
            elif t == "=>" and par == 0: found = j; break
            elif par == 0 and t in ("finally", "end", ";"): break
            j += 1
        if found is None: break
        pat = text[toks[i].start():toks[found].start()].strip()
        # body: to `,` at depth 0, or to the closing `finally` / `end`
        k = found + 1; par = 0; depth = 0; prev = ""; closed = False
        body_start = toks[k].start() if k < n else len(text)
        body_end = body_start
        while k < n:
            t = toks[k].group(0)
            if t in "([{": par += 1
            elif t in ")]}": par -= 1
            elif par == 0:
                if t == "," and depth == 0: body_end = toks[k].start(); break
                if t == "finally" and depth == 0: body_end = toks[k].start(); closed = True; break
                if t == "end":
                    if depth == 0: body_end = toks[k].start(); closed = True; break
                    depth -= 1
                    # skip the optional closing keyword or name after `end`
                    if k + 1 < n and re.match(r"[A-Za-z_]\w*$", toks[k + 1].group(0)) and toks[k + 1].group(0) not in ("finally",):
                        nxt = toks[k + 1].group(0)
                        if nxt in ("if", "for", "while", "try", "case", "do") or nxt[0].isupper() or nxt in ("constructor", "destructor", "method", "get", "set"):
                            prev = t; k += 2; continue
                elif t in OPEN_KW and prev != "end": depth += 1
                elif t == "if" and prev not in ("end", "("): depth += 1
            elif t == "end" and False: pass
            prev = t; k += 1
        else:
            body_end = len(text)
        body = text[body_start:body_end].strip()
        arms.append((pat, body))
        if closed or k >= n: break
        i = k + 1
    return arms

BARE_THROW = re.compile(r"\bthrow\s*(?:;|,|\)|$|\bend\b)", re.M)
ANY_THROW = re.compile(r"\bthrow\b")
def body_class(body):
    b = body.strip().rstrip(";").strip()
    if re.fullmatch(r"\(\s*\)", b): return "suppress"
    if BARE_THROW.search(body + "\n"): return "reraise"
    if ANY_THROW.search(body): return "convert"
    return "handles"

def corpora():
    c = {}
    c["rules"] = [("bzl", p) for p in sorted(glob.glob(os.path.join(root, "tests/conformance/rules/cases/**/*.bzl"), recursive=True))]
    skip = set()
    man = os.path.join(export, "tests/conformance/manifest.tsv")
    if os.path.exists(man):
        for row in csv.reader(open(man, encoding="utf-8"), delimiter="\t"):
            if len(row) > 2 and any(x.startswith("skip") for x in row[1:4]): skip.add(row[0])
    c["docex"] = [("bzl", p) for p in sorted(glob.glob(os.path.join(export, "tests/conformance/cases/*.bzl")))
                  if os.path.splitext(os.path.basename(p))[0] not in skip]
    c["lit"] = [("bzl", p) for p in sorted(glob.glob(os.path.join(root, "tests/lit/**/*.bzl"), recursive=True))]
    c["gtest"] = [("cpp", p) for p in sorted(glob.glob(os.path.join(root, "tests/**/*.cpp"), recursive=True))
                  if "/conformance/" not in p and "/lit/" not in p]
    return c

rows = []
for name, files in corpora().items():
    for kind, p in files:
        try: src = open(p, encoding="utf-8", errors="replace").read()
        except OSError: continue
        raw = src
        if kind == "cpp":
            src = cpp_literals(src)
            if "module " not in src: continue
        text = blank_bezel(src)
        rel = os.path.relpath(p, export if name == "docex" else root)
        for m in re.finditer(r"\bcatch\b", text):
            arms = parse_catch(text, m.end())
            marker_seen = False
            for idx, (pat, body) in enumerate(arms):
                pm = PATTERN.match(pat)
                if not pm:
                    rows.append((name, rel, idx, len(arms), "?", "", "", "", "unparsed", "")); continue
                bind, types, guard = pm.group(1) or "", pm.group(2), pm.group(3) or ""
                tl = [re.sub(r"<.*", "", t.strip()).split(".")[-1] for t in types.split("|")]
                rows.append((name, rel, idx, len(arms), "|".join(tl), "bind" if bind else "bare",
                             "guard" if guard else "", "last" if idx == len(arms) - 1 else "",
                             body_class(body), re.sub(r"\s+", " ", body)[:80]))

os.makedirs(out, exist_ok=True)
with open(os.path.join(out, "catch_arms.tsv"), "w", encoding="utf-8") as f:
    f.write("corpus\tfile\tarm_index\tarms_in_block\ttypes\tbinding\tguard\tlast\tbody_class\tbody_head\n")
    for r in rows: f.write("\t".join(str(x) for x in r) + "\n")

# ---- summary
S = collections.OrderedDict()
by = collections.Counter(); catchall = collections.Counter(); catchall_c = collections.Counter()
for r in rows:
    name, rel, idx, nb, types, bind, guard, last, cls, head = r
    by[name] += 1
    if types == "Exception":
        catchall[(guard or "unguarded", cls)] += 1
        catchall_c[(name, guard or "unguarded", cls)] += 1
S["arms_total"] = len(rows)
S["arms_by_corpus"] = dict(by)
S["unparsed"] = sum(1 for r in rows if r[8] == "unparsed")
S["catch_all_arms"] = sum(catchall.values())
S["catch_all_by_guard_and_body"] = {f"{g}/{c}": n for (g, c), n in sorted(catchall.items())}
S["catch_all_by_corpus"] = {f"{n}/{g}/{c}": v for (n, g, c), v in sorted(catchall_c.items())}

# The proposal's undivided catch-all (section 5.4): an unguarded arm naming `Exception`
# alone whose body is not `()` and holds no bare re-raise. No corpus program names the
# marker, so the "no earlier marker arm" condition holds for every one of them.
und = [r for r in rows if r[4] == "Exception" and r[6] == "" and r[8] in ("handles", "convert")]
S["undivided_catch_all_arms"] = len(und)
S["undivided_by_corpus"] = dict(collections.Counter(r[0] for r in und))
S["undivided_sole_arm_of_its_block"] = sum(1 for r in und if r[3] == 1)
ufiles = collections.Counter((r[0], r[1]) for r in und)
S["undivided_files"] = len(ufiles)
S["undivided_files_by_corpus"] = dict(collections.Counter(k[0] for k in ufiles))
exp = collections.Counter(); nodiag = []
for (corp, f) in ufiles:
    if corp == "rules":
        src = open(os.path.join(root, f), encoding="utf-8").read()
        m = re.search(r"//! expect:\s*(\w+)", src); exp[m.group(1) if m else "?"] += 1
    if corp == "lit":
        src = open(os.path.join(root, f), encoding="utf-8").read()
        if re.search(r"NOT:\s*warning|--allow-empty|NODIAG|Werror", src): nodiag.append(f)
S["undivided_rule_cases_by_expect"] = dict(exp)
S["undivided_lit_files_asserting_no_diagnostics"] = len(nodiag)

# The ergonomics study's 40 everyday programs (final versions), as a second population.
ev = collections.Counter(); evn = 0; evc = 0
for p in sorted(glob.glob(os.path.join(root, "docs/research/ergonomics-study/programs/*/final.bzl"))):
    evn += 1
    text = blank_bezel(open(p, encoding="utf-8").read())
    had = False
    for m in re.finditer(r"\bcatch\b", text):
        had = True
        for pat, body in parse_catch(text, m.end()):
            pm = PATTERN.match(pat)
            if pm:
                for t in pm.group(2).split("|"): ev[t.strip()] += 1
    evc += had
S["everyday_programs"] = evn
S["everyday_programs_with_a_catch"] = evc
S["everyday_arm_types"] = dict(ev)

json.dump(S, open(os.path.join(out, "catch_arms_summary.json"), "w"), indent=1)
with open(os.path.join(out, "undivided_lit_files.txt"), "w") as f:
    for x in sorted(nodiag): f.write(x + "\n")
print(json.dumps(S, indent=1))
