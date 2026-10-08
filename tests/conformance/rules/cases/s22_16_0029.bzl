//! case: s22_16_0029
//! doc: spec
//! section: 22.16
//! title: old over a choice that holds borrows is captured by copy
//! rule: An operand of a value type that holds borrows (§3.6's holds-borrows
//!   property: a borrow-bearing record or inline array, or a choice that
//!   holds borrows — §9.7, §15.1.2, §35.4) is admitted with its carried
//!   borrows under §9.7's ordinary rules — an inline array's under the one
//!   source set §15.1.2 gives the array value.
//! cites: 22.16, 35.4, 9.7
//! expect: accept
module Conf;

public class Item
  public constructor()
  end constructor;

  public method Size(): int => 1;
end Item;

public choice Slot
  Filled(item: Item),
  Empty;
end Slot;

public class Ops
  public static pure method Holds(s: Slot): bool => true;

  public static method Touch(s: Slot): int
    ensures(Holds(old(s)))

    return 1;
  end Touch;
end Ops;
