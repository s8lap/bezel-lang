//! case: l5_9957
//! doc: stdlib
//! section: 5
//! title: the predicate terminals over a Dictionary with int keys and string values answer the matching
//!   entry
//! rule: `First(pred)` and `Last(pred)` yield the first and last matching element, and `Single(pred)`
//!   the sole matching element.
//! cites: 5, 4.8
//! expect: run
//! output: 2 2 3 2 1
//! notes: Pins #9261 (int keys). Entries 1 -> "a", 2 -> "b", 3 -> "c" in insertion order. First(Key > 1)
//!   is entry 2, Last(Key < 3) is entry 2, Single(Key = 3) is entry 3, FirstOrNull(Key > 1) is
//!   entry 2, SingleOrNull(Key = 1) is entry 1.
module Conf;
import System;
public class Program
  public static method Main()
    using var d := new Dictionary<int, string>();
    d[1] := "a";
    d[2] := "b";
    d[3] := "c";
    var f := d.First(lambda(e) => e.Key > 1);
    var l := d.Last(lambda(e) => e.Key < 3);
    var s := d.Single(lambda(e) => e.Key = 3);
    var fo := d.FirstOrNull(lambda(e) => e.Key > 1);
    var so := d.SingleOrNull(lambda(e) => e.Key = 1);
    if fo <> null and so <> null then
      Console.WriteLine($"{f.Key} {l.Key} {s.Key} {fo.Key} {so.Key}");
    end;
  end Main;
end Program;
