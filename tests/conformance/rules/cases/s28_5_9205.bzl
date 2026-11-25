//! case: s28_5_9205
//! doc: spec
//! section: 28.5
//! title: records read out of an `owned` `Dictionary` field outlive the holder that is disposed
//! rule: **Auto-disposes** the field when the containing object is disposed
//! cites: 28.9, 28.11
//! expect: run
//! output: 41400
//!  204
//! notes: Pins #9183 (fix f9f8a62cf): an `owned` Dictionary field with a RECORD value frees the value box at
//!   the holder disposal. The case is an output-only control (green on both arms): the corpus has no leak
//!   leg, so it guards the fix against double free, use-after-free and over-freeing, not the leak itself.
//!   Per outer i: two holders each Put(i) (final `_d[i] = Pt(i + 5, i)`), `Try(i)` = i + 5, `Try(-5)` =
//!   -1, so each holder adds i + 4: 2i + 8; summed over i = 0..199 = 39800 + 1600 = 41400. `kept` is a
//!   copy of the last Get: `kept.A` = 199 + 5 = 204.
module Conf;
public record Pt(A: int, B: int);
public class Holder
  private owned _d: Dictionary<int, Pt> := new Dictionary<int, Pt>();
  private owned _e: Dictionary<string, Pt?> := new Dictionary<string, Pt?>();
  public method Put(k: int) 
    _d[k] := new Pt(k, k + 1);
    _d[k] := new Pt(k + 5, k);
    _e["x"] := new Pt(k, 1);
  end Put;
  public method Get(k: int): Pt => _d[k];
  public method Try(k: int): int
    var p: Pt? := null;
    if _d.TryGetValue(k, out p) then return p!.A; end;
    return -1;
  end Try;
  public method Rem(k: int) => _d.Remove(k);
end Holder;
public class Program
  public static method Main()
    var t := 0;
    var kept := new Pt(0, 0);
    var i := 0;
    while i < 200 do
      using var hs := new OwningArray<Holder>();
      hs.Add(new Holder());
      hs.Add(new Holder());
      var j := 0;
      while j < hs.Count do
        hs[j].Put(i);
        kept := hs[j].Get(i);
        t := t + hs[j].Try(i) + hs[j].Try(-5);
        if j = 0 then hs[j].Rem(i); end;
        j := j + 1;
      end;
      i := i + 1;
    end;
    Console.WriteLine(t);
    Console.WriteLine(kept.A);
  end Main;
end Program;
