#!/usr/bin/env python3
"""builtin_admission_grid.py — the campaign-2 (builtin-call admission) DONE instrument.

WHAT IT IS
    A grid of generated Bezel programs.  Each cell is one built-in call reached
    with a receiver kind, an argument kind and a spelling, and each cell states
    what the compiler MUST do with it (accept with this exact output, reject
    with a located error, raise this fault, or at minimum not abort the
    compiler).  The grid is the campaign's DONE instrument: §2.3 of the master
    plan wants it green under ASan at every gate afterwards.

SHAPE
    Deliberately the shape of `scripts/regression_matrix.py`: a deterministic
    generator, one generated program per cell, a compile+run leg per cell, a
    per-cell CLASS, a TSV of every cell, and a non-zero exit when a cell is
    wrong.  Cells are named so a row can be cited in a commit body.

USAGE
    scripts/builtin_admission_grid.py --out DIR [--bezelc build/bezelc]
                                      [--jobs N] [--filter SUBSTR] [--list]

    BEZEL_AOT_FORCE_SANITIZE=address  compiles every cell with --sanitize=address
                                      (the campaign's DONE leg).  The script prints
                                      `NATIVE SANITIZER: N check(s) skipped` — N must be 0.

EXIT STATUS
    0  every selected cell matched its expectation
    1  at least one cell is wrong (the campaign's baseline number)
    2  usage / infrastructure error

WHAT A CELL'S CLASS MEANS
    OK          the cell matched its expectation
    ACCEPTED    the program compiled and ran, but not as the expectation says
    REJECTED    the program was rejected, but not as the expectation says
    ABORT       the compiler aborted (IR verification failed, an `internal:`
                degrade, a signal, a timeout) — the worst class
    INTERNAL    the program compiled with an `internal:` note — a silent
                acceptance inversion (campaign 0(a): every `internal:` degrade
                site is to become Severity::Error)
"""
import argparse
import concurrent.futures
import os
import re
import subprocess
import sys



CELLS = []

def C(name, body, expect, pre="", extra="", imports="", campaign="2"):
    """`expect` is (kind, want):
         ("value", TEXT)  the program must compile, run, and print exactly TEXT
         ("reject", NEEDLE) the program must be REJECTED with a located ERROR whose
                          message contains NEEDLE (a str, or a list of acceptable
                          substrings).  A reject cell MUST name its needle.
         ("fault", FRAG)  the program must compile, run, and the output must contain FRAG
         ("noabort", None) the compiler must not abort AND the program must run to
                          completion (exit code 0)
       `campaign` names the campaign that owns the cell: "2" (this campaign) or "5"
       (the nullable-carrier unwrap, which is campaign 5's mechanism, not an
       admission gate's).
    """
    if expect[0] == "reject" and not expect[1]:
        raise ValueError(f'{name}: a reject cell must name the diagnostic it requires')
    CELLS.append((name, extra, pre, body, expect, imports, campaign))

def prog(extra, pre, body, imports=""):
    out = "module M;\n"
    if imports:
        out += imports.rstrip("\n") + "\n"
    if extra:
        out += extra.rstrip("\n") + "\n"
    out += "class C\n  public static method Main(): int\n"
    for l in (pre + "\n" + body).split("\n"):
        if l.strip():
            out += "    " + l.strip() + "\n"
    out += "    return 0;\n  end Main;\nend C;\n"
    return out

# ---------------------------------------------------------------- B1 parse


# ---------------------------------------------------------------- B1 parse
C("int_parse_string", 'Console.WriteLine(int.Parse("16"));', ("value", "16\n"))
C("int_parse_int", 'Console.WriteLine(int.Parse(16));', ("reject", "no matching overload of `int.Parse`"))
C("int_parse_bool", 'Console.WriteLine(int.Parse(true));', ("reject", "no matching overload of `int.Parse`"))
C("int_parse_char", "Console.WriteLine(int.Parse('a'));", ("reject", "no matching overload of `int.Parse`"))
C("int_parse_double", 'Console.WriteLine(int.Parse(1.5));', ("reject", "no matching overload of `int.Parse`"))
C("int_parse_object", 'Console.WriteLine(int.Parse(o));', ("reject", "no matching overload of `int.Parse`"),
  pre="var o: Object := 1;")
