//! case: s28_3_0104
//! doc: spec
//! section: 28.3
//! title: a bare-T result at a borrow-bearing record substitution may not be returned past its source
//! rule: At one that is a value type that holds borrows (§3.6's holds-borrows
//!   property), the borrows the result carries take clause 1's union, clause
//!   2's temporary bound included, and §9.7's rules read them: over `static
//!   method Id<T>(a: T): T => a;` and `record Pair(Item: Item)`, `return
//!   Id<Pair>(new Pair(x));` with `x` scope-bound is rejected at the return.
//! cites: 28.3, 9.7
//! expect: reject
//! diagnostic: outlive
//! issue: 5623
module Conf;

public class Item
  public constructor()
  end constructor;

  public method Size(): int => 1;
end Item;

public record Pair(Item: Item);

public class Fns
  public static method Id<T>(a: T): T => a;
end Fns;

public class Maker
  public constructor()
  end constructor;

  public method Make(): Pair
    using var x := new Item();
    return Fns.Id<Pair>(new Pair(x));
  end Make;
end Maker;
