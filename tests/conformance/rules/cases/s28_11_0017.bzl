//! case: s28_11_0017
//! doc: spec
//! section: 28.11
//! title: ToArray of a local collection of borrow-bearing records into a field is rejected
//! rule: (i) a method, constructor or `static` factory whose result is an `owned`
//!   collection ... copies into that result every element value it takes: from
//!   its receiver ... A copy is judged as a store of each placed value into the destination, at
//!   the call — at a class or interface element type under this rule, at a value
//!   type that holds borrows under §9.7's collection bullet
//! cites: 28.11, 9.7
//! expect: reject
//! diagnostic: outlive
//! issue: 5721
module Conf;

public class Item
  public constructor()
  end constructor;

  public method N(): int => 1;
end Item;

public record Pair(Ref: Item);

public class Keeper
  private owned _keep: Array<Pair>;

  public constructor()
    _keep := new Array<Pair>();
  end constructor;

  public method Arm()
    using var it := new Item();
    var xs: Array<Pair> := [new Pair(it)];
    _keep := xs.ToArray();
  end Arm;
end Keeper;