C("int_parse_radix_ok", 'Console.WriteLine(int.Parse("ff", 16));', ("value", "255\n"))
C("int_parse_radix_uint", 'Console.WriteLine(int.Parse("ff", 16u));', ("reject", "no matching overload of `int.Parse`"))
C("int_parse_radix_string", 'Console.WriteLine(int.Parse("ff", "16"));', ("reject", "no matching overload of `int.Parse`"))
C("int_parse_radix_bool", 'Console.WriteLine(int.Parse("ff", true));', ("reject", "no matching overload of `int.Parse`"))
C("int_tryparse_string",
  'Console.WriteLine(int.TryParse("16", out v));\nConsole.WriteLine(v);',
  ("value", "True\n16\n"), pre="var v: int := 0;")
C("int_tryparse_int", '_ := int.TryParse(16, out v);', ("reject", "no matching overload of `int.TryParse`"),
  pre="var v: int := 0;")
C("int_tryparse_double", '_ := int.TryParse(1.5, out v);', ("reject", "no matching overload of `int.TryParse`"),
  pre="var v: int := 0;")
C("int_tryparse_nullable", '_ := int.TryParse(s, out v);', ("reject", "no matching overload of `int.TryParse`"),
  pre="var v: int := 0;\nvar s: string? := \"16\";")
C("double_parse_string", 'Console.WriteLine(double.Parse("1.5"));', ("value", "1.5\n"))
C("double_parse_int", 'Console.WriteLine(double.Parse(1));', ("reject", "no matching overload of `double.Parse`"))
C("double_tryparse_int", '_ := double.TryParse(1, out v);', ("reject", "no matching overload of `double.TryParse`"),
  pre="var v: double := 0.0;")
C("decimal_parse_string", 'Console.WriteLine(decimal.Parse("1.5"));', ("value", "1.5\n"))
C("decimal_parse_double", 'Console.WriteLine(decimal.Parse(1.5));', ("reject", "no matching overload of `decimal.Parse`"))
C("decimal_tryparse_double", '_ := decimal.TryParse(1.5, out v);', ("reject", "no matching overload of `decimal.TryParse`"),
  pre="var v: decimal := 0m;")
C("bool_parse_string", 'Console.WriteLine(bool.Parse("true"));', ("value", "True\n"))
C("bool_parse_int", 'Console.WriteLine(bool.Parse(1));', ("reject", "no matching overload of `bool.Parse`"))
C("bool_tryparse_int", '_ := bool.TryParse(1, out v);', ("reject", "no matching overload of `bool.TryParse`"),
  pre="var v: bool := false;")
C("char_parse_string", "Console.WriteLine(char.Parse(\"a\"));", ("value", "a\n"))
C("char_parse_radix", "Console.WriteLine(char.Parse(\"a\", 16));", ("reject", "no matching overload of `char.Parse`"))
C("char_tryparse_int", "_ := char.TryParse(1, out v);", ("reject", "no matching overload of `char.TryParse`"),
  pre="var v: char := 'x';")
C("guid_tryparse_string",
  'Console.WriteLine(Guid.TryParse("00000000-0000-0000-0000-000000000000", out g));',
  ("value", "True\n"), pre="var g: Guid := Guid.Empty;")
C("guid_tryparse_int", '_ := Guid.TryParse(1, out g);', ("reject", "no matching overload of `Guid.TryParse`"),
  pre="var g: Guid := Guid.Empty;")
C("long_parse_string", 'Console.WriteLine(long.Parse("16"));', ("value", "16\n"))
C("long_parse_intarg", 'Console.WriteLine(long.Parse(16));', ("reject", "no matching overload of `long.Parse`"))

# ------------------------------------------------------- B2 ToString/format
C("int_tostring_radix", 'Console.WriteLine((255).ToString(16));', ("value", "ff\n"))
C("int_tostring_radix_uint", 'Console.WriteLine((255).ToString(16u));',
  ("reject", "not implicitly convertible to the target type"))
