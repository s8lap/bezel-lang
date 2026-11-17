#!/usr/bin/env python3
"""test_dup_scan.py -- find EXACT-duplicate gtest rows (#5233 Phase 1).

Stdlib-only.  Re-creation of the R25 scanner described in #5233's *Method*.

Two rows are an EXACT duplicate when, after C++ comments and layout whitespace
are removed, both of these are identical:

  * the PROGRAM  -- every string literal in the row's body, plus the string
                    literals of every file-level definition the body reaches;
  * the BODY     -- the whole `BEZEL_TEST(suite, name) { ... }` token stream
                    (assertions, exit codes, loop counts, needles, helper calls)
                    PLUS the text of every file-level helper/constant it reaches
                    (transitively).  Two files that both call `run(...)` but
                    define `run` differently are therefore NOT duplicates.

The definition of "exact" is deliberately NO LOOSER than #5233's:
  * rows that differ in any literal (an exit code, a loop count, a needle) are
    not duplicates;
  * adjacent string literals are concatenated (so `"a " "b"` equals `"a b"`),
    and a string of 40 or more characters has its whitespace runs collapsed to
    one space (programs are indented by hand); a shorter string (a needle, a
    name) is compared verbatim;
  * rows that reach `__FILE__`, `__LINE__`, `__COUNTER__` or `__func__` are
    never reported (their behaviour depends on where they sit).

Known limits (same as the R25 scanner): a program built at run time by a helper
or by `std::string` concatenation is only seen through the literals it contains;
two rows whose helpers come from DIFFERENT headers under the same name are not
distinguished (the headers are not read).

Usage:
  scripts/test_dup_scan.py                     human-readable groups on stdout
  scripts/test_dup_scan.py --json OUT.json     also write the groups as JSON
  scripts/test_dup_scan.py --exclude FILE...   treat these test files as
                                               excluded: a group is "outside the
                                               excluded set" only if NONE of its
                                               rows lives in an excluded file
  scripts/test_dup_scan.py --count-rows        print BEZEL_TEST row counts per area
Exit status: 0 when no exact group lies outside the excluded set, 1 otherwise,
2 on a scanner error.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AREAS = {
    "codegen": ["codegen"],
    "ownership": ["ownership"],
    "sema": ["sema", "flow"],
    "frontend": ["lexer", "parser"],
    "module": ["module"],
    "support": ["runtime", "support"],
}
STR_COLLAPSE_MIN = 40
NONDETERMINISTIC = {"__FILE__", "__LINE__", "__COUNTER__", "__func__"}

ESC = {"n": "\n", "t": "\t", "r": "\r", "0": "\0", "\\": "\\", '"': '"',
       "'": "'", "a": "\a", "b": "\b", "f": "\f", "v": "\v", "?": "?"}


def decode_plain(s):
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            n = s[i + 1]
            if n == "x":
                m = re.match(r"[0-9a-fA-F]+", s[i + 2:])
                if m:
                    out.append(chr(int(m.group(0), 16) & 0x10FFFF))
                    i += 2 + len(m.group(0))
                    continue
            if n in "01234567":
                m = re.match(r"[0-7]{1,3}", s[i + 1:])
                out.append(chr(int(m.group(0), 8)))
                i += 1 + len(m.group(0))
                continue
            if n in ESC:
                out.append(ESC[n])
                i += 2
                continue
            out.append(c + n)  # unknown escape: keep verbatim
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


class Tok:
    __slots__ = ("kind", "text")

    def __init__(self, kind, text):
        self.kind = kind  # id num str chr punct pp
        self.text = text

    def __repr__(self):
        return "%s:%r" % (self.kind, self.text)


NUM_RE = re.compile(r"(?:0[xX][0-9a-fA-F']+|0[bB][01']+|[0-9][0-9']*(?:\.[0-9']*)?(?:[eE][+-]?[0-9]+)?)[A-Za-z_]*|\.[0-9][0-9']*(?:[eE][+-]?[0-9]+)?[A-Za-z_]*")
ID_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
STR_PREFIX_RE = re.compile(r"(?:u8|u|U|L)?")
PUNCT3 = ("<<=", ">>=", "...", "->*", "<=>")
PUNCT2 = ("::", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
          "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", ".*")


_MASTER = re.compile(
    r"""(?P<ws>[ \t\r\f\v\n]+)
    |(?P<lc>//[^\n]*)
    |(?P<bc>/\*.*?\*/)
    |(?P<raw>(?:u8|u|U|L)?R"(?P<delim>[^()\\ \t\n]{0,16})\()
    |(?P<str>(?:u8|u|U|L)?"(?:[^"\\\n]|\\.)*")
    |(?P<chr>(?:u8|u|U|L)?'(?:[^'\\\n]|\\.)+')
    |(?P<id>[A-Za-z_][A-Za-z0-9_]*)
    |(?P<num>(?:0[xX][0-9a-fA-F']+|0[bB][01']+|[0-9][0-9']*(?:\.[0-9']*)?(?:[eE][+-]?[0-9]+)?|\.[0-9][0-9']*(?:[eE][+-]?[0-9]+)?)[A-Za-z_]*)
    |(?P<p3><<=|>>=|\.\.\.|->\*|<=>)
    |(?P<p2>::|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||\+=|-=|\*=|/=|%=|&=|\|=|\^=|\.\*)
    |(?P<p1>.)""", re.S | re.X)


def tokenize(src):
    """C++ tokens with comments dropped.  String tokens carry decoded content."""
    toks = []
    i = 0
    n = len(src)
    match = _MASTER.match
    at_line_start = True
    while i < n:
        if at_line_start:
            # preprocessor line?
            k = i
            while k < n and src[k] in " \t":
                k += 1
            if k < n and src[k] == "#":
                j = k
                while True:
                    e = src.find("\n", j)
                    if e < 0:
                        j = n
                        break
                    if src[e - 1] == "\\":
                        j = e + 1
                        continue
                    j = e
                    break
                text = re.sub(r"/\*.*?\*/", " ", src[k:j], flags=re.S)
                text = re.sub(r"//.*", "", text)
                toks.append(Tok("pp", " ".join(text.split())))
                i = j
                at_line_start = False
                continue
            at_line_start = False
        m = match(src, i)
        kind = m.lastgroup
        if kind == "delim":
            kind = "raw"
        if kind == "ws":
            if "\n" in m.group(0):
                at_line_start = True
            i = m.end()
            continue
        if kind in ("lc", "bc"):
            if kind == "bc" and "\n" in m.group(0):
                at_line_start = False
            i = m.end()
            continue
        if kind == "raw":
            delim = m.group("delim")
            end = src.find(")" + delim + '"', m.end())
            if end < 0:
                raise ValueError("unterminated raw string")
            toks.append(Tok("str", src[m.end():end]))
            i = end + len(delim) + 2
            continue
        text = m.group(0)
        if kind == "str":
            q = text.index('"')
            toks.append(Tok("str", decode_plain(text[q + 1:-1])))
        elif kind == "chr":
            toks.append(Tok("chr", text))
        elif kind == "id":
            toks.append(Tok("id", text))
        elif kind == "num":
            toks.append(Tok("num", text))
        else:
            toks.append(Tok("punct", text))
        i = m.end()
    return toks


def merge_strings(toks):
    out = []
    for t in toks:
        if t.kind == "str" and out and out[-1].kind == "str":
            out[-1] = Tok("str", out[-1].text + t.text)
        else:
            out.append(t)
    return out


def norm_str(s):
    if len(s) >= STR_COLLAPSE_MIN:
        return " ".join(s.split())
    return s


def render(toks):
    """Canonical text of a token list (strings are escaped to one line)."""
    parts = []
    for t in merge_strings(toks):
        if t.kind == "str":
            parts.append("S" + json.dumps(norm_str(t.text)))
        else:
            parts.append(t.text)
    return " ".join(parts)


def programs(toks):
    return [norm_str(t.text) for t in merge_strings(toks) if t.kind == "str"]


def match_brace(toks, i):
    """toks[i] is '{'; return the index of its matching '}'."""
    depth = 0
    for j in range(i, len(toks)):
        if toks[j].kind == "punct":
            if toks[j].text == "{":
                depth += 1
            elif toks[j].text == "}":
                depth -= 1
                if depth == 0:
                    return j
    raise ValueError("unbalanced braces")


class FileScan:
    def __init__(self, path):
        self.path = path
        with open(os.path.join(ROOT, path), encoding="utf-8", errors="replace") as f:
            toks = tokenize(f.read())
        self.tests = []   # (suite, name, body_toks)
        self.defs = defaultdict(list)  # name -> [token lists]
        self.includes = [m.group(1) for t in toks if t.kind == "pp"
                         for m in [re.match(r'#\s*include\s*"([^"]+)"', t.text)] if m]
        self._split(toks)
        self.hdr_defs = None  # name -> token lists, from reachable headers

    def lookup(self, name):
        d = self.defs.get(name)
        if d:
            return d
        if self.hdr_defs is None:
            self.hdr_defs = header_defs(self.path)
        return self.hdr_defs.get(name, ())

    def _split(self, toks):
        i = 0
        n = len(toks)
        stmt = []
        while i < n:
            t = toks[i]
            if t.kind == "pp":
                i += 1
                m = re.match(r"#\s*define\s+([A-Za-z_]\w*)", t.text)
                if m:
                    self.defs[m.group(1)].append([t])
                continue
            if (t.kind == "id" and t.text == "BEZEL_TEST" and not stmt
                    and i + 1 < n and toks[i + 1].text == "("):
                j = i + 2
                depth = 1
                args = []
                cur = []
                while depth:
                    x = toks[j]
                    if x.text == "(" and x.kind == "punct":
                        depth += 1
                    elif x.text == ")" and x.kind == "punct":
                        depth -= 1
                        if depth == 0:
                            break
                    if depth == 1 and x.text == "," and x.kind == "punct":
                        args.append(cur)
                        cur = []
                    else:
                        cur.append(x)
                    j += 1
                args.append(cur)
                while toks[j].text != "{":
                    j += 1
                e = match_brace(toks, j)
                suite = "".join(a.text for a in args[0])
                name = "".join(a.text for a in args[1])
                self.tests.append((suite, name, toks[j:e + 1]))
                i = e + 1
                continue
            if t.kind == "punct" and t.text == "{":
                head = stmt
                e = match_brace(toks, i)
                is_ns = bool(head) and head[0].text in ("namespace", "extern")
                if is_ns:
                    stmt = []
                    # descend: namespace contents are scanned in place
                    self._split(toks[i + 1:e])
                    i = e + 1
                    continue
                # function body / struct body / brace-init: part of this stmt
                stmt = stmt + toks[i:e + 1]
                i = e + 1
                # a function / struct definition ends at the '}' (struct has ';')
                nxt = toks[i] if i < n else None
                is_func_like = any(x.text == "(" for x in head) and not any(
                    x.text == "=" for x in head)
                if nxt is not None and nxt.text == ";":
                    stmt.append(nxt)
                    i += 1
                    self._commit(stmt)
                    stmt = []
                elif is_func_like or (head and head[0].text in ("struct", "class", "enum", "union")):
                    self._commit(stmt)
                    stmt = []
                continue
            if t.kind == "punct" and t.text == ";":
                stmt.append(t)
                self._commit(stmt)
                stmt = []
                i += 1
                continue
            stmt.append(t)
            i += 1
        if stmt:
            self._commit(stmt)

    def _commit(self, stmt):
        if not stmt:
            return
        names = set()
        head = []
        for x in stmt:
            if x.kind == "punct" and x.text in ("{", "=", ";"):
                break
            head.append(x)
        for k, x in enumerate(head):
            if x.kind != "id":
                continue
            nxt = head[k + 1].text if k + 1 < len(head) else ""
            prev = head[k - 1].text if k else ""
            if nxt == "(" or k == len(head) - 1 or prev in ("struct", "class", "enum", "union", "using", "typedef"):
                names.add(x.text)
        if not names and head:
            for x in reversed(head):
                if x.kind == "id":
                    names.add(x.text)
                    break
        # `using bezel::test::runAot;` -> runAot ; `using A = B;` -> A
        for nm in names:
            self.defs[nm].append(stmt)
        # also register every declarator of `int a = 1, b = 2;` cheaply: skip.


_HDR_CACHE = {}


def _resolve_include(inc, from_path):
    for base in (os.path.dirname(from_path), "tests", "tests/support"):
        cand = os.path.normpath(os.path.join(base, inc))
        if cand.startswith("tests/") and os.path.isfile(os.path.join(ROOT, cand)):
            return cand
    return None  # system / src header: not a test helper, identical for every row


def _header(path):
    if path not in _HDR_CACHE:
        fs = FileScan(path)
        _HDR_CACHE[path] = fs
    return _HDR_CACHE[path]


def header_defs(path):
    """name -> definitions, over every test header reachable from `path`."""
    out = defaultdict(list)
    seen = set()
    work = [path]
    while work:
        cur = work.pop()
        fs = _header(cur) if cur != path else _top(cur)
        for inc in fs.includes:
            r = _resolve_include(inc, cur)
            if r is None or r in seen:
                continue
            seen.add(r)
            h = _header(r)
            for nm, ds in h.defs.items():
                out[nm].extend(ds)
            work.append(r)
    return out


_TOP = {}


def _top(path):
    return _TOP[path]


def closure(fs, body):
    """Texts of every file-level definition reachable from `body`."""
    seen = set()
    texts = []
    work = [t.text for t in body if t.kind == "id"]
    allidents = set(work)
    while work:
        nm = work.pop()
        if nm in seen:
            continue
        seen.add(nm)
        for d in fs.lookup(nm):
            texts.append((nm, d))
            for t in d:
                if t.kind == "id" and t.text not in seen:
                    work.append(t.text)
                    allidents.add(t.text)
    return texts, allidents


def row_key(fs, body):
    texts, idents = closure(fs, body)
    if idents & NONDETERMINISTIC:
        return None
    ctext = sorted(set((nm, render(d)) for nm, d in texts))
    ctoks = [t for _, d in texts for t in d]
    prog = programs(body) + sorted(set(programs(ctoks)))
    body_text = render(body)
    h_prog = hashlib.sha256(json.dumps(prog).encode()).hexdigest()
    h_body = hashlib.sha256(json.dumps([body_text, ctext]).encode()).hexdigest()
    return h_prog, h_body, sum(len(p) for p in prog)


def test_files():
    out = []
    for areadirs in AREAS.values():
        for d in areadirs:
            p = os.path.join(ROOT, "tests", d)
            if not os.path.isdir(p):
                continue
            for f in sorted(os.listdir(p)):
                if f.endswith(".cpp"):
                    out.append("tests/%s/%s" % (d, f))
    for f in sorted(os.listdir(os.path.join(ROOT, "tests"))):
        if f.endswith(".cpp") and f != "test_main.cpp":
            out.append("tests/" + f)
    return out


def area_of(path):
    d = path.split("/")[1]
    for a, dirs in AREAS.items():
        if d in dirs:
            return a
    return "support"


def first_added(path):  # only used for files with no issue number in the name
    """Commit timestamp of the commit that added the file (oldest first)."""
    try:
        r = subprocess.run(
            ["git", "log", "--diff-filter=A", "--follow", "--format=%ct", "--", path],
            cwd=ROOT, capture_output=True, text=True, check=True)
        ts = [int(x) for x in r.stdout.split()]
        return min(ts) if ts else 1 << 60
    except Exception:
        return 1 << 60


def issue_num(path):
    """Lowest issue number in the file name, or None."""
    nums = [int(x) for x in re.findall(r"(?<![0-9])([0-9]{2,5})(?![0-9])", os.path.basename(path))]
    return min(nums) if nums else None


def keep_order_key(path, ts_cache):
    inum = issue_num(path)
    if inum is not None:
        return (0, inum, 0, path)
    if path not in ts_cache:
        ts_cache[path] = first_added(path)
    return (1, 0, ts_cache[path], path)


def scan():
    rows = []
    counts = defaultdict(int)
    for path in test_files():
        fs = FileScan(path)
        _TOP[path] = fs
        counts[area_of(path)] += len(fs.tests)
        for idx, (suite, name, body) in enumerate(fs.tests):
            k = row_key(fs, body)
            if k is None:
                continue
            rows.append({"file": path, "suite": suite, "name": name, "idx": idx,
                         "prog": k[0], "body": k[1], "plen": k[2], "area": area_of(path)})
    return rows, counts


def group(rows):
    g = defaultdict(list)
    for r in rows:
        g[(r["prog"], r["body"])].append(r)
    return [v for v in g.values() if len(v) > 1]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--json")
    ap.add_argument("--exclude", nargs="*", default=[], help="test files to treat as excluded")
    ap.add_argument("--exclude-from", help="file with one excluded test path per line")
    ap.add_argument("--count-rows", action="store_true")
    ap.add_argument("--min-program", type=int, default=0)
    a = ap.parse_args()
    excluded = set(a.exclude)
    if a.exclude_from:
        with open(a.exclude_from) as f:
            excluded |= {x.strip() for x in f if x.strip()}
    rows, counts = scan()
    if a.count_rows:
        for k in AREAS:
            print("%-10s %d" % (k, counts.get(k, 0)))
        print("%-10s %d" % ("total", sum(counts.values())))
    groups = group(rows)
    ts_cache = {}
    result = []
    for g in groups:
        g.sort(key=lambda r: (keep_order_key(r["file"], ts_cache), r["idx"]))
        # same lowest issue number in two files: the file added first wins
        top = keep_order_key(g[0]["file"], ts_cache)[:2]
        tied = [r for r in g if keep_order_key(r["file"], ts_cache)[:2] == top]
        if len({r["file"] for r in tied}) > 1:
            for r in tied:
                if r["file"] not in ts_cache:
                    ts_cache[r["file"]] = first_added(r["file"])
            tied.sort(key=lambda r: (ts_cache[r["file"]], r["file"], r["idx"]))
            g[:len(tied)] = tied
        deferred = any(r["file"] in excluded for r in g)
        result.append({"keep": g[0], "delete": g[1:], "deferred": deferred})
    result.sort(key=lambda x: (x["keep"]["file"], x["keep"]["name"]))
    live = [x for x in result if not x["deferred"]]
    print("exact groups: %d (rows redundant: %d); outside the excluded set: %d (rows redundant: %d); deferred: %d"
          % (len(result), sum(len(x["delete"]) for x in result),
             len(live), sum(len(x["delete"]) for x in live), len(result) - len(live)))
    for i, x in enumerate(result, 1):
        k = x["keep"]
        print("%d.%s KEEP %s :: %s.%s" % (i, " [DEFERRED]" if x["deferred"] else "", k["file"], k["suite"], k["name"]))
        for d in x["delete"]:
            print("     drop %s :: %s.%s" % (d["file"], d["suite"], d["name"]))
    if a.json:
        with open(a.json, "w") as f:
            json.dump({"counts": counts, "groups": result}, f, indent=1)
    return 1 if live else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:  # noqa
        print("test_dup_scan: error: %r" % (e,), file=sys.stderr)
        sys.exit(2)
