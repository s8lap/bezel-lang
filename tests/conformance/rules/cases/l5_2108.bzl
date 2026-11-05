//! case: l5_2108
//! doc: stdlib
//! section: 5
//! title: ElementAtOrNull results flow through a field, a return and a
//!   null-conditional read
//! rule: public method ElementAtOrNull(index: int): T?; // — null when
//!   index is out of range
//! cites: 4.8, 5
//! expect: run
//! output: 8
//!   -1
//!   -2
//!   5
//!   3
//! issue: 8232
//! notes: Red on base: the pre-fix compiler rejected ElementAtOrNull on a
//!   concrete collection with 'no member ElementAtOrNull'. It passes on the
//!   staging tip.
module Conf;

public class H
  public E: DictionaryEntry<int, int>?;

  public constructor()
  end constructor;

  destructor
  end destructor;
end H;

public class Program
  static method Pick(d: Dictionary<int, int>, i: int): DictionaryEntry<int, int>?
    if i < 0 then
      return null;
    end;
    return d.ElementAtOrNull(i);
  end Pick;

  public static method Main()
    using var d := new Dictionary<int, int>();
    d.Add(3, 5);
    var p := Pick(d, 0);
    if p <> null then
      Console.WriteLine(p.Key + p.Value);
    end;
    Console.WriteLine(Pick(d, 4)?.Key ?? -1);
    Console.WriteLine(Pick(d, -1)?.Key ?? -2);
    using var h := new H();
    h.E := d.ElementAtOrNull(0);
    Console.WriteLine(h.E?.Value ?? -1);
    if h.E <> null then
      Console.WriteLine(h.E!.Key);
    end;
  end Main;
end Program;
