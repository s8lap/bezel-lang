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

def C(name, body, expect, pre="", extra="", imports=""):
    CELLS.append((name, extra, pre, body, expect, imports))

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
C("int_parse_string", 'Console.WriteLine(int.Parse("16"));', ("value", "16\n"))
C("int_parse_int", 'Console.WriteLine(int.Parse(16));', ("reject", None))
C("int_parse_bool", 'Console.WriteLine(int.Parse(true));', ("reject", None))
C("int_parse_char", "Console.WriteLine(int.Parse('a'));", ("reject", None))
C("int_parse_double", 'Console.WriteLine(int.Parse(1.5));', ("reject", None))
C("int_parse_object", 'Console.WriteLine(int.Parse(o));', ("reject", None),
  pre="var o: Object := 1;")
C("int_parse_radix_ok", 'Console.WriteLine(int.Parse("ff", 16));', ("value", "255\n"))
C("int_parse_radix_uint", 'Console.WriteLine(int.Parse("ff", 16u));', ("reject", None))
C("int_parse_radix_string", 'Console.WriteLine(int.Parse("ff", "16"));', ("reject", None))
C("int_parse_radix_bool", 'Console.WriteLine(int.Parse("ff", true));', ("reject", None))
C("int_tryparse_string",
  'Console.WriteLine(int.TryParse("16", out v));\nConsole.WriteLine(v);',
  ("value", "True\n16\n"), pre="var v: int := 0;")
C("int_tryparse_int", '_ := int.TryParse(16, out v);', ("reject", None), pre="var v: int := 0;")
C("int_tryparse_double", '_ := int.TryParse(1.5, out v);', ("reject", None), pre="var v: int := 0;")
C("int_tryparse_nullable", '_ := int.TryParse(s, out v);', ("reject", None),
  pre="var v: int := 0;\nvar s: string? := \"16\";")
C("double_parse_string", 'Console.WriteLine(double.Parse("1.5"));', ("value", "1.5\n"))
C("double_parse_int", 'Console.WriteLine(double.Parse(1));', ("reject", None))
C("double_tryparse_int", '_ := double.TryParse(1, out v);', ("reject", None), pre="var v: double := 0.0;")
C("decimal_parse_string", 'Console.WriteLine(decimal.Parse("1.5"));', ("value", "1.5\n"))
C("decimal_parse_double", 'Console.WriteLine(decimal.Parse(1.5));', ("reject", None))
C("decimal_tryparse_double", '_ := decimal.TryParse(1.5, out v);', ("reject", None),
  pre="var v: decimal := 0m;")
C("bool_parse_string", 'Console.WriteLine(bool.Parse("true"));', ("value", "True\n"))
C("bool_parse_int", 'Console.WriteLine(bool.Parse(1));', ("reject", None))
C("bool_tryparse_int", '_ := bool.TryParse(1, out v);', ("reject", None), pre="var v: bool := false;")
C("char_parse_string", "Console.WriteLine(char.Parse(\"a\"));", ("value", "a\n"))
C("char_parse_radix", "Console.WriteLine(char.Parse(\"a\", 16));", ("reject", None))
C("char_tryparse_int", "_ := char.TryParse(1, out v);", ("reject", None), pre="var v: char := 'x';")
C("guid_tryparse_string",
  'Console.WriteLine(Guid.TryParse("00000000-0000-0000-0000-000000000000", out g));',
  ("value", "True\n"), pre="var g: Guid := Guid.Empty;")
C("guid_tryparse_int", '_ := Guid.TryParse(1, out g);', ("reject", None), pre="var g: Guid := Guid.Empty;")
C("long_parse_string", 'Console.WriteLine(long.Parse("16"));', ("value", "16\n"))
C("long_parse_intarg", 'Console.WriteLine(long.Parse(16));', ("reject", None))

