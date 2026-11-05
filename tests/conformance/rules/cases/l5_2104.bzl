//! case: l5_2104
//! doc: stdlib
//! section: 5
//! title: ElementAtOrNull with extreme and negative indexes reads null, in
//!   range reads the entry
//! rule: public method ElementAtOrNull(index: int): T?; // — null when
//!   index is out of range
//! cites: 4.8, 5
//! expect: run
//! output: -1
//!   -1
//!   1
//!   -2
//! issue: 8232
//! notes: Red on base: the pre-fix compiler rejected ElementAtOrNull on a
//!   concrete collection with 'no member ElementAtOrNull'. It passes on the
//!   staging tip.
module Conf;

public class Program
  public static method Main()
    using var d := new Dictionary<int, int>();
    d.Add(1, 10);
    Console.WriteLine(d.ElementAtOrNull(int.MaxValue)?.Value ?? -1);
    Console.WriteLine(d.ElementAtOrNull(-1)?.Key ?? -1);
    Console.WriteLine(d.ElementAtOrNull(0)?.Key ?? -1);
    Console.WriteLine(d.ElementAtOrNull(1)?.Value ?? -2);
  end Main;
end Program;
