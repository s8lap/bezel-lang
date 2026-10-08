//! case: s28_3_0112
//! doc: spec
//! section: 28.3
//! title: invoking a lambda-built (Item) -> Pair over a local yields a value that may not be returned
//! rule: Over `record Pair(Item: Item)`, `var g: (Item) -> Pair := lambda(i) =>
//!   new Pair(i);` is admitted and `return g(x);` with `x` scope-bound is
//!   rejected at the return.
//! cites: 28.3, 9.7
//! expect: reject
//! diagnostic: outlive
//! issue: 5632
module Conf;

public class Item
  public constructor()
  end constructor;

  public method Size(): int => 1;
end Item;

public record Pair(Item: Item);

public class Maker
  public constructor()
  end constructor;

  public method Make(): Pair
    using var x := new Item();
    var g: (Item) -> Pair := lambda(i) => new Pair(i);
    return g(x);
  end Make;
end Maker;
