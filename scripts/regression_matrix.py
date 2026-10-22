#!/usr/bin/env python3
"""Regression matrix: compare several bezelc builds over generated ASan programs.

WHEN TO RUN (not a gate phase; run it by hand): before picking any change to
runtime object layout, disposal, or retain/release (collection runtime structs,
drop/dispose emission, owned-field/param/local release, move/adopt paths). The
#6776 layout change shipped 3 regressions that every suite missed and that a
generated pre/tip/fix ASan matrix caught. See tests/README.md, "Regression
matrix".

    scripts/regression_matrix.py --out DIR \
        --bezelc pre=/path/to/reference/bezelc --bezelc new=build/bezelc \
        [--filter Set,SortedSet] [--scope loc|app] [--jobs 8] [--iterations 100]

Tags are ordered: a LATER compiler is compared against every EARLIER one, and a
cell where it is worse is printed. Exit status: 0 no cell got worse, 1 at least
one did, 2 usage or infrastructure error (library would not compile, no cells).

Each cell is one generated program (type x operation x {loc, app}). `loc_` is a
single module; `app_` imports the same helpers from a separately compiled library
module `LibX` (`bezelc -c` + `--import`), because cross-module paths take
different codegen routes. Every Main runs its operation `--iterations` times
(default 100) in a while loop so that a per-iteration leak is unmistakable, and
is built with `--sanitize=address`. Classes, best to worst:
CLEAN (rc 0, no sanitizer report) > LEAK(bytes/allocs) (LeakSanitizer only) >
CRASH (rc != 0, ASan error, timeout) > CF (did not compile). "Worse" means a
lower class, or, when both ran to completion, different stdout or a bigger leak.

Generation is deterministic (fixed ordering, no randomness). All scratch lives
under --out, never /tmp. Layout: OUT/programs/*.bzl, OUT/<tag>/ (exe, logs),
OUT/<tag>.tsv (name, class, detail, stdout), OUT/report.txt.

Every type and member used exists in docs/Bezel_Standard_Library.md; the
sections are cited at the type table (T) below. Do not add a member that is not
there; a stdlib gap is an issue, not a generator patch.
"""
import argparse
import concurrent.futures
import os
import re
import subprocess
import sys

CF, CRASH, LEAK, CLEAN = 'CF', 'CRASH', 'LEAK', 'CLEAN'
TIER = {CF: 0, CRASH: 1, LEAK: 2, CLEAN: 3}

# ---------------------------------------------------------------------------
# Generator
# ---------------------------------------------------------------------------

STR3 = '"a" + "x"', '"b" + "y"', '"c" + "z"'   # fresh heap strings (retain/release path)
S3DUP = '"a" + "x"', '"b" + "y"', '"a" + "x"'  # third element equals the first (set dedupe path)


def _fill(ty, stmts):
    return f'var v := new {ty}(); {stmts}'


