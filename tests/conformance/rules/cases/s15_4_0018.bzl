//! case: s15_4_0018
//! doc: spec
//! section: 15.4
//! title: an owned call result at Set.TryAdd in an if condition is a compile error
//! rule: in a single-element store position of a borrowing-family collection is a compile error at the store site
//! cites: 28.11
//! expect: reject
//! diagnostic: fresh-store
//! issue: 3047
//! notes: Condition position of an if statement.
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
    if s.TryAdd(F.Make(1)) then
      Console.WriteLine("added");
    end if;
    return s.Count;
  end Main;
end Program;
