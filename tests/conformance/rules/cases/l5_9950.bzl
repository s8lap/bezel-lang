//! case: l5_9950
//! doc: stdlib
//! section: 5
//! title: First, Last and Single with a predicate over a Dictionary answer the matching entry
//! rule: `First(pred)` and `Last(pred)` yield the first and last matching element, and `Single(pred)`
//!   the sole matching element.
//! cites: 5, 4.8
//! expect: run
//! output: First b 9
//!   Last c 12
//!   Single c 12
//! notes: Pins #9261. A Dictionary iterates as DictionaryEntry values in insertion order (stdlib 4.8),
//!   so the entries are a=1, b=9, c=12. With thr = 5 the matches of Value > thr are b and c: First
//!   is b 9, Last is c 12. Value > 10 matches only c, the sole match for Single.
module Conf;
import System;
public class Program
  public static method Main()
    using var d := new Dictionary<string, int>();
    d["a"] := 1;
    d["b"] := 9;
    d["c"] := 12;
    var thr := 5;
    var f := d.First(lambda(e) => e.Value > thr);
    Console.WriteLine($"First {f.Key} {f.Value}");
    var l := d.Last(lambda(e) => e.Value > thr);
    Console.WriteLine($"Last {l.Key} {l.Value}");
    var s := d.Single(lambda(e) => e.Value > 10);
    Console.WriteLine($"Single {s.Key} {s.Value}");
  end Main;
end Program;