# key -> (type, builder of `v`). Stdlib sections: Array/OwningArray 4.4/4.5,
# LinkedList/OwningLinkedList 4.6/4.7, Dictionary/OwningDictionary 4.8/4.9,
# Set/OwningSet 4.10, Stack/Queue and owning forms 4.11, OwningBuffer 4.14,
# SortedDictionary 4.17, SortedSet 4.18, FrozenArray 4.19, StringBuilder 3.1,
# Mutex 6.2. Preamble types (P, P2, E, V, IS, Cmp, Item) are user types below.
T = {
    # --- the 6776e set: element kinds int / string / class ---
    'St_i': ('Set<int>', _fill('Set<int>', 'v.Add(5); v.Add(6); v.Add(7);')),
    'St_s': ('Set<string>', _fill('Set<string>', f'v.Add({STR3[0]}); v.Add({STR3[1]}); v.Add({STR3[2]});')),
    'St_c': ('Set<Item>', _fill('Set<Item>', '')),     # array-backed class element: holds borrows, built empty
    'SS_i': ('SortedSet<int>', _fill('SortedSet<int>', 'v.Add(7); v.Add(5); v.Add(6);')),
    'SS_s': ('SortedSet<string>', _fill('SortedSet<string>', f'v.Add({STR3[2]}); v.Add({STR3[0]}); v.Add({STR3[1]});')),
    'OS_c': ('OwningSet<Item>', _fill('OwningSet<Item>', 'v.Add(new Item(5)); v.Add(new Item(6)); v.Add(new Item(7));')),
    'A_i': ('Array<int>', _fill('Array<int>', 'v.Add(5); v.Add(6); v.Add(7);')),
    'A_s': ('Array<string>', _fill('Array<string>', f'v.Add({STR3[0]}); v.Add({STR3[1]}); v.Add({STR3[2]});')),
    'A_c': ('Array<Item>', _fill('Array<Item>', '')),
    'OA_c': ('OwningArray<Item>', _fill('OwningArray<Item>', 'v.Add(new Item(5)); v.Add(new Item(6)); v.Add(new Item(7));')),
    'D_i': ('Dictionary<int, int>', _fill('Dictionary<int, int>', 'v[5] := 50; v[6] := 60; v[7] := 70;')),
    'D_s': ('Dictionary<string, string>', _fill('Dictionary<string, string>', f'v[{STR3[0]}] := "p" + "q"; v[{STR3[1]}] := "r" + "s"; v[{STR3[2]}] := "t" + "u";')),
    'OD_c': ('OwningDictionary<int, Item>', _fill('OwningDictionary<int, Item>', 'v.Add(5, new Item(5)); v.Add(6, new Item(6)); v.Add(7, new Item(7));')),
    'OD_sc': ('OwningDictionary<string, Item>', _fill('OwningDictionary<string, Item>', f'v.Add({STR3[0]}, new Item(5)); v.Add({STR3[1]}, new Item(6)); v.Add({STR3[2]}, new Item(7));')),
    'SD_i': ('SortedDictionary<int, int>', _fill('SortedDictionary<int, int>', 'v[7] := 70; v[5] := 50; v[6] := 60;')),
    'SD_s': ('SortedDictionary<string, string>', _fill('SortedDictionary<string, string>', f'v[{STR3[2]}] := "t" + "u"; v[{STR3[0]}] := "p" + "q"; v[{STR3[1]}] := "r" + "s";')),
    'Sk_i': ('Stack<int>', _fill('Stack<int>', 'v.Push(5); v.Push(6); v.Push(7);')),
    'Sk_s': ('Stack<string>', _fill('Stack<string>', f'v.Push({STR3[0]}); v.Push({STR3[1]}); v.Push({STR3[2]});')),
    'OSk_c': ('OwningStack<Item>', _fill('OwningStack<Item>', 'v.Push(new Item(5)); v.Push(new Item(6)); v.Push(new Item(7));')),
    'Q_i': ('Queue<int>', _fill('Queue<int>', 'v.Enqueue(5); v.Enqueue(6); v.Enqueue(7);')),
    'Q_s': ('Queue<string>', _fill('Queue<string>', f'v.Enqueue({STR3[0]}); v.Enqueue({STR3[1]}); v.Enqueue({STR3[2]});')),
    'OQ_c': ('OwningQueue<Item>', _fill('OwningQueue<Item>', 'v.Enqueue(new Item(5)); v.Enqueue(new Item(6)); v.Enqueue(new Item(7));')),
    'LL_i': ('LinkedList<int>', _fill('LinkedList<int>', '_ := v.AddLast(5); _ := v.AddLast(6); _ := v.AddLast(7);')),
    'LL_s': ('LinkedList<string>', _fill('LinkedList<string>', f'_ := v.AddLast({STR3[0]}); _ := v.AddLast({STR3[1]}); _ := v.AddLast({STR3[2]});')),
    'OLL_c': ('OwningLinkedList<Item>', _fill('OwningLinkedList<Item>', '_ := v.AddLast(new Item(5)); _ := v.AddLast(new Item(6)); _ := v.AddLast(new Item(7));')),
    'OB_i': ('OwningBuffer<int>', 'var v := new OwningBuffer<int>(3); v[0] := 5; v[1] := 6; v[2] := 7;'),
    'OB_c': ('OwningBuffer<Item>', 'var v := new OwningBuffer<Item>(3); v[0] := new Item(5); v[1] := new Item(6); v[2] := new Item(7);'),
    'FA_i': ('FrozenArray<int>', 'var b := new OwningBuffer<int>(3); b[0] := 5; b[1] := 6; b[2] := 7; var v := FrozenArray<int>.From(move b);'),
    'FA_s': ('FrozenArray<string>', f'using var a := new Array<string>(); a.Add({STR3[0]}); a.Add({STR3[1]}); a.Add({STR3[2]}); var v := FrozenArray<string>.From(a);'),
    'SB': ('StringBuilder', _fill('StringBuilder', 'v.Append("ab"); v.Append("cde");')),
    'MX': ('Mutex', _fill('Mutex', '')),
    # --- the reviewer's element kinds (probe matrix for #6776 fix-forward 3) ---
    'St_r': ('Set<P>', _fill('Set<P>', 'v.Add(new P(1, 2)); v.Add(new P(3, 4)); v.Add(new P(1, 2));')),          # record
    'St_e': ('Set<E>', _fill('Set<E>', 'v.Add(E.A); v.Add(E.B); v.Add(E.A);')),                                  # enum
    'St_d': ('Set<decimal>', _fill('Set<decimal>', 'v.Add(1.0m); v.Add(2m); v.Add(1.00m);')),                      # decimal
    'St_ch': ('Set<V>', _fill('Set<V>', 'v.Add(V.A); v.Add(V.B(1)); v.Add(V.B(1));')),                            # choice
    'St_if': ('Set<IS>', _fill('Set<IS>', '')),                                                                    # interface
    'St_ni': ('Set<int?>', _fill('Set<int?>', 'v.Add(5); v.Add(null); v.Add(5);')),                                # nullable value
    'St_nc': ('Set<Item?>', _fill('Set<Item?>', '')),                                                              # nullable class
    'St_sd': ('Set<string>', _fill('Set<string>', f'v.Add({S3DUP[0]}); v.Add({S3DUP[1]}); v.Add({S3DUP[2]});')),   # duplicate heap strings
    'SS_c': ('SortedSet<Cmp>', _fill('SortedSet<Cmp>', '')),                                                       # IComparable class
    'SS_r': ('SortedSet<P2>', _fill('SortedSet<P2>', 'v.Add(new P2(2)); v.Add(new P2(1)); v.Add(new P2(2));')),    # IComparable record
}

