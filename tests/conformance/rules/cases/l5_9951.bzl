//! case: l5_9951
//! doc: stdlib
//! section: 5
//! title: the OrNull predicate forms over a Dictionary answer the matching entry, or null when none
//!   matches
//! rule: `FirstOrNull(pred)`/`LastOrNull(pred)`/`SingleOrNull(pred)` return `null` when none matches
//! cites: 5, 4.8
//! expect: run
//! output: FON b 9
//!   LON c 12
//!   SON c 12
//!   FON0 none
//!   LON0 none
//!   SON0 none
//! notes: Pins #9261. Entries a=1, b=9, c=12 in insertion order. Value > 5 matches b and c: FirstOrNull
//!   is b 9, LastOrNull is c 12; Value > 10 matches only c, so SingleOrNull answers it (exactly one
//!   match). Value > 100 matches nothing: all three answer null. The several-match SingleOrNull is
//!   deliberately not covered (#9287).
module Conf;
import System;
public class Program
  public static method Main()
    using var d := new Dictionary<string, int>();
    d["a"] := 1;
    d["b"] := 9;
    d["c"] := 12;
    var thr := 5;
    var fo := d.FirstOrNull(lambda(e) => e.Value > thr);
    if fo = null then Console.WriteLine("FON none"); else Console.WriteLine($"FON {fo.Key} {fo.Value}"); end;
    var lo := d.LastOrNull(lambda(e) => e.Value > thr);
    if lo = null then Console.WriteLine("LON none"); else Console.WriteLine($"LON {lo.Key} {lo.Value}"); end;
    var so := d.SingleOrNull(lambda(e) => e.Value > 10);
    if so = null then Console.WriteLine("SON none"); else Console.WriteLine($"SON {so.Key} {so.Value}"); end;
    var fo2 := d.FirstOrNull(lambda(e) => e.Value > 100);
    if fo2 = null then Console.WriteLine("FON0 none"); else Console.WriteLine("FON0 found"); end;
    var lo2 := d.LastOrNull(lambda(e) => e.Value > 100);
    if lo2 = null then Console.WriteLine("LON0 none"); else Console.WriteLine("LON0 found"); end;
    var so2 := d.SingleOrNull(lambda(e) => e.Value > 100);
    if so2 = null then Console.WriteLine("SON0 none"); else Console.WriteLine("SON0 found"); end;
  end Main;
end Program;
