//! case: s27_3_2009
//! doc: spec
//! section: 27.3
//! title: a lambda assigned to a function-typed local with an int body at
//!   an owned Item slot is rejected
//! rule: its inferred return type must equal the target's return slot or
//!   implicitly convert to it
//! cites: 5.1
//! expect: reject
//! diagnostic: implicitly convertible
//! notes: Failed on the compiler before the #8168 fix and passes on the
//!   staging tip.
module Conf;

class Item
  public method V(): int => 7;
end Item;

public class Program
  public static method Main()
    var f: (int) -> owned Item := lambda(x) => new Item();
    f := lambda(x) => x;
  end Main;
end Program;