INT_CONTAINS = {'St_i', 'SS_i', 'A_i', 'Sk_i', 'Q_i', 'LL_i'}
STR_CONTAINS = {'St_s', 'SS_s', 'A_s', 'Sk_s', 'Q_s', 'LL_s'}
INT_KEY = {'D_i', 'SD_i', 'OD_c'}
STR_KEY = {'D_s', 'SD_s', 'OD_sc'}
ITEM_ITER = {'OA_c', 'OS_c', 'OSk_c', 'OQ_c', 'OLL_c'}
COUNT_ONLY = {'St_c', 'A_c', 'St_r', 'St_e', 'St_d', 'St_ch', 'St_if', 'St_ni', 'St_nc', 'St_sd', 'SS_c', 'SS_r'}
HAS_CLEAR = {'St_i', 'St_s', 'St_c', 'SS_i', 'SS_s', 'OS_c', 'A_i', 'A_s', 'A_c', 'OA_c',
             'D_i', 'D_s', 'OD_c', 'OD_sc', 'SD_i', 'SD_s', 'Sk_i', 'Sk_s', 'OSk_c',
             'Q_i', 'Q_s', 'OQ_c', 'LL_i', 'LL_s', 'OLL_c', 'SB',
             'St_r', 'St_e', 'St_d', 'St_ch', 'St_if', 'St_ni', 'St_nc', 'St_sd', 'SS_c', 'SS_r'}
# types whose `new T()` takes no arguments (OwningBuffer needs a size, FrozenArray has no ctor)
NEWABLE = {k for k in T if not k.startswith(('OB_', 'FA_'))}


def cnt(k):
    """Name of the size property (stdlib: Count on collections, Length on buffers/builders)."""
    if k == 'MX':
        return None
    return 'Length' if k.startswith(('OB', 'FA')) or k == 'SB' else 'Count'


def show(k, x):
    """One statement reading the collection's size and, where cheap, its contents."""
    if k in INT_CONTAINS:
        return f'Console.WriteLine($"{{{x}.Count}} {{{x}.Contains(6)}}");'
    if k in STR_CONTAINS:
        return f'Console.WriteLine($"{{{x}.Count}} {{{x}.Contains("by")}}");'
    if k in INT_KEY:
        return f'Console.WriteLine($"{{{x}.Count}} {{{x}.ContainsKey(6)}}");'
    if k in STR_KEY:
        return f'Console.WriteLine($"{{{x}.Count}} {{{x}.ContainsKey("by")}}");'
    if k in ITEM_ITER:
        return f'var t := 0; for var e in {x} do t += e.N; end; Console.WriteLine($"{{{x}.Count}} {{t}}");'
    if k in COUNT_ONLY:
        return f'Console.WriteLine({x}.Count);'
    if k in ('OB_i', 'FA_i', 'FA_s'):
        return f'Console.WriteLine($"{{{x}.Length}} {{{x}[1]}}");'
    if k == 'OB_c':
        return f'Console.WriteLine({x}.Length);'
    if k == 'SB':
        return f'Console.WriteLine($"{{{x}.Length}} {{{x}.ToString()}}");'
    if k == 'MX':
        return f'using var lk := {x}.Acquire(); Console.WriteLine("m");'   # §6.2
    raise KeyError(k)


