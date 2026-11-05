//! case: s27_3_2003
//! doc: spec
//! section: 27.3
//! title: a lambda returning an int at a class-typed slot is rejected in
//!   return position
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
  public static method Make(): (int) -> Item => lambda(x) => x;

  public static method Main()
  end Main;
end Program;
