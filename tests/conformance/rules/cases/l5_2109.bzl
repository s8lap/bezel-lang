//! case: l5_2109
//! doc: stdlib
//! section: 5
//! title: ElementAtOrNull in a while loop that reassigns its own nullable
//!   local
//! rule: public method ElementAtOrNull(index: int): T?; // — null when
//!   index is out of range
//! cites: 4.8, 5
//! expect: run
//! output: 660
//! issue: 8232
//! notes: Red on base: the pre-fix compiler rejected ElementAtOrNull on a
//!   concrete collection with 'no member ElementAtOrNull'. It passes on the
//!   staging tip.
module Conf;

public class Program
  public static method Main()
    using var d := new Dictionary<int, int>();
    d.Add(1, 10);
    d.Add(2, 20);
    d.Add(3, 30);
    var i := 0;
    var e := d.ElementAtOrNull(i);
    var t := 0;
    while e <> null do
      t := t + e.Key * 100 + e.Value;
      i := i + 1;
      e := d.ElementAtOrNull(i);
    end while;
    Console.WriteLine(t);
  end Main;
end Program;