C("int_tostring_radix_null", 'Console.WriteLine((255).ToString(null));',
  ("reject", "is nullable but the target type is non-nullable"))
# stdlib §2.18: "16" is not in the integer vocabulary (D/Dn/X/Xn/B), so the call is
# ADMITTED and the format is unrecognized at RUN time -> FormatException.
C("int_tostring_radix_string", 'Console.WriteLine((255).ToString("16"));', ("fault", "FormatException"))
C("long_tostring_radix", 'Console.WriteLine((255L).ToString(16));', ("value", "ff\n"))
C("long_tostring_null", 'Console.WriteLine((255L).ToString(null));',
  ("reject", "is nullable but the target type is non-nullable"))
C("int_tostring_format_d", 'Console.WriteLine((255).ToString("D"));', ("value", "255\n"))
C("int_tostring_format_x", 'Console.WriteLine((255).ToString("x"));', ("value", "ff\n"))
C("int_tostring_format_X", 'Console.WriteLine((255).ToString("X"));', ("value", "FF\n"))
C("bool_tostring_format", 'Console.WriteLine((true).ToString("F4"));', ("reject", "no matching overload of"))
C("char_tostring_format", "Console.WriteLine(('a').ToString(\"F4\"));", ("reject", "no matching overload of"))
C("double_tostring_format_c", 'Console.WriteLine((1.5).ToString("C"));', ("fault", "FormatException"))
C("double_tostring_format_d", 'Console.WriteLine((3.5).ToString("D"));', ("fault", "FormatException"))
C("double_tostring_format_bad", 'Console.WriteLine((1.5).ToString("Q"));', ("fault", "FormatException"))
C("string_tostring_format_bad", 'Console.WriteLine("x".ToString("Q"));',
  ("reject", "supplies 1 argument(s) but stdlib §9.6 declares"))
C("decimal_tostring_radix", 'Console.WriteLine((15m).ToString(16));', ("reject", "no matching overload of"))
C("decimal_tostring_format", 'Console.WriteLine((1.5m).ToString("F2"));', ("value", "1.50\n"))
C("convert_tostring_int", 'Console.WriteLine(Convert.ToString(255));', ("value", "255\n"))
C("convert_tostring_radix", 'Console.WriteLine(Convert.ToString(255, 16));', ("value", "ff\n"))
C("convert_tostring_radix_long", 'Console.WriteLine(Convert.ToString(255, lv));',
  ("reject", "no matching overload of `Convert.ToString`"), pre="var lv: long := 16;")
C("convert_tostring_char", "Console.WriteLine(Convert.ToString('A'));",
  ("reject", "ambiguous call to `Convert.ToString`"))
C("convert_tostring_uint", 'Console.WriteLine(Convert.ToString(255U));',
  ("reject", "ambiguous call to `Convert.ToString`"))
C("convert_tostring_double", 'Console.WriteLine(Convert.ToString(1.5));', ("value", "1.5\n"))

# ------------------------------------------------------------- B3 Math
C("math_max_uint", 'Console.WriteLine(Math.Max(3u, 4000000000u));', ("value", "4000000000\n"))
C("math_min_char", "Console.WriteLine(Math.Min('a', 'b'));", ("value", "a\n"))
C("math_max_int", 'Console.WriteLine(Math.Max(1, 2));', ("value", "2\n"))
C("math_abs_double", 'Console.WriteLine(Math.Abs(-1.5));', ("value", "1.5\n"))
C("math_abs_decimal", 'Console.WriteLine(Math.Abs(-1.5m));', ("value", "1.5\n"))
C("math_abs_decimal_nullable", 'Console.WriteLine(Math.Abs(dn));', ("reject", "no matching overload of"),
  pre="var dn: decimal? := 1.5m;")
C("math_abs_uint", 'Console.WriteLine(Math.Abs(4000000000u));', ("reject", "ambiguous call to `Math.Abs`"))
C("math_round_decimal", 'Console.WriteLine(Math.Round(1.5m));', ("reject", "no matching overload of `Math.Round`"))
C("math_sqrt_uint", 'Console.WriteLine(Math.Sqrt(4000000000u));', ("value", "63245.553203367585\n"))
C("math_floor_double", 'Console.WriteLine(Math.Floor(2.5));', ("value", "2\n"))
C("math_floor_nullable", 'Console.WriteLine(Math.Floor(dn));', ("reject", "no matching overload of"),
  pre="var dn: double? := 2.5;")
