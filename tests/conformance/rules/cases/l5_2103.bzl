//! case: l5_2103
//! doc: stdlib
//! section: 5
//! title: ElementAtOrNull on a Dictionary of nullable strings: ?. chains
//!   and ?? defaults
//! rule: public method ElementAtOrNull(index: int): T?; // — null when
//!   index is out of range
//! cites: 4.8, 5
//! expect: run
//! output: v1/k1
//!   dflt/k2
//!   dflt/nokey
//!   dflt/nokey
//! issue: 8232
//! notes: Red on base: the pre-fix compiler rejected ElementAtOrNull on a
//!   concrete collection with 'no member ElementAtOrNull'. It passes on the
//!   staging tip.
module Conf;

public class Program
  public static method Main()
    using var d := new Dictionary<string, string?>();
    d.Add("k1", "v1");
    d.Add("k2", null);
    for var i in 0..<4 do
      var e := d.ElementAtOrNull(i);
      Console.WriteLine((e?.Value ?? "dflt") + "/" + (e?.Key ?? "nokey"));
    end for;
  end Main;
end Program;
