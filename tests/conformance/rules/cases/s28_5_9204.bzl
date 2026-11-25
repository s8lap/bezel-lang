//! case: s28_5_9204
//! doc: spec
//! section: 28.5
//! title: `owned` sorted, choice-valued and nullable-record `Dictionary` fields are disposed with the holder
//! rule: **Auto-disposes** the field when the containing object is disposed
//! cites: 28.9, 28.11
//! expect: run
//! output: 400
//! notes: Pins #9183 (fix f9f8a62cf): an `owned` Dictionary field with a RECORD value frees the value box at
//!   the holder disposal. The case is an output-only control (green on both arms): the corpus has no leak
//!   leg, so it guards the fix against double free, use-after-free and over-freeing, not the leak itself.
//!   Overwrite, Remove and set-to-null over the three fields; `_s[1].A` is 2 after the overwrite, times
//!   200 = 400.
module Conf;
public record P(A: int, B: int);
public choice Shape Circle(r: int), Rect(w: int, h: int), Named(label: string); end Shape;
public class Holder
  private owned _s: SortedDictionary<int, P> := new SortedDictionary<int, P>();
  private owned _c: Dictionary<string, Shape> := new Dictionary<string, Shape>();
  private owned _n: Dictionary<string, P?> := new Dictionary<string, P?>();
  public method Put(i: int)
    _s[i] := new P(i, i);
    _s[i] := new P(i + 1, i);
    _c["a"] := Shape.Named("lbl" + i.ToString());
    _c["a"] := Shape.Circle(i);
    _c["b"] := Shape.Named("zz");
    _n["q"] := new P(1, 2);
    _n["q"] := null;
    _n["r"] := new P(3, 4);
    _c.Remove("b");
  end Put;
  public method Sum(): int => _s[1].A;
end Holder;
public class Program
  public static method Main()
    var i := 0; var t := 0;
    while i < 200 do
      using var h := new Holder();
      h.Put(1);
      t := t + h.Sum();
      i := i + 1;
    end;
    Console.WriteLine(t);
  end Main;
end Program;
