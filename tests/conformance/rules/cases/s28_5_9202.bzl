//! case: s28_5_9202
//! doc: spec
//! section: 28.5
//! title: a NON-owned `Dictionary` field is not disposed with its holder
//! rule: A field marked `owned` belongs to the containing object exclusively
//! cites: 28.5, 28.11
//! expect: run
//! output: nz
//! notes: Pins #9183 (fix f9f8a62cf): an `owned` Dictionary field with a RECORD value frees the value box at
//!   the holder disposal. The case is an output-only control (green on both arms): the corpus has no leak
//!   leg, so it guards the fix against double free, use-after-free and over-freeing, not the leak itself.
//!   Counter-control: the field is a back-reference, so disposing the holder leaves the dictionary
//!   readable: `d2["z"].Name` = `nz`.
module Conf;
public record Entry(Name: string, N: int);
public class Holder
  private _d: Dictionary<string, Entry>;
  public constructor(d: Dictionary<string, Entry>) _d := d; end;
  public method Put(k: string) => _d[k] := new Entry("n" + k, 1);
end Holder;
public class Program
  public static method Main()
    var i := 0;
    while i < 200 do
      using var d := new Dictionary<string, Entry>();
      using var h := new Holder(d);
      h.Put("a"); h.Put("b");
      i := i + 1;
    end;
    using var d2 := new Dictionary<string, Entry>();
    using var h2 := new Holder(d2);
    h2.Put("z");
    h2.Dispose();
    Console.WriteLine(d2["z"].Name);
  end Main;
end Program;
