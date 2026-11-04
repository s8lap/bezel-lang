//! case: s15_4_0020
//! doc: spec
//! section: 15.4
//! title: an owned call result at Set.TryAdd nested as a call argument is a compile error
//! rule: the position is the member the call resolves to
//! cites: 28.11
//! expect: reject
//! diagnostic: fresh-store
//! issue: 3047
//! notes: Nested-argument position.
module Conf;

public class Item
  public V: int := 0;

  public constructor(v: int)
    V := v;
  end constructor;
end Item;

public class F
  public static method Make(v: int): owned Item
    return new Item(v);
  end Make;
end F;

public class Program
  public static method Main(): int
    using var s := new Set<Item>();
    Console.WriteLine(s.TryAdd(F.Make(1)));
    return 0;
  end Main;
end Program;