# ------------------------------------------------------- B2 ToString/format
C("int_tostring_radix", 'Console.WriteLine((255).ToString(16));', ("value", "ff\n"))
C("int_tostring_radix_uint", 'Console.WriteLine((255).ToString(16u));', ("reject", None))
C("int_tostring_radix_null", 'Console.WriteLine((255).ToString(null));', ("reject", None))
C("int_tostring_radix_string", 'Console.WriteLine((255).ToString("16"));', ("reject", None))
C("long_tostring_radix", 'Console.WriteLine((255L).ToString(16));', ("value", "ff\n"))
C("long_tostring_null", 'Console.WriteLine((255L).ToString(null));', ("reject", None))
C("int_tostring_format_d", 'Console.WriteLine((255).ToString("D"));', ("value", "255\n"))
C("int_tostring_format_x", 'Console.WriteLine((255).ToString("x"));', ("value", "ff\n"))
C("int_tostring_format_X", 'Console.WriteLine((255).ToString("X"));', ("value", "FF\n"))
C("bool_tostring_format", 'Console.WriteLine((true).ToString("F4"));', ("reject", None))
C("char_tostring_format", "Console.WriteLine(('a').ToString(\"F4\"));", ("reject", None))
C("double_tostring_format_c", 'Console.WriteLine((1.5).ToString("C"));', ("fault", "FormatException"))
C("double_tostring_format_d", 'Console.WriteLine((3.5).ToString("D"));', ("fault", "FormatException"))
C("double_tostring_format_bad", 'Console.WriteLine((1.5).ToString("Q"));', ("fault", "FormatException"))
C("string_tostring_format_bad", 'Console.WriteLine("x".ToString("Q"));', ("reject", None))
C("decimal_tostring_radix", 'Console.WriteLine((15m).ToString(16));', ("reject", None))
C("decimal_tostring_format", 'Console.WriteLine((1.5m).ToString("F2"));', ("value", "1.50\n"))
C("convert_tostring_int", 'Console.WriteLine(Convert.ToString(255));', ("value", "255\n"))
C("convert_tostring_radix", 'Console.WriteLine(Convert.ToString(255, 16));', ("value", "ff\n"))
C("convert_tostring_radix_long", 'Console.WriteLine(Convert.ToString(255, lv));', ("reject", None),
  pre="var lv: long := 16;")
C("convert_tostring_char", "Console.WriteLine(Convert.ToString('A'));", ("reject", None))
C("convert_tostring_uint", 'Console.WriteLine(Convert.ToString(255U));', ("reject", None))
C("convert_tostring_double", 'Console.WriteLine(Convert.ToString(1.5));', ("value", "1.5\n"))

# ------------------------------------------------------------- B3 Math
C("math_max_uint", 'Console.WriteLine(Math.Max(3u, 4000000000u));', ("value", "4000000000\n"))
C("math_min_char", "Console.WriteLine(Math.Min('a', 'b'));", ("value", "a\n"))
C("math_max_int", 'Console.WriteLine(Math.Max(1, 2));', ("value", "2\n"))
C("math_abs_double", 'Console.WriteLine(Math.Abs(-1.5));', ("value", "1.5\n"))
C("math_abs_decimal", 'Console.WriteLine(Math.Abs(-1.5m));', ("nocrash", None))
C("math_abs_decimal_nullable", 'Console.WriteLine(Math.Abs(dn));', ("reject", None),
  pre="var dn: decimal? := 1.5m;")
C("math_abs_uint", 'Console.WriteLine(Math.Abs(4000000000u));', ("reject", None))
C("math_round_decimal", 'Console.WriteLine(Math.Round(1.5m));', ("reject", None))
C("math_sqrt_uint", 'Console.WriteLine(Math.Sqrt(4000000000u));', ("value", "63245.553203367585\n"))
C("math_floor_double", 'Console.WriteLine(Math.Floor(2.5));', ("value", "2\n"))
C("math_floor_nullable", 'Console.WriteLine(Math.Floor(dn));', ("reject", None),
  pre="var dn: double? := 2.5;")
C("math_floor_narrowed",
  'if d <> null then Console.WriteLine(Math.Floor(d)); end if;',
  ("value", "2\n"), pre="var d: double? := 2.5;")
C("math_sign_string", 'Console.WriteLine(Math.Sign("x"));', ("reject", None))
C("math_sign_bool", 'Console.WriteLine(Math.Sign(true));', ("reject", None))
C("math_sign_double", 'Console.WriteLine(Math.Sign(-1.5));', ("value", "-1\n"))
C("math_max_mixed", 'Console.WriteLine(Math.Max(1, 2L));', ("reject", None))
C("math_minmax_typed_result", 'var r: string := Math.Max(1, 2);', ("reject", None))

# --------------------------------------------------------- B4 StringBuilder / string
C("sb_append_string", '_ := sb.Append("a");\nConsole.WriteLine(sb.ToString());',
  ("value", "a\n"), pre="var sb := new StringBuilder();")
C("sb_append_int", '_ := sb.Append(1);\nConsole.WriteLine(sb.ToString());',
  ("value", "1\n"), pre="var sb := new StringBuilder();")
C("sb_append_null", '_ := sb.Append(null);', ("reject", None), pre="var sb := new StringBuilder();")
C("sb_replace_int", '_ := sb.Replace(1, 2);', ("reject", None), pre="var sb := new StringBuilder();")
C("sb_insert_two_strings", '_ := sb.Insert("x", "y");', ("reject", None), pre="var sb := new StringBuilder();")
C("sb_insert_zero", '_ := sb.Insert(0);', ("reject", None), pre="var sb := new StringBuilder();")
C("sb_remove_two_args", '_ := sb.Remove("a", 1);', ("reject", None), pre="var sb := new StringBuilder();")
C("sb_replace_two_strings", '_ := sb.Replace("a", "b");\nConsole.WriteLine(sb.ToString());',
  ("value", "\n"), pre="var sb := new StringBuilder();")
