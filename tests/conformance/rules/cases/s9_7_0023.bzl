//! case: s9_7_0023
//! doc: spec
//! section: 9.7
//! title: a generic class storing its T parameter in a T field is rejected wherever Keep<Pair> is named
//! rule: a `class Keep<T>` whose `Put(a: T)` stores `a` into a `_last: T` field
//!   is admitted at the declaration and, under §28.3's field-store bullet, at
//!   `Keep<Item>`, and is rejected wherever `Keep<Pair>` is named, by the
//!   instance-field bullet, the diagnostic naming the store.
//! cites: 9.7, 3.6
//! expect: reject
//! diagnostic: instance field
//! issue: 5649
module Conf;

public class Item
  public constructor()
  end constructor;

  public method Size(): int => 1;
end Item;

public record Pair(Item: Item);

public class Keep<T>
  private _last: T;

  public constructor(first: T)
    _last := first;
  end constructor;

  public method Put(a: T)
    _last := a;
  end Put;
end Keep;

public class Client
  public static method Run(p: Pair, q: Pair)
    using var k := new Keep<Pair>(p);
    k.Put(q);
  end Run;
end Client;
