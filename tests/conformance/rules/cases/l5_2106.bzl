//! case: l5_2106
//! doc: stdlib
//! section: 5
//! title: ElementAtOrNull on a Dictionary of class values reads the entry
//!   in a 200x loop
//! rule: public method ElementAtOrNull(index: int): T?; // — null when
//!   index is out of range
//! cites: 4.8, 5
//! expect: run
//! output: 1100
//! issue: 8232
//! notes: Red on base: the pre-fix compiler rejected ElementAtOrNull on a
//!   concrete collection with 'no member ElementAtOrNull'. It passes on the
//!   staging tip.
module Conf;

public class Item
  public V: int;

  public constructor(v: int)
    V := v;
  end constructor;

  destructor
  end destructor;
end Item;

public class Program
  public static method Main()
    using var i1 := new Item(5);
    using var d := new Dictionary<int, Item>();
    d.Add(1, i1);
    var t := 0;
    for var i in 0..<200 do
      var e := d.ElementAtOrNull(i mod 2);
      if e <> null then
        t := t + e.Value.V + e.Key;
      end;
      t := t + (e?.Value?.V ?? 0);
    end for;
    Console.WriteLine(t);
  end Main;
end Program;