# A narrowed `double?` is LEGAL (§7.4) and §5.1 widens it to `double`; the call
# must therefore COMPILE AND RUN.  It cannot: the carrier reaches the Math arm.
# That is the UNWRAP (campaign 5's mechanism), not an admission gate's, so the
# cell is tagged campaign "5".
C("math_floor_narrowed",
  'if d <> null then Console.WriteLine(Math.Floor(d)); end if;',
  ("value", "2\n"), pre="var d: double? := 2.5;", campaign="5")
C("math_sign_string", 'Console.WriteLine(Math.Sign("x"));', ("reject", "no matching overload of"))
C("math_sign_bool", 'Console.WriteLine(Math.Sign(true));', ("reject", "no matching overload of"))
C("math_sign_double", 'Console.WriteLine(Math.Sign(-1.5));', ("value", "-1\n"))
C("math_max_mixed", 'Console.WriteLine(Math.Max(1, 2L));', ("reject", "inconsistent type-argument inference"))
C("math_minmax_typed_result", 'var r: string := Math.Max(1, 2);', ("reject", "the initializer for"))

# --------------------------------------------------------- B4 StringBuilder / string
C("sb_append_string", '_ := sb.Append("a");\nConsole.WriteLine(sb.ToString());',
  ("value", "a\n"), pre="var sb := new StringBuilder();")
C("sb_append_int", '_ := sb.Append(1);\nConsole.WriteLine(sb.ToString());',
  ("value", "1\n"), pre="var sb := new StringBuilder();")
C("sb_append_null", '_ := sb.Append(null);', ("reject", "is nullable but the target type is non-nullable"),
  pre="var sb := new StringBuilder();")
C("sb_replace_int", '_ := sb.Replace(1, 2);', ("reject", "not implicitly convertible to the target type"),
  pre="var sb := new StringBuilder();")
C("sb_insert_two_strings", '_ := sb.Insert("x", "y");',
  ("reject", "not implicitly convertible to the target type"), pre="var sb := new StringBuilder();")
C("sb_insert_zero", '_ := sb.Insert(0);', ("reject", "supplies 1 argument(s) but stdlib §3.1 declares"),
  pre="var sb := new StringBuilder();")
C("sb_remove_two_args", '_ := sb.Remove("a", 1);',
  ("reject", "not implicitly convertible to the target type"), pre="var sb := new StringBuilder();")
C("sb_replace_two_strings", '_ := sb.Replace("a", "b");\nConsole.WriteLine(sb.ToString());',
  ("value", "\n"), pre="var sb := new StringBuilder();")
C("sb_appendformat_string", '_ := sb.AppendFormat("x", a);', ("noabort", None),
  pre="var sb := new StringBuilder();\nusing var a := new Array<Object>();")
C("string_contains_int", 'Console.WriteLine("abc".Contains(1));', ("reject", "no matching overload of"))
C("string_contains_string", 'Console.WriteLine("abc".Contains("b"));', ("value", "True\n"))
C("string_contains_object", 'Console.WriteLine("abc".Contains(o));', ("reject", "no matching overload of"),
  pre="var o: Object := \"b\";")
C("string_split_int", 'Console.WriteLine("abc".Split(1));', ("reject", "no matching overload of"))
C("string_lastindexof_int", 'Console.WriteLine("abc".LastIndexOf(1));', ("reject", "no matching overload of"))
C("string_replace_object", 'Console.WriteLine("abc".Replace(o, "x"));', ("reject", "no matching overload of"),
  pre="var o: Object := \"b\";")
C("string_repeat_char", "Console.WriteLine(string.Repeat('a', 3));", ("value", "aaa\n"))
C("string_repeat_string", 'Console.WriteLine(string.Repeat("ab", 3));', ("value", "ababab\n"))
# stdlib §9.6: "a `startIndex` above `Length` ... raises IndexOutOfRangeException".
C("string_remove_oob", 'Console.WriteLine("ab".Remove(253).Length);', ("fault", "IndexOutOfRangeException"))
C("string_equals_named_reordered",
  'Console.WriteLine("ABCDEF".Equals(ignoreCase := true, other := "abcdef"));', ("value", "True\n"))
