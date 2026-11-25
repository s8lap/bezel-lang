//! case: s7_5_9004
//! doc: spec
//! section: 7.5
//! title: the `?.` spelling of Dictionary's declared Count property is admitted
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 26.5
//! expect: run
//! output: 1
//! notes: Companion of s7_5_9001..9003: a documented member of a dictionary head through `?.`. Stdlib 4.8
//!   declares `Count` on `Dictionary<K, V>`. Trace: one entry added; `q?.Count` is 1.
module Conf;
public class Program
  public static method Main()
    using var d := new Dictionary<int, int>();
    d.Add(1, 10);
    var q: Dictionary<int, int>? := d;
    Console.WriteLine(q?.Count ?? -1);
  end Main;
end Program;