C("sb_appendformat_string", '_ := sb.AppendFormat("x", a);', ("nocrash", None),
  pre="var sb := new StringBuilder();\nusing var a := new Array<Object>();")
C("string_contains_int", 'Console.WriteLine("abc".Contains(1));', ("reject", None))
C("string_contains_string", 'Console.WriteLine("abc".Contains("b"));', ("value", "True\n"))
C("string_contains_object", 'Console.WriteLine("abc".Contains(o));', ("reject", None),
  pre="var o: Object := \"b\";")
C("string_split_int", 'Console.WriteLine("abc".Split(1));', ("reject", None))
C("string_lastindexof_int", 'Console.WriteLine("abc".LastIndexOf(1));', ("reject", None))
C("string_replace_object", 'Console.WriteLine("abc".Replace(o, "x"));', ("reject", None),
  pre="var o: Object := \"b\";")
C("string_repeat_char", "Console.WriteLine(string.Repeat('a', 3));", ("value", "aaa\n"))
C("string_repeat_string", 'Console.WriteLine(string.Repeat("ab", 3));', ("value", "ababab\n"))
C("string_remove_oob", 'Console.WriteLine("ab".Remove(253).Length);', ("nocrash", None))
C("string_equals_named_reordered",
  'Console.WriteLine("ABCDEF".Equals(ignoreCase := true, other := "abcdef"));', ("value", "True\n"))
C("string_equals_named_ordered",
  'Console.WriteLine("ABCDEF".Equals(other := "abcdef", ignoreCase := true));', ("value", "True\n"))
C("string_compareto_named_reordered",
  'Console.WriteLine("ABCDEF".CompareTo(ignoreCase := true, other := "abcdef"));', ("value", "0\n"))
C("string_join_int_array", 'Console.WriteLine(string.Join(", ", a));', ("reject", None),
  pre="using var a := new Array<int>();")
C("string_isascii", 'Console.WriteLine("abc".IsAscii());', ("value", "True\n"))
C("debug_writeline_stringbuilder", '_ := sb.Append("x");\nDebug.WriteLine(sb);',
  ("reject", None), pre="var sb := new StringBuilder();", imports="import System;")

# ------------------------------------------------------- B5 nullable carrier
C("sb_append_narrowed_int", 'if n <> null then _ := sb.Append(n); end if;',
  ("nocrash", None), pre="var sb := new StringBuilder();\nvar n: int? := 1;")
C("int_tostring_radix_narrowed", 'if n <> null then Console.WriteLine(n.ToString(16)); end if;',
  ("value", "ff\n"), pre="var n: int? := 255;")
C("int_parse_from_narrowed", 'if n <> null then Console.WriteLine(int.Parse(n.ToString())); end if;',
  ("value", "1\n"), pre="var n: int? := 1;")
C("math_floor_nullable_null", 'Console.WriteLine(Math.Floor(dnull));', ("reject", None),
  pre="var dnull: double? := null;")

# ------------------------------------------------------- B6 null at a parameter
C("sb_append_null_literal", '_ := sb.Append(null);', ("reject", None), pre="var sb := new StringBuilder();")
C("string_equals_null", 'Console.WriteLine("a".Equals(null));', ("value", "False\n"))
C("datetime_tostring_null", 'Console.WriteLine(DateTime.Now.ToString(null));', ("reject", None))

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
  ("reject", None), pre="using var a := new Array<int>();")
C("encoding_getstring_byte_array", 'a.Add(65);\nConsole.WriteLine(Encoding.UTF8.GetString(a));',
  ("value", "A\n"), pre="using var a := new Array<byte>();")
C("builtin_typearg_list", '_ := Console.WriteLine<int>("x");', ("reject", None))
C("builtin_typearg_arity", '_ := Console.WriteLine<int, int>("x");', ("reject", None))

# ------------------------------------------------------- method-reference face
C("math_max_reference", 'var g := Math.Max;\nConsole.WriteLine(g(1, 2));', ("reject", None))
C("math_sign_reference", 'var g := Math.Sign;', ("reject", None))

# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

DIAG = re.compile(r'^(?P<file>[^\n:]*?):(?P<line>\d+):(?P<col>\d+):\s*'
                   r'(?:(?P<sev>error|warning|note):\s*)?(?P<msg>.*)$', re.M)