C("string_equals_named_ordered",
  'Console.WriteLine("ABCDEF".Equals(other := "abcdef", ignoreCase := true));', ("value", "True\n"))
C("string_compareto_named_reordered",
  'Console.WriteLine("ABCDEF".CompareTo(ignoreCase := true, other := "abcdef"));', ("value", "0\n"))
C("string_join_int_array", 'Console.WriteLine(string.Join(", ", a));', ("reject", "no matching overload of"),
  pre="using var a := new Array<int>();")
# stdlib §9.6 declares `public pure readonly property IsAscii: bool` -- a PROPERTY.
# The call spelling has no candidate (§22.6); the property read is the legal one.
C("string_isascii", 'Console.WriteLine("abc".IsAscii());', ("reject", "no matching overload of"))
C("string_isascii_property", 'Console.WriteLine("abc".IsAscii);', ("value", "True\n"))
C("debug_writeline_stringbuilder", '_ := sb.Append("x");\nDebug.WriteLine(sb);',
  ("reject", "not implicitly convertible to the target type"), pre="var sb := new StringBuilder();",
  imports="import System.Diagnostics;")

# ------------------------------------------------------- B5 nullable carrier (campaign 5)
C("sb_append_narrowed_int", 'if n <> null then _ := sb.Append(n); end if;',
  ("noabort", None), pre="var sb := new StringBuilder();\nvar n: int? := 1;", campaign="5")
C("int_tostring_radix_narrowed", 'if n <> null then Console.WriteLine(n.ToString(16)); end if;',
  ("value", "ff\n"), pre="var n: int? := 255;", campaign="5")
C("int_parse_from_narrowed", 'if n <> null then Console.WriteLine(int.Parse(n.ToString())); end if;',
  ("value", "1\n"), pre="var n: int? := 1;", campaign="5")
C("math_floor_nullable_null", 'Console.WriteLine(Math.Floor(dnull));', ("reject", "no matching overload of"),
  pre="var dnull: double? := null;")

# ------------------------------------------------------- B6 null at a parameter
C("sb_append_null_literal", '_ := sb.Append(null);', ("reject", "is nullable but the target type is non-nullable"),
  pre="var sb := new StringBuilder();")
C("string_equals_null", 'Console.WriteLine("a".Equals(null));', ("value", "False\n"))
C("datetime_tostring_null", 'Console.WriteLine(DateTime.Now.ToString(null));',
  ("reject", "is nullable but the target type is non-nullable"))

# --------------------------------------------- receiver kinds (symbol vs name)
C("user_class_named_stringbuilder",
  '_ := sb.Replace("l", "L");\nConsole.WriteLine("after");',
  ("value", "USER-REPLACE\nafter\n"), pre="var sb: StringBuilder := new StringBuilder();",
  extra=("class StringBuilder\n"
         "  public method Replace(a: string, b: string): StringBuilder\n"
         "    Console.WriteLine(\"USER-REPLACE\");\n"
         "    return this;\n"
         "  end Replace;\n"
         "end StringBuilder;"))
C("object_holding_stringbuilder", 'Console.WriteLine(o is StringBuilder);',
  ("value", "True\n"), pre="var sb := new StringBuilder();\nvar o: Object := sb;")
C("encoding_getstring_int_array", 'Console.WriteLine(Encoding.UTF8.GetString(a));',
  ("reject", "expects `Array<byte>`"), pre="using var a := new Array<int>();")
C("encoding_getstring_byte_array", 'a.Add(65);\nConsole.WriteLine(Encoding.UTF8.GetString(a));',
  ("value", "A\n"), pre="using var a := new Array<byte>();")
C("builtin_typearg_list", '_ := Console.WriteLine<int>("x");', ("reject", "type argument"))
C("builtin_typearg_arity", '_ := Console.WriteLine<int, int>("x");', ("reject", "type argument"))

