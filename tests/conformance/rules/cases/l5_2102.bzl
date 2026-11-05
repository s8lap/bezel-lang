//! case: l5_2102
//! doc: stdlib
//! section: 5
//! title: ElementAtOrNull on a SortedDictionary reads entries in key order
//!   across 200 iterations
//! rule: public method ElementAtOrNull(index: int): T?; // — null when
//!   index is out of range
//! cites: 4.17, 5
//! expect: run
//! output: 7136
//! issue: 8232
//! notes: Red on base: the pre-fix compiler rejected ElementAtOrNull on a
//!   concrete collection with 'no member ElementAtOrNull'. It passes on the
//!   staging tip.
module Conf;

public class Program
  public static method Main()
    using var d := new SortedDictionary<string, int>();
    d.Add("b", 2);
    d.Add("a", 1);
    var t := 0;
    for var i in 0..<200 do
      var e := d.ElementAtOrNull(i mod 3);
      if e <> null then
        t := t + e.Key.Length + e.Value;
      end;
      var s := e?.Value ?? 100;
      t := t + s;
    end for;
    Console.WriteLine(t);
  end Main;
end Program;
