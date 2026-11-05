//! case: l5_2101
//! doc: stdlib
//! section: 5
//! title: ElementAtOrNull on a Dictionary yields the entry or null past the
//!   end
//! rule: public method ElementAtOrNull(index: int): T?; // — null when
//!   index is out of range
//! cites: 4.8, 5
//! expect: run
//! output: 8
//!   none
//! issue: 8232
//! notes: Red on base: the pre-fix compiler rejected ElementAtOrNull on a
//!   concrete collection with 'no member ElementAtOrNull'. It passes on the
//!   staging tip.
module Conf;

public class Program
  public static method Main()
    using var d := new Dictionary<int, int>();
    d.Add(3, 5);
    var e := d.ElementAtOrNull(0);
    if e <> null then
      Console.WriteLine(e.Key + e.Value);
    end;
    var f := d.ElementAtOrNull(1);
    if f = null then
      Console.WriteLine("none");
    end;
  end Main;
end Program;