def ops(k):
    """Operation name -> Main-loop body for type key k."""
    ty = T[k][0]
    S = lambda x: show(k, x)
    c = cnt(k)
    o = {}
    o['using'] = f'using var x := M.Mk_{k}(); {S("x")}'
    o['relocal'] = f'var x := M.Mk_{k}(); x := M.Mk_{k}(); {S("x")}'
    if k != 'MX':   # a second Acquire in one scope redeclares `lk`
        o['movedre'] = f'var x := M.Mk_{k}(); var y := move x; x := M.Mk_{k}(); {S("x")} {S("y")}'
    o['adopt'] = f'var x := M.Mk_{k}(); using var y := move x; {S("y")}'
    o['moveparam'] = f'var x := M.Mk_{k}(); H.Sink(move x);'
    o['tempparam'] = f'H.Sink(M.Mk_{k}());'
    o['borrow'] = f'using var x := M.Mk_{k}(); H.Peek(x);'
    o['tempborrow'] = f'H.Peek(M.Mk_{k}());'
    o['hop'] = f'using var x := H.Hop(); {S("x")}'
    o['hopre'] = f'var x := H.Hop(); x := H.Hop(); {S("x")}'
    o['fieldown'] = 'using var b := new Box(); b.Show();'
    o['refield'] = 'using var b := new Box(); b.Reset(); b.Reset(); b.Show();'
    o['objshow'] = f'using var x := M.Mk_{k}(); H.Show(x);'
    o['objdrop'] = f'var x := M.Mk_{k}(); var o: Object := move x; o.Dispose(); Console.WriteLine("d");'
    if k in HAS_CLEAR:
        o['clear'] = f'using var x := M.Mk_{k}(); x.Clear(); {S("x")}'
    o['nestOA'] = f'using var outer := new OwningArray<{ty}>(); outer.Add(M.Mk_{k}()); outer.Add(M.Mk_{k}()); Console.WriteLine(outer.Count);'
    o['nestOD'] = f'using var outer := new OwningDictionary<int, {ty}>(); outer.Add(1, M.Mk_{k}()); outer.Add(2, M.Mk_{k}()); Console.WriteLine(outer.Count);'
    if c:
        o['tempcount'] = f'Console.WriteLine(M.Mk_{k}().{c});'
        o['lambda'] = f'using var x := M.Mk_{k}(); var f := lambda() => x.{c}; Console.WriteLine(f());'
        o['lambdare'] = f'var x := M.Mk_{k}(); var f := lambda() => x.{c}; x := M.Mk_{k}(); Console.WriteLine(f());'
    if k in NEWABLE:
        o['relocal3'] = f'var x := M.Mk_{k}(); x := M.Mk_{k}(); x := new {ty}(); {S("x")}'
        o['relocalnew'] = f'var x := new {ty}(); x := new {ty}(); {S("x")}'
        o['refieldnew'] = 'using var b := new Box(); b.ResetNew(); b.ResetNew(); b.Show();'
        o['genfield'] = f'using var g := new GF<{ty}>(); g.Reset(); g.Reset(); Console.WriteLine(g.N());'
    return o


# Generic-context ops: a generic class with an owned collection field, a generic
# owned parameter, a generic local reassigned. Element T is int / string / Item.
# (Set<T>/SortedSet<T> with a type parameter: stdlib 4.10, 4.18; `where T:
# IComparable<T>` is the SortedSet bound.)
GENERIC_LIB = (
    'public class GB<T>\n  private owned _s: Set<T>;\n'
    '  public constructor() _s := new Set<T>(); end constructor;\n'
    '  public method Reset() _s := new Set<T>(); end Reset;\n'
    '  public method Add(x: T) _s.Add(x); end Add;\n'
    '  public method Count(): int => _s.Count;\nend GB;\n'
    'public class GS<T> where T: IComparable<T>\n  private owned _s: SortedSet<T>;\n'
    '  public constructor() _s := new SortedSet<T>(); end constructor;\n'
    '  public method Reset() _s := new SortedSet<T>(); end Reset;\n'
    '  public method Add(x: T) _s.Add(x); end Add;\n'
    '  public method Count(): int => _s.Count;\nend GS;\n'
    'public class GM\n'
    '  public static method LocRe<T>(): int\n    var l := new Set<T>(); l := new Set<T>(); return l.Count;\n  end LocRe;\n'
    '  public static method SinkS<T>(owned v: Set<T>): int => v.Count;\n'
    '  public static method MkS<T>(): owned Set<T>\n    var v := new Set<T>(); return v;\n  end MkS;\n'
    'end GM;\n')

