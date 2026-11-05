//! case: l4_9_2101
//! doc: stdlib
//! section: 4.9
//! title: ElementAtOrNull is not offered on an OwningDictionary
//! rule: The interface conformance is `IDisposable` only
//! cites: 5, 4.8
//! expect: reject
//! diagnostic: IDisposable
//! notes: Green on base and on the staging tip: the refusal control for the
//!   #8232 enablement of ElementAtOrNull on the borrowing collections.
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
    using var d := new OwningDictionary<int, Item>();
    Console.WriteLine(d.ElementAtOrNull(0)?.Key ?? -1);
  end Main;
end Program;
