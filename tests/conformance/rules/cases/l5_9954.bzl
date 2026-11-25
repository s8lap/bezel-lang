//! case: l5_9954
//! doc: stdlib
//! section: 5
//! title: the predicate terminals over a Dictionary with class values answer the matching entry, 200x
//! rule: `First(pred)` and `Last(pred)` yield the first and last matching element, and `Single(pred)`
//!   the sole matching element.
//! cites: 5, 4.8
//! expect: run
//! output: total 3160
//! notes: Pins #9261 (class-valued entries). Per pass i the dictionary holds a -> Box("x" + i) and b ->
//!   Box("yy" + i). First(Key = a), Single(Key = a) and SingleOrNull(Key = a) answer the a entry;
//!   FirstOrNull(Key = b), LastOrNull(Key = b) and Last(Value.V.Length > 0) (last in insertion
//!   order) answer the b entry. The pass adds len(x+i) + len(yy+i) + len(x+i) + len(yy+i) = 6 + 4 *
//!   digits(i). Over i in 0..199 (digits: 10 x 1, 90 x 2, 100 x 3, sum 490): 6 * 200 + 4 * 490 =
//!   1200 + 1960 = 3160.
module Conf;
import System;
public class Box
  public V: string := "";
  public constructor(v: string)
    V := v;
  end constructor;
end Box;
public class Program
  public static method Main()
    var i := 0;
    var total := 0;
    while i < 200 do
      var b0 := new Box($"x{i}");
      var b1 := new Box($"yy{i}");
      using var d := new Dictionary<string, Box>();
      d["a"] := b0;
      d["b"] := b1;
      var f := d.First(lambda(e) => e.Key = "a");
      var fo := d.FirstOrNull(lambda(e) => e.Key = "b");
      var lo := d.LastOrNull(lambda(e) => e.Key = "b");
      var so := d.SingleOrNull(lambda(e) => e.Key = "a");
      var s := d.Single(lambda(e) => e.Key = "a");
      var l := d.Last(lambda(e) => e.Value.V.Length > 0);
      if fo <> null and lo <> null and so <> null then
        total := total + f.Value.V.Length + fo.Value.V.Length + s.Value.V.Length + l.Value.V.Length;
      end;
      i := i + 1;
    end;
    Console.WriteLine($"total {total}");
  end Main;
end Program;