GENERIC_OPS = {
    'gb_i': 'using var g := new GB<int>(); g.Add(1); g.Reset(); g.Add(2); g.Add(3); g.Reset(); g.Add(4); Console.WriteLine(g.Count());',
    'gb_s': 'using var g := new GB<string>(); g.Add("a" + "x"); g.Reset(); g.Add("b" + "y"); Console.WriteLine(g.Count());',
    'gb_c': 'using var a := new Item(1); using var g := new GB<Item>(); g.Add(a); g.Reset(); g.Add(a); Console.WriteLine(g.Count());',
    'gb_s_noreset': 'using var g := new GB<string>(); g.Add("a" + "x"); Console.WriteLine(g.Count());',
    'gb_s_resetonly': 'using var g := new GB<string>(); g.Add("a" + "x"); g.Reset(); Console.WriteLine(g.Count());',
    'gs_i': 'using var g := new GS<int>(); g.Add(1); g.Reset(); g.Add(2); g.Add(3); Console.WriteLine(g.Count());',
    'gs_s': 'using var g := new GS<string>(); g.Add("a" + "x"); g.Reset(); g.Add("b" + "y"); Console.WriteLine(g.Count());',
    'gs_s_noreset': 'using var g := new GS<string>(); g.Add("a" + "x"); Console.WriteLine(g.Count());',
    'gm_lr_i': 'Console.WriteLine(GM.LocRe<int>());',
    'gm_lr_s': 'Console.WriteLine(GM.LocRe<string>());',
    'gm_lr_c': 'Console.WriteLine(GM.LocRe<Item>());',
    'gm_sink_i': 'var v := GM.MkS<int>(); v.Add(1); Console.WriteLine(GM.SinkS<int>(move v));',
    'gm_sink_s': 'var v := GM.MkS<string>(); v.Add("q" + "r"); Console.WriteLine(GM.SinkS<string>(move v));',
    'gm_sink_c': 'using var a := new Item(1); var v := GM.MkS<Item>(); v.Add(a); Console.WriteLine(GM.SinkS<Item>(move v));',
    's_relocal': 'var x := new Set<string>(); x.Add("a" + "x"); x := new Set<string>(); x.Add("b" + "y"); x.Add("c" + "z"); Console.WriteLine(x.Count);',
    's_moved': 'var x := new Set<string>(); x.Add("a" + "x"); var y := move x; x := new Set<string>(); x.Add("b" + "y"); Console.WriteLine($"{x.Count} {y.Count} {y.Contains("ax")}");',
    's_ret': 'var x := GM.MkS<string>(); x.Add("a" + "x"); x := GM.MkS<string>(); x.Add("b" + "y"); Console.WriteLine(x.Count);',
    's_reloop': 'var x := new Set<string>(); var j := 0; while j < 5 do x := new Set<string>(); x.Add($"k{j}"); j += 1; end; Console.WriteLine(x.Count);',
    's_relself': 'var x := new Set<string>(); x.Add("a" + "x"); var t := move x; x := move t; x := new Set<string>(); Console.WriteLine(x.Count);',
    'ss_relocal': 'var x := new SortedSet<string>(); x.Add("a" + "x"); x := new SortedSet<string>(); x.Add("b" + "y"); x.Add("c" + "z"); Console.WriteLine(x.Count);',
    'ss_moved': 'var x := new SortedSet<string>(); x.Add("a" + "x"); var y := move x; x := new SortedSet<string>(); x.Add("b" + "y"); Console.WriteLine($"{x.Count} {y.Count}");',
}
GENERIC_KEY = 'Gen'

