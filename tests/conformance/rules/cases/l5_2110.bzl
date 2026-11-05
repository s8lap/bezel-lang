//! case: l5_2110
//! doc: stdlib
//! section: 5
//! title: ElementAtOrNull on a Dictionary through a declared nullable
//!   DictionaryEntry local
//! rule: public method ElementAtOrNull(index: int): T?; // — null when
//!   index is out of range
//! cites: 4.8, 5
//! expect: run
//! output: -1
//!   3
//!   8
//! issue: 8232
//! notes: Red on base: the pre-fix compiler rejected ElementAtOrNull on a
//!   concrete collection with 'no member ElementAtOrNull'. It passes on the
//!   staging tip.
module Conf;

public class Program
  public static method Main()
    using var d := new Dictionary<int, int>();
    d.Add(3, 5);
    var e: DictionaryEntry<int, int>? := null;
    Console.WriteLine(e?.Key ?? -1);
    e := d.ElementAtOrNull(0);
    Console.WriteLine(e?.Key ?? -1);
    if e <> null then
      Console.WriteLine(e.Value + e!.Key);
    end;
  end Main;
end Program;