# ------------------------------------------------------- method-reference face
C("math_max_reference", 'var g := Math.Max;\nConsole.WriteLine(g(1, 2));',
  ("reject", "is a reference to a generic method"))
C("math_sign_reference", 'var g := Math.Sign;', ("reject", "is a reference to a generic method"))

# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

DIAG = re.compile(r'^(?P<file>[^\n:]*?):(?P<line>\d+):(?P<col>\d+):\s*'
                   r'(?:(?P<sev>error|warning|note):\s*)?(?P<msg>.*)$', re.M)
INTERNAL = re.compile(r'\binternal:', re.I)
ABORT_PAT = re.compile(r'IR verification failed|codegen error|Assertion|Aborted|Segmentation fault|'
                       r'AddressSanitizer|SUMMARY: |stack dump|out of memory', re.I)
# A compiler abort is an ORACLE: the sanitizer's own report is one, and so is a
# signal (the exit status is negative when the compiler was killed by one).
SIGNAL_RC = -11

CLASSES = ('OK', 'ACCEPTED', 'REJECTED', 'ABORT', 'INTERNAL')


def _needle_hit(msg, want):
    """True when the diagnostic contains the cell's needle.  `want` may be a
    single substring or a list of acceptable substrings (any one suffices)."""
    if want is None:
        return False
    needles = [want] if isinstance(want, str) else list(want)
    return any(n in msg for n in needles)