# User types the programs need. Item: class; P: record; P2/Cmp: IComparable;
# E: enum; V: choice; IS: interface. GF<C>: generic holder of an owned C.
PREAMBLE = (
    'public class Item\n  public N: int;\n  public constructor(n: int) N := n; end constructor;\nend Item;\n'
    'public record P(X: int, Y: int);\n'
    'public record P2(K: int): IComparable<P2>\n  public method CompareTo(other: P2): int => K - other.K;\nend P2;\n'
    'public enum E A, B; end E;\n'
    'public choice V A, B(n: int); end V;\n'
    'public interface IS method Area(): int; end IS;\n'
    'public class Cmp: IComparable<Cmp>\n  public K: int;\n  public constructor(k: int) K := k; end constructor;\n'
    '  public method CompareTo(other: Cmp): int => K - other.K;\nend Cmp;\n'
    'public class GF<C> where C: new()\n  private owned _s: C;\n'
    '  public constructor() _s := new C(); end constructor;\n'
    '  public method Reset() _s := new C(); end Reset;\n'
    '  public method N(): int => 1;\nend GF;\n')
HDR = 'import System.Threading;\n'   # Mutex (§6.2)


def mclass(keys):
    s = 'public class M\n'
    for k in keys:
        ty, b = T[k]
        s += f'  public static method Mk_{k}(): owned {ty}\n    {b}\n    return v;\n  end Mk_{k};\n'
    return s + 'end M;\n'


def helpers(k):
    ty = T[k][0]
    S = lambda x: show(k, x)
    return (f'class Box\n  private owned _s: {ty};\n'
            f'  public constructor() _s := M.Mk_{k}(); end constructor;\n'
            f'  public method Reset() _s := M.Mk_{k}(); end Reset;\n'
            + (f'  public method ResetNew() _s := new {ty}(); end ResetNew;\n' if k in NEWABLE else '')
            + f'  public method Show() {S("_s")} end Show;\nend Box;\n'
            f'class H\n'
            f'  public static method Sink(owned v: {ty}) {S("v")} end Sink;\n'
            f'  public static method Peek(v: {ty}) {S("v")} end Peek;\n'
            f'  public static method Hop(): owned {ty}\n    var v := M.Mk_{k}();\n    return v;\n  end Hop;\n'
            f'  public static method Show(o: Object) Console.WriteLine("o"); end Show;\n'   # never print `o`: a collection renders as a raw address, which varies per run
            f'end H;\n')


def main_src(body, iterations):
    return ('class C\n  public static method Main(): int\n    var i := 0;\n'
            f'    while i < {iterations} do\n'
            f'      {body}\n      i += 1;\n    end;\n    return 0;\n  end Main;\nend C;\n')


def cells(filt=None, scope=None):
    """Deterministic list of (cell name, scope, type key, op, type descriptor, body)."""
    toks = [t for t in (filt or '').split(',') if t]
    out = []
    for k in T:
        for op, body in ops(k).items():
            out.append((k, op, T[k][0], body))
    for op, body in GENERIC_OPS.items():
        out.append((GENERIC_KEY, op, 'generic Set<T> SortedSet<T> ' + op, body))
    res = []
    for k, op, desc, body in out:
        if toks and not any(t in f'{k} {desc} {op}' for t in toks):
            continue
        for sc in ('app', 'loc'):
            if scope and sc != scope:
                continue
            res.append((f'{sc}_{k}_{op}', sc, k, op, desc, body))
    return res


def generate(progdir, filt=None, scope=None, iterations=100):
    """Write LibX.bzl and every cell program into progdir; return the cell list."""
    os.makedirs(progdir, exist_ok=True)
    cs = cells(filt, scope)
    keys = sorted({c[2] for c in cs if c[2] != GENERIC_KEY}, key=list(T).index)
    has_gen = any(c[2] == GENERIC_KEY for c in cs)
    lib = 'module LibX;\n' + HDR + PREAMBLE + (mclass(keys) if keys else '') + (GENERIC_LIB if has_gen else '')
    _write(f'{progdir}/LibX.bzl', lib)
    for name, sc, k, op, desc, body in cs:
        if k == GENERIC_KEY:
            src = ('module App;\nimport LibX;\n' if sc == 'app' else
                   'module Loc;\n' + HDR + PREAMBLE + GENERIC_LIB)
        else:
            src = ('module App;\nimport LibX;\n' + HDR + helpers(k) if sc == 'app' else
                   'module Loc;\n' + HDR + PREAMBLE + mclass([k]) + helpers(k))
        _write(f'{progdir}/{name}.bzl', src + main_src(body, iterations))
    return cs


def _write(path, text):
    with open(path, 'w') as f:
        f.write(text)


# ---------------------------------------------------------------------------
# Runner / classifier
# ---------------------------------------------------------------------------

