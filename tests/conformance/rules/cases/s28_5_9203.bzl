//! case: s28_5_9203
//! doc: spec
//! section: 28.5
//! title: an `owned` `Dictionary<int, T>` field of a generic holder is disposed with its holder
//! rule: **Auto-disposes** the field when the containing object is disposed
//! cites: 28.9, 28.11
//! expect: run
//! output: 1400
//! notes: Pins #9183 (fix f9f8a62cf): an `owned` Dictionary field with a RECORD value frees the value box at
//!   the holder disposal. The case is an output-only control (green on both arms): the corpus has no leak
//!   leg, so it guards the fix against double free, use-after-free and over-freeing, not the leak itself.
//!   `H<Entry>` then `H<int>`: after an overwrite Get(1).N = 3 and g.Get(1) = 4, so 7 per iteration,
//!   1400.
module Conf;
public record Entry(Name: string, N: int);
public class H<T>
  private owned _d: Dictionary<int, T> := new Dictionary<int, T>();
  public method Put(k: int, v: T) => _d[k] := v;
  public method Get(k: int): T => _d[k];
end H;
public class Program
  public static method Main()
    var i := 0;
    var t := 0;
    while i < 200 do
      using var hs := new Array<H<Entry>>();
      var h := new H<Entry>();
      h.Put(1, new Entry("x", 2)); h.Put(1, new Entry("y", 3));
      t := t + h.Get(1).N;
      h.Dispose();
      var g := new H<int>();
      g.Put(1, 4);
      t := t + g.Get(1);
      i := i + 1;
    end;
    Console.WriteLine(t);
  end Main;
end Program;
