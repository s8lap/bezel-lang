//! case: s28_11_0015
//! doc: spec
//! section: 28.11
//! title: a direct FromCollectionLiteral copy of borrow-bearing elements into a field is rejected
//! rule: An inline array's `ToArray` (§15.1), range slicing (§15.6) and a direct
//!   `FromCollectionLiteral` call are shape (i)'s copies ... A copy is judged as a store of each placed value into the destination, at
//!   the call — at a class or interface element type under this rule, at a value
//!   type that holds borrows under §9.7's collection bullet
//! cites: 28.11, 9.7, 15.4.1
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
    _keep := Array<Pair>.FromCollectionLiteral(xs.AsReadOnlySpan());
  end Arm;
end Keeper;
