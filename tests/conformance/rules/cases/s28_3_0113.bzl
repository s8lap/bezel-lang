//! case: s28_3_0113
//! doc: spec
//! section: 28.3
//! title: a lambda at a borrow-bearing return slot may return a value carrying its parameter's borrow
//! rule: Over `record Pair(Item: Item)`, `var g: (Item) -> Pair := lambda(i) =>
//!   new Pair(i);` is admitted and `return g(x);` with `x` scope-bound is
//!   rejected at the return.
//! cites: 28.3, 9.7
//! expect: accept
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

  public method Measure(): int
    using var x := new Item();
    var g: (Item) -> Pair := lambda(i) => new Pair(i);
    var p := g(x);
    return p.Item.Size();
  end Measure;
end Maker;