def classify_one(bezelc, progdir, outdir, cell, asan, timeout, run_timeout):
    name, extra, pre, body, expect, imports, campaign = cell
    kind, want = expect
    src = prog(extra, pre, body, imports)
    os.makedirs(progdir, exist_ok=True)
    os.makedirs(outdir, exist_ok=True)
    path = os.path.join(progdir, name + '.bzl')
    with open(path, 'w') as f:
        f.write(src)
    exe = os.path.join(outdir, name + '.exe')
    cmd = [bezelc, '-o', exe]
    if asan:
        cmd.append('--sanitize=address')
    cmd.append(path)
    try:
        c = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=timeout)
    except subprocess.TimeoutExpired:
        return (name, campaign, kind, want, 'ABORT', 'compile timeout', '')
    err = (c.stdout or '') + (c.stderr or '')
    diags = [d.groupdict() for d in DIAG.finditer(err)]
    errors = [d for d in diags if d['sev'] in (None, 'error')]
    warns = [d for d in diags if d['sev'] in ('warning', 'note')]
    internal = bool(INTERNAL.search(err))
    # The detail column carries a CELL-RELATIVE path, so two runs into different
    # --out directories produce byte-identical TSVs.
    rel = os.path.join('programs', name + '.bzl')
    if c.returncode != 0:
        if c.returncode == SIGNAL_RC or ABORT_PAT.search(err) or (not errors and internal):
            return (name, campaign, kind, want, 'ABORT', 'compiler abort: ' + err[-300:].replace(path, rel), rel)
        if errors:
            first = f"{errors[0]['file']}:{errors[0]['line']}:{errors[0]['col']}: error: {errors[0]['msg']}"
            first = first.replace(path, rel)
            if kind == 'reject':
                if _needle_hit(first, want):
                    return (name, campaign, kind, want, 'OK', first, rel)
                return (name, campaign, kind, want, 'REJECTED', first, rel)
            return (name, campaign, kind, want, 'REJECTED', first, rel)
        return (name, campaign, kind, want, 'ABORT', err[-300:].replace(path, rel), rel)
    # the program compiled
    if internal:
        return (name, campaign, kind, want, 'INTERNAL',
                '; '.join(w['msg'] for w in warns if 'internal:' in w['msg'])[:300], rel)
    try:
        r = subprocess.run([exe], capture_output=True, text=True, errors='replace', timeout=run_timeout)
    except subprocess.TimeoutExpired:
        return (name, campaign, kind, want, 'ABORT', 'run timeout', rel)
    out = r.stdout or ''
    blob = out + (r.stderr or '')
    if kind == 'value':
        if out == want:
            return (name, campaign, kind, want, 'OK', repr(out), rel)
        return (name, campaign, kind, want, 'ACCEPTED', f'printed {out!r}, want {want!r}', rel)
    if kind == 'fault':
        if want in blob:
            return (name, campaign, kind, want, 'OK', 'fault raised', rel)
        return (name, campaign, kind, want, 'ACCEPTED', f'no {want!r} in output {blob[:200]!r}', rel)
    if kind == 'noabort':
        # The program must run to COMPLETION: a runtime fault is a wrong answer,
        # not a pass.  (The name says what it checks: the compiler must not abort
        # AND the program must not fault.)
        if r.returncode == 0:
            return (name, campaign, kind, want, 'OK', f'rc=0 {out!r}', rel)
        return (name, campaign, kind, want, 'ACCEPTED', f'rc={r.returncode} {blob[:200]!r}', rel)
    # kind == 'reject' but the program compiled: the wrong shape was admitted
    return (name, campaign, kind, want, 'ACCEPTED', f'compiled and ran, printed {out!r}', rel)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--out', help='output directory (all scratch goes here)')
    ap.add_argument('--bezelc', default='build/bezelc')
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--filter', default='', help='substring matched against the cell name')
    ap.add_argument('--campaign', default='', help='only the cells owned by this campaign ("2", "5")')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--timeout', type=int, default=300)
    ap.add_argument('--run-timeout', type=int, default=60)
    a = ap.parse_args(argv)
    if a.jobs < 1:
        ap.error('--jobs must be at least 1')
    cells = [c for c in CELLS if a.filter in c[0] and (not a.campaign or c[6] == a.campaign)]
    if a.list:
        for c in cells:
            print(c[0])
        return 0
    if not cells:
        print('no cells selected', file=sys.stderr)
        return 2
    if not a.out:
        ap.error('--out is required')
    bezelc = os.path.abspath(a.bezelc)
    if not os.access(bezelc, os.X_OK):
        print(f'no compiler at {bezelc}', file=sys.stderr)
        return 2
    asan = os.environ.get('BEZEL_AOT_FORCE_SANITIZE', '') == 'address'
    progdir = os.path.join(a.out, 'programs')
    outdir = os.path.join(a.out, 'cells')
    os.makedirs(progdir, exist_ok=True)
    os.makedirs(outdir, exist_ok=True)
    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        futs = [ex.submit(classify_one, bezelc, progdir, outdir, c, asan, a.timeout, a.run_timeout)
                for c in cells]
        for f in futs:
            rows.append(f.result())
    rows.sort()
    tsv = os.path.join(a.out, 'grid.tsv')
    with open(tsv, 'w') as f:
        f.write('cell\tcampaign\texpectation\twant\tclass\tdetail\tprogram\n')
        for r in rows:
            f.write('\t'.join((x if x is not None else '').replace('\t', ' ').replace('\n', '\\n')
                              for x in r) + '\n')
    counts = {k: 0 for k in CLASSES}
    per_campaign = {}
    for r in rows:
        counts[r[4]] = counts.get(r[4], 0) + 1
        c = per_campaign.setdefault(r[1], {k: 0 for k in CLASSES})
        c[r[4]] += 1
    wrong = len(rows) - counts['OK']
    print(f'{len(rows)} cells, {wrong} wrong')
    for k in CLASSES:
        print(f'  {k:9s} {counts[k]}')
    for camp in sorted(per_campaign):
        c = per_campaign[camp]
        cw = sum(c.values()) - c['OK']
        print(f'  campaign {camp}: {sum(c.values())} cells, {cw} wrong')
    # The sanitizer skip counter is HONEST: a cell whose compile failed because the
    # ASan runtime was unavailable counts as skipped, not as a wrong cell.
    skip_pat = re.compile(r'cannot find -fsanitize|libasan|ASan runtime|unable to find.*sanitiz', re.I)
    skipped = sum(1 for r in rows if r[4] == 'ABORT' and skip_pat.search(r[5] or ''))
    print(f'NATIVE SANITIZER: {skipped} check(s) skipped')
    if wrong:
        print('\nWRONG CELLS:')
        for r in rows:
            if r[4] != 'OK':
                print(f'  {r[0]:34s} c{r[1]} {r[4]:9s} {r[5][:110]}')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