LEAK_RE = re.compile(r'(\d+) byte\(s\) leaked in (\d+) allocation')
GLIBC_RE = re.compile(r'free\(\): |double free|invalid pointer|corrupted|Segmentation')


def classify(compiled, rc, err, compile_msg=''):
    """-> (class, detail). `err` is the program's stderr."""
    if not compiled:
        return CF, compile_msg
    m = re.search(r'ERROR: AddressSanitizer: ([A-Za-z_-]+)', err)
    if m:
        return CRASH, m.group(1)
    if GLIBC_RE.search(err):
        return CRASH, 'glibc'
    if 'ERROR: LeakSanitizer' in err:
        sm = re.search(r'SUMMARY: .*?' + LEAK_RE.pattern, err)
        lm = sm or LEAK_RE.search(err)
        b, a = (lm.group(1), lm.group(2)) if lm else ('?', '?')
        return LEAK, f'{b}B/{a}'
    if rc != 0:
        return CRASH, f'rc={rc}'
    return CLEAN, ''


def run_cell(bezelc, odir, progdir, name, has_lib, timeout, run_timeout):
    prog = f'{progdir}/{name}.bzl'
    exe = f'{odir}/{name}.exe'
    cmd = [bezelc, '--sanitize=address']
    if name.startswith('app_') and has_lib:
        cmd += ['--import', f'{odir}/LibX.o']
    cmd += ['-o', exe, prog]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, errors='replace')
        ok, clog = p.returncode == 0, p.stdout + p.stderr
    except subprocess.TimeoutExpired:
        ok, clog = False, 'compile timeout'
    _write(f'{odir}/{name}.clog', clog)
    if not ok:
        first = next((ln for ln in clog.splitlines() if ln.strip() and 'warning' not in ln), '')
        first = re.sub(r'^[^ ]*:\d+:\d+: ', '', first)[:140]
        return name, CF, first, ''
    try:
        r = subprocess.run([exe], capture_output=True, text=True, timeout=run_timeout, errors='replace')
        rc, out, err = r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        rc, out, err = 124, '', 'timeout'
    _write(f'{odir}/{name}.out', out)
    _write(f'{odir}/{name}.err', err)
    cls, det = classify(True, rc, err)
    return name, cls, det, uniq_c(out)


def uniq_c(text):
    """`uniq -c` rendering, '|'-joined: stable, short stdout fingerprint."""
    parts, prev, n = [], None, 0
    for ln in text.splitlines():
        if ln == prev:
            n += 1
        else:
            if prev is not None:
                parts.append(f'{n} {prev}')
            prev, n = ln, 1
    if prev is not None:
        parts.append(f'{n} {prev}')
    return '|'.join(parts)


def run_compiler(tag, bezelc, outdir, progdir, names, jobs, timeout, run_timeout):
    odir = f'{outdir}/{tag}'
    os.makedirs(odir, exist_ok=True)
    lib = subprocess.run([bezelc, '-c', '-o', f'{odir}/LibX.o', f'{progdir}/LibX.bzl'],
                         capture_output=True, text=True, errors='replace')
    _write(f'{odir}/LibX.clog', lib.stdout + lib.stderr)
    has_lib = lib.returncode == 0
    if not has_lib:
        print(f'[{tag}] LIBRARY COMPILE FAIL (app_ cells will be CF): ' +
              next((ln for ln in (lib.stdout + lib.stderr).splitlines() if 'error' in ln.lower()), ''),
              file=sys.stderr)
    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        futs = [ex.submit(run_cell, bezelc, odir, progdir, n, has_lib, timeout, run_timeout) for n in names]
        for f in futs:
            rows.append(f.result())
    rows.sort()
    with open(f'{outdir}/{tag}.tsv', 'w') as f:
        for r in rows:
            f.write('\t'.join(r) + '\n')
    return {r[0]: r[1:] for r in rows}


# ---------------------------------------------------------------------------
# Comparison
# ---------------------------------------------------------------------------

def _leak(detail):
    m = re.match(r'(\d+)B/(\d+)', detail)
    return (int(m.group(1)), int(m.group(2))) if m else (0, 0)


