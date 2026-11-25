//! case: l5_9955
//! doc: stdlib
//! section: 5
//! title: the predicate terminals over a SortedDictionary honour ascending key order, for string and int
//!   keys
//! rule: iteration visits entries in ascending key order
//! cites: 5, 4.17
//! expect: run
//! output: a z a m
//!   2 9 t n
//! notes: Pins #9261 (SortedDictionary receiver). Keys z=1, a=2, m=3 iterate a, m, z: First(true) is a,
//!   Last(true) is z, FirstOrNull(Value > 1) is a (value 2), LastOrNull(Value > 2) is m (the only
//!   entry with value 3). Keys 9, 2, 5 iterate 2, 5, 9: First is 2 (t), Last is 9 (n). Single is
//!   not used (#9290).
module Conf;
import System;
public class Program
  public static method Main()
    using var sd := new SortedDictionary<string, int>();
    sd["z"] := 1;
    sd["a"] := 2;
    sd["m"] := 3;
    var f := sd.First(lambda(e) => true);
    var l := sd.Last(lambda(e) => true);
    var fo := sd.FirstOrNull(lambda(e) => e.Value > 1);
    var lo := sd.LastOrNull(lambda(e) => e.Value > 2);
    var foKey := "none";
    if fo <> null then foKey := fo.Key; end;
    var loKey := "none";
    if lo <> null then loKey := lo.Key; end;
    Console.WriteLine($"{f.Key} {l.Key} {foKey} {loKey}");
    using var si := new SortedDictionary<int, string>();
    si[9] := "n";
    si[2] := "t";
    si[5] := "f";
    var g := si.First(lambda(e) => true);
    var h := si.Last(lambda(e) => true);
    Console.WriteLine($"{g.Key} {h.Key} {g.Value} {h.Value}");
  end Main;
end Program;
