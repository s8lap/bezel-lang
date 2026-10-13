//! case: s28_11_0023
//! doc: spec
//! section: 28.11
//! title: the fill constructor copying a borrow-bearing record into a field is rejected
//! rule: (i) a method, constructor or `static` factory whose result is an `owned`
//!   collection — the constructed collection itself, for a constructor — copies
//!   into that result every element value it takes ... from each argument
//!   declared at one of them (`fill`)
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
    _keep := new Array<Pair>(3, new Pair(it));
  end Arm;
end Keeper;