INTERNAL = re.compile(r'\binternal:', re.I)
ABORT_PAT = re.compile(r'IR verification failed|codegen error|Assertion|Aborted|Segmentation fault|'
                       r'AddressSanitizer|SUMMARY: |stack dump|out of memory', re.I)

CLASSES = ('OK', 'ACCEPTED', 'REJECTED', 'ABORT', 'INTERNAL')


def classify_one(bezelc, progdir, outdir, cell, asan, timeout, run_timeout):
    name, extra, pre, body, expect, imports = cell
    kind, want = expect
    src = prog(extra, pre, body, imports)
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
        return (name, kind, want, 'ABORT', 'compile timeout', '')
    err = (c.stdout or '') + (c.stderr or '')
    diags = [d.groupdict() for d in DIAG.finditer(err)]
    errors = [d for d in diags if d['sev'] in (None, 'error')]
    warns = [d for d in diags if d['sev'] in ('warning', 'note')]
    internal = bool(INTERNAL.search(err))
    if c.returncode != 0:
        if ABORT_PAT.search(err) or (not errors and internal):
            return (name, kind, want, 'ABORT', 'compiler abort', err[-400:])
        if errors:
            first = f"{errors[0]['file']}:{errors[0]['line']}:{errors[0]['col']}: error: {errors[0]['msg']}"
            if kind == 'reject':
                if want is None or want in first:
                    return (name, kind, want, 'OK', first, '')
                return (name, kind, want, 'REJECTED', first, '')
            return (name, kind, want, 'REJECTED', first, '')
        return (name, kind, want, 'ABORT', err[-400:], '')
    # the program compiled
    if internal:
        return (name, kind, want, 'INTERNAL', '; '.join(w['msg'] for w in warns if 'internal:' in w['msg'])[:300], '')
    try:
        r = subprocess.run([exe], capture_output=True, text=True, errors='replace', timeout=run_timeout)
    except subprocess.TimeoutExpired:
        return (name, kind, want, 'ABORT', 'run timeout', '')
    out = r.stdout or ''
    blob = out + (r.stderr or '')
    if kind == 'value':
        if out == want:
            return (name, kind, want, 'OK', repr(out), '')
        return (name, kind, want, 'ACCEPTED', f'printed {out!r}, want {want!r}', '')
    if kind == 'fault':
        if want in blob:
            return (name, kind, want, 'OK', 'fault raised', '')
        return (name, kind, want, 'ACCEPTED', f'no {want!r} in output {blob[:200]!r}', '')
    if kind == 'clean':
        if r.returncode == 0:
            return (name, kind, want, 'OK', f'rc=0 {out!r}', '')
        return (name, kind, want, 'ACCEPTED', f'rc={r.returncode} {blob[:200]!r}', '')
    if kind == 'nocrash':
        return (name, kind, want, 'OK', f'rc={r.returncode}', '')
    # kind == 'reject' but the program compiled: the wrong shape was admitted
    return (name, kind, want, 'ACCEPTED', f'compiled and ran, printed {out!r}', '')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--out', required=True, help='output directory (all scratch goes here)')
    ap.add_argument('--bezelc', default='build/bezelc')
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--filter', default='', help='substring matched against the cell name')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--timeout', type=int, default=300)
    ap.add_argument('--run-timeout', type=int, default=60)
    a = ap.parse_args(argv)
    if a.jobs < 1:
        ap.error('--jobs must be at least 1')
    cells = [c for c in CELLS if a.filter in c[0]]
    if a.list:
        for c in cells:
            print(c[0])
        return 0
    if not cells:
        print('no cells selected', file=sys.stderr)
        return 2
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
        f.write('cell\texpectation\twant\tclass\tdetail\n')
        for r in rows:
            f.write('\t'.join((x or '').replace('\t', ' ').replace('\n', '\\n') for x in r) + '\n')
    counts = {k: 0 for k in CLASSES}
    for r in rows:
        counts[r[3]] = counts.get(r[3], 0) + 1
    wrong = len(rows) - counts['OK']
    print(f'{len(rows)} cells, {wrong} wrong')
    for k in CLASSES:
        print(f'  {k:9s} {counts[k]}')
    # The sanitizer skip counter is HONEST: a cell whose compile failed because the
    # ASan runtime was unavailable counts as skipped, not as a wrong cell.
    skip_pat = re.compile(r'cannot find -fsanitize|libasan|ASan runtime|unable to find.*sanitiz', re.I)
    skipped = sum(1 for r in rows if r[3] == 'ABORT' and skip_pat.search(r[4] or ''))
    print(f'NATIVE SANITIZER: {skipped} check(s) skipped')
    if wrong:
        print('\nWRONG CELLS:')
        for r in rows:
            if r[3] != 'OK':
                print(f'  {r[0]:34s} {r[3]:9s} {r[4][:110]}')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
