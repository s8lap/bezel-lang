#!/usr/bin/env python3
"""bzlprint — print a bzcore (L1) model program as a real, self-contained Bezel module.

The rendering follows docs/research/soundness/bzcore-l1/README.md §3 ("printer interface"):

  * two classes: `R` (sealed, derives from `Exception` so that `throw x` type-checks, with one
    `public owned f: Q` field initialised by a constructor taking `owned q: Q`) and the leaf `Q`;
    both carry an instrumented `destructor` printing `D:<id>`, a constructor printing `N:<id>`
    and a `Use()` method printing `U:<id>`.  Ids are minted at run time from a static counter
    (`Ids.Next()`), in allocation order: `new R(new Q())` mints the `Q` first, as the model does;
  * the body is `public static method Body()` of class `R` (so `x.f` is read by the declaring
    type, §28.6 / K9.3);
  * `*`-branches and loop counts are driven by a command-line digit string: the k-th
    nondeterministic choice met at run time reads the k-th digit (`Ids.Choose()`), so one module
    serves every path; the model's `run_all` choice vector maps onto the digits 1:1;
  * `send x` is an owned-argument call `R.SinkR(move x)` / `R.SinkQ(move x)` into a static
    holder (`static owned readonly OwningArray<…>`), so the object is transferred, never
    disposed and never flagged — the model's "foreign" sink; the sink touches no member of the
    object (a disposed object may be sent, §28.2).  A real `Domain.Spawn` is NOT used:
    its `owned` row demands a sendable (sealed, transfer-closed) class and `Exception`-derived
    classes are not sendable on r35, which would make `throw` and `send` unprintable on the same
    class.  Each send site prints a bare `S` first; the harness aligns the k-th `S` with the
    model path's k-th send event, and a later `U:<id>` of an object sent so far is the
    compiler-side image of the model's RACE (a read of an object another domain owns).
  * `catch e` is `catch var e: R => do … end,`; `throw x` throws the `R`.

The module ends with `Program.Main(args)` which prints `END` after `Body()` returns normally.
"""
import sys

PREAMBLE = '''module Probe;

public static class Ids
  private static _n: int := 0;
  private static _code: int := 0;
  private static _len: int := 0;
  private static _pos: int := 0;
  public static method Next(): int
    _n := _n + 1;
    return _n;
  end Next;
  public static method Init(args: Array<string>)
    if args.Count > 0 then
      _len := args[0].Length;
      _code := int.Parse(args[0]);
    end;
  end Init;
  public static method Choose(): int
    if _pos < _len then
      var p := 1;
      var k := _len - 1 - _pos;
      while k > 0 do
        p := p * 10;
        k := k - 1;
      end;
      _pos := _pos + 1;
      return (_code / p) mod 10;
    end;
    return 0;
  end Choose;
end Ids;

public sealed class Q
  private _id: int;
  public constructor()
    _id := Ids.Next();
    Console.WriteLine($"N:{_id}");
  end constructor;
  destructor
    Console.WriteLine($"D:{_id}");
  end destructor;
  public method Use()
    Console.WriteLine($"U:{_id}");
  end Use;
  public method Id(): int => _id;
end Q;

public sealed class R: Exception
  private _id: int;
  public owned f: Q;
  private static owned readonly _keepR: OwningArray<R> := new OwningArray<R>();
  private static owned readonly _keepQ: OwningArray<Q> := new OwningArray<Q>();
  public constructor(owned q: Q)
    base("R");
    _id := Ids.Next();
    Console.WriteLine($"N:{_id}");
    f := q;
  end constructor;
  destructor
    Console.WriteLine($"D:{_id}");
  end destructor;
  public method Use()
    Console.WriteLine($"U:{_id}");
  end Use;
  public method Id(): int => _id;
  public static method SinkR(owned r: R)
    _keepR.Add(move r);
  end SinkR;
  public static method SinkQ(owned q: Q)
    _keepQ.Add(move q);
  end SinkQ;
  public static method Body()
'''

POSTAMBLE = '''  end Body;
end R;

public class Program
  public static method Main(args: Array<string>): int
    Ids.Init(args);
    R.Body();
    Console.WriteLine("END");
    return 0;
  end Main;
end Program;
'''

BODY_FIRST_LINE = PREAMBLE.count('\n') + 1   # 1-based line number of the first body statement

NEW_R = 'new R(new Q())'
NEW_Q = 'new Q()'


def fresh(t):
    return NEW_R if t == 'R' else NEW_Q


