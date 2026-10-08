//! case: s9_7_0024
//! doc: spec
//! section: 9.7
//! title: a generic class copying T into a field-held Array<T> is rejected wherever Bag<Pair> is named
//! rule: a `class Bag<T>` whose `Put(t: T)` copies `t` into a field-held
//!   `Array<T>` is admitted at the declaration and at `Bag<Item>` (§28.11's
//!   store rule, under its residual) and rejected wherever `Bag<Pair>` is
//!   named.
//! cites: 9.7, 3.6, 28.11
//! expect: reject
//! diagnostic: collection
//! issue: 5649
module Conf;

public class Item
  public constructor()
  end constructor;

  public method Size(): int => 1;
end Item;

public record Pair(Item: Item);

public class Bag<T>
  private owned _xs: Array<T>;

  public constructor()
    _xs := new Array<T>();
  end constructor;

  public method Put(t: T)
    _xs.Add(t);
  end Put;
end Bag;

public class Client
  public static method Run(p: Pair)
    using var b := new Bag<Pair>();
    b.Put(p);
  end Run;
end Client;