def worse(prev, cur):
    """Why `cur` is worse than `prev` (cells are (class, detail, stdout)), or ''.
    A reference that never ran to completion (CF/CRASH) cannot be regressed from."""
    tp, tc = TIER[prev[0]], TIER[cur[0]]
    if tp < TIER[LEAK]:
        return ''
    if tc < tp:
        return f'{cur[0]} {cur[1]}'.strip()
    if cur[2] != prev[2]:
        return f'stdout {prev[2][:40]!r} -> {cur[2][:40]!r}'
    if tc == TIER[LEAK]:
        (pb, pa), (cb, ca) = _leak(prev[1]), _leak(cur[1])
        if cb > pb or ca > pa:
            return f'MORE-LEAK {prev[1]} -> {cur[1]}'
    return ''


def compare(tags, tables):
    """-> (report text, number of worse cells). Each later tag vs every earlier tag."""
    lines = []
    keys = sorted(set().union(*[t.keys() for t in tables]))
    for tag, t in zip(tags, tables):
        c = {k: 0 for k in TIER}
        for k in keys:
            c[t.get(k, (CF,))[0]] += 1
        lines.append(f'{tag}: CF {c[CF]}  CRASH {c[CRASH]}  LEAK {c[LEAK]}  CLEAN {c[CLEAN]}')
    nworse = 0
    for j in range(1, len(tags)):
        for i in range(j):
            bad = []
            for k in keys:
                if k not in tables[i]:
                    continue
                w = worse(tuple(tables[i][k]), tuple(tables[j].get(k, (CF, 'missing', ''))))
                if w:
                    bad.append((k, w))
            nworse += len(bad)
            lines.append(f'\n== {tags[j]} vs {tags[i]}: {len(bad)} cell(s) WORSE')
            for k, w in bad:
                p = tables[i][k]
                lines.append(f'  {k:34s} {tags[i]}={p[0]}{"/" + p[1] if p[1] else ""}  ->  {tags[j]}: {w}')
            better = [k for k in keys if k in tables[i] and k in tables[j]
                      and TIER[tables[j][k][0]] > TIER[tables[i][k][0]]]
            lines.append(f'== {tags[j]} vs {tags[i]}: {len(better)} cell(s) better (class rose)')
    return '\n'.join(lines), nworse


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--bezelc', action='append', default=[], metavar='TAG=PATH',
                    help='compiler under test; repeat, earliest (reference) first')
    ap.add_argument('--out', help='output directory (all scratch goes here)')
    ap.add_argument('--jobs', type=int, default=4, help='parallel cells (default 4)')
    ap.add_argument('--filter', default='', help='comma-separated substrings matched against '
                    'type key, type text (e.g. Set, SortedSet, Mutex) and op name; any match selects')
    ap.add_argument('--scope', choices=('loc', 'app'), help='only same-module or only cross-module cells')
    ap.add_argument('--iterations', type=int, default=100, help='loop count inside each program')
    ap.add_argument('--timeout', type=int, default=300, help='per-cell compile timeout (s)')
    ap.add_argument('--run-timeout', type=int, default=60, help='per-cell run timeout (s)')
    ap.add_argument('--list', action='store_true', help='print the selected cell names and exit')
    ap.add_argument('--generate-only', action='store_true', help='write the programs and exit')
    a = ap.parse_args(argv)

    if a.list:
        for c in cells(a.filter, a.scope):
            print(c[0])
        return 0
    if not a.out:
        ap.error('--out is required')
    progdir = os.path.join(a.out, 'programs')
    cs = generate(progdir, a.filter, a.scope, a.iterations)
    if not cs:
        print('no cells selected by --filter/--scope', file=sys.stderr)
        return 2
    print(f'{len(cs)} programs in {progdir}')
    if a.generate_only:
        return 0
    tags, paths = [], []
    for spec in a.bezelc:
        tag, _, path = spec.partition('=')
        if not tag or not path or not os.access(path, os.X_OK) or tag in tags:
            print(f'bad --bezelc {spec!r} (want unique TAG=EXECUTABLE)', file=sys.stderr)
            return 2
        tags.append(tag)
        paths.append(os.path.abspath(path))
    if len(tags) < 1:
        print('need at least one --bezelc TAG=PATH (two or more to compare)', file=sys.stderr)
        return 2
    tables = []
    for tag, path in zip(tags, paths):
        print(f'[{tag}] running {len(cs)} cells ...', flush=True)
        tables.append(run_compiler(tag, path, a.out, progdir, [c[0] for c in cs],
                                   a.jobs, a.timeout, a.run_timeout))
    report, nworse = compare(tags, tables)
    _write(f'{a.out}/report.txt', report + '\n')
    print(report)
    if nworse:
        print(f'\nFAIL: {nworse} cell(s) where a later compiler is worse', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
