//! case: s15_4_0023
//! doc: spec
//! section: 15.4
//! title: a fresh new expression as the value of Dictionary.TryAdd is a compile error
//! rule: in a single-element store position of a borrowing-family collection is a compile error at the store site
//! cites: 28.11
//! expect: reject
//! diagnostic: fresh-store
//! issue: 3047
//! notes: Value position of a two-element TryAdd; TryAdd is listed.
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
    using var d := new Dictionary<int, Item>();
    var added := d.TryAdd(1, new Item(1));
    Console.WriteLine(added);
    return 0;
  end Main;
end Program;
