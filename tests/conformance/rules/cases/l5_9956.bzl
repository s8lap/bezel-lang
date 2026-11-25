//! case: l5_9956
//! doc: stdlib
//! section: 5
//! title: the predicate terminals over string-valued Dictionary entries with a captured local answer the
//!   entry
//! rule: `First(pred)` and `Last(pred)` yield the first and last matching element, and `Single(pred)`
//!   the sole matching element.
//! cites: 5, 4.8
//! expect: run
//! output: kb kc kb kb kb kb
//!   none
//!   Single raised
//!   First raised
//! notes: Pins #9261 (string-valued entries, captured local n = 2). Entries ka -> "x", kb -> "yy", kc ->
//!   "zzz" in insertion order. Length = 2 matches only kb: First, Single, FirstOrNull, LastOrNull
//!   and SingleOrNull answer kb; Last(Length >= 2) answers kc. SingleOrNull(Length = 9) answers
//!   null. Single(Length > 1) matches kb and kc and raises; First(Length = 9) matches nothing and
//!   raises.
module Conf;
import System;
public class Program
  public static method Main()
    using var d := new Dictionary<string, string>();
    d["ka"] := "x";
    d["kb"] := "yy";
    d["kc"] := "zzz";
    var n := 2;
    var f := d.First(lambda(e) => e.Value.Length = n);
    var l := d.Last(lambda(e) => e.Value.Length >= n);
    var s := d.Single(lambda(e) => e.Value.Length = n);
    var fo := d.FirstOrNull(lambda(e) => e.Value.Length = n);
    var lo := d.LastOrNull(lambda(e) => e.Value.Length = n);
    var so := d.SingleOrNull(lambda(e) => e.Value.Length = n);
    var none := d.SingleOrNull(lambda(e) => e.Value.Length = 9);
    if fo <> null and lo <> null and so <> null then
      Console.WriteLine($"{f.Key} {l.Key} {s.Key} {fo.Key} {lo.Key} {so.Key}");
    end;
    if none = null then Console.WriteLine("none"); else Console.WriteLine("found"); end;
    try
      var x := d.Single(lambda(e) => e.Value.Length > 1);
      Console.WriteLine("Single returned");
    catch ex: InvalidOperationException =>
      Console.WriteLine("Single raised");
    end;
    try
      var y := d.First(lambda(e) => e.Value.Length = 9);
      Console.WriteLine("First returned");
    catch ex: InvalidOperationException =>
      Console.WriteLine("First raised");
    end;
  end Main;
end Program;
