//! case: l5_2105
//! doc: stdlib
//! section: 5
//! title: ElementAtOrNull on a Dictionary with nullable values passes the
//!   nullable value through
//! rule: public method ElementAtOrNull(index: int): T?; // — null when
//!   index is out of range
//! cites: 4.8, 5, 7.10
//! expect: run
//! output: 10
//!   -9
//!   -9
//! issue: 8232
//! notes: Red on base: the pre-fix compiler rejected ElementAtOrNull on a
//!   concrete collection with 'no member ElementAtOrNull'. It passes on the
//!   staging tip.
module Conf;

public class Program
  public static method Main()
    using var d := new Dictionary<int, int?>();
    d.Add(1, 10);
    d.Add(2, null);
    for var i in 0..<3 do
      var v := d.ElementAtOrNull(i)?.Value;
      Console.WriteLine(v ?? -9);
    end for;
  end Main;
end Program;
