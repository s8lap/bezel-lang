//! case: s30_14_0010
//! doc: spec
//! section: 30.14
//! title: the nullable form of a borrow-bearing record does not satisfy where T sendable
//! rule: A **nullable type** `V?` is sendable exactly where `V` is (§3.6
//!   *Nullable forms*) ... **Value types** are sendable iff they are not
//!   borrow-bearing (§9.7), not move-only (§15.3), and **every component type
//!   is sendable**
//! cites: 30.14, 3.6, 14.2
//! expect: reject
//! diagnostic: sendable
module Conf;

public class Item
  public constructor()
  end constructor;
end Item;

public record Job(It: Item);

public class Ship
  public static method Count<T>(x: T): int where T: sendable => 1;

  public static method Run(it: Item): int
    var j: Job? := new Job(it);
    return Ship.Count<Job?>(j);
  end Run;
end Ship;