class Printer:
    """Tracks the static type (R | Q) of every binding exactly as the model's enumerator does."""

    def __init__(self):
        self.lines = []
        self.loop_n = 0

    def emit(self, ind, text):
        self.lines.append(ind + text)

    def body(self, prog, types, ind):
        types = dict(types)
        for s in prog:
            k = s[0]
            if k == 'new':
                self.emit(ind, f'var {s[1]} := {NEW_R};'); types[s[1]] = 'R'
            elif k == 'decl':
                self.emit(ind, f'var {s[1]}: R;'); types[s[1]] = 'R'
            elif k == 'using':
                self.emit(ind, f'using var {s[1]} := {NEW_R};'); types[s[1]] = 'R'
            elif k == 'brw':
                self.emit(ind, f'var {s[1]} := {s[2]};'); types[s[1]] = types[s[2]]
            elif k == 'move':
                self.emit(ind, f'var {s[1]} := move {s[2]};'); types[s[1]] = types[s[2]]
            elif k == 'usingm':
                self.emit(ind, f'using var {s[1]} := move {s[2]};'); types[s[1]] = types[s[2]]
            elif k == 'usingb':
                self.emit(ind, f'using var {s[1]} := {s[2]};'); types[s[1]] = types[s[2]]
            elif k == 'asg':
                self.emit(ind, f'{s[1]} := {fresh(types[s[1]])};')
            elif k == 'asgb':
                self.emit(ind, f'{s[1]} := {s[2]};')
            elif k == 'asgm':
                self.emit(ind, f'{s[1]} := move {s[2]};')
            elif k == 'disp':
                self.emit(ind, f'{s[1]}.Dispose();')
            elif k == 'send':
                self.emit(ind, 'Console.WriteLine("S");')      # the send site marker (no member access: a disposed object may be sent)
                self.emit(ind, f'R.Sink{types[s[1]]}(move {s[1]});')
            elif k == 'use':
                self.emit(ind, f'{s[1]}.Use();')
            elif k == 'fst':
                self.emit(ind, f'{s[1]}.f := {NEW_Q};')
            elif k == 'fstm':
                self.emit(ind, f'{s[1]}.f := {s[2]};')
            elif k == 'fbrw':
                self.emit(ind, f'var {s[1]} := {s[2]}.f;'); types[s[1]] = 'Q'
            elif k == 'fmove':
                self.emit(ind, f'var {s[1]} := move {s[2]}.f with {NEW_Q};'); types[s[1]] = 'Q'
            elif k == 'tuse':
                self.emit(ind, f'{NEW_R}.Use();')
            elif k == 'tdisp':
                self.emit(ind, f'{NEW_R}.Dispose();')
            elif k == 'tsend':
                self.emit(ind, 'Console.WriteLine("S");')
                self.emit(ind, f'R.SinkR({NEW_R});')
            elif k == 'tbrw':
                self.emit(ind, f'var {s[1]} := {NEW_R}.f;'); types[s[1]] = 'Q'
            elif k == 'throw':
                self.emit(ind, f'throw {s[1]};')
            elif k == 'tthrow':
                self.emit(ind, f'throw {NEW_R};')
            elif k == 'if':
                self.emit(ind, 'if Ids.Choose() = 0 then')
                self.body(s[1], types, ind + '  ')
                self.emit(ind, 'else')
                self.body(s[2], types, ind + '  ')
                self.emit(ind, 'end;')
            elif k == 'block':
                self.emit(ind, 'do')
                self.body(s[1], types, ind + '  ')
                self.emit(ind, 'end;')
            elif k == 'try':
                self.emit(ind, 'try')
                self.body(s[1], types, ind + '  ')
                self.emit(ind, 'catch')
                self.emit(ind, f'  var {s[2]}: R => do')
                t2 = dict(types); t2[s[2]] = 'R'
                self.body(s[3], t2, ind + '    ')
                self.emit(ind, '  end,')
                self.emit(ind, 'end;')
            elif k == 'while':
                self.loop_n += 1
                n = f'n{self.loop_n}'; i = f'i{self.loop_n}'
                self.emit(ind, f'var {n} := Ids.Choose();')
                self.emit(ind, f'var {i} := 0;')
                self.emit(ind, f'while {i} < {n} do')
                self.emit(ind + '  ', f'{i} := {i} + 1;')
                self.body(s[1], types, ind + '  ')
                self.emit(ind, 'end;')
            else:
                raise ValueError(f'unknown statement {s}')


def render(prog):
    p = Printer()
    p.body(prog, {}, '    ')
    return PREAMBLE + '\n'.join(p.lines) + ('\n' if p.lines else '') + POSTAMBLE


def body_lines(prog):
    """The rendered body only (for listings)."""
    p = Printer(); p.body(prog, {}, ''); return p.lines


if __name__ == '__main__':
    sys.path.insert(0, sys.argv[1] if len(sys.argv) > 1 else '../bzcore-l1')
    import bzcore
    for title, prog, *_ in bzcore.SELFTEST[:3]:
        print('//', title); print(render(prog))
