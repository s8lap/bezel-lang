//! case: s27_3_2012
//! doc: spec
//! section: 27.3
//! title: a lambda returning an Item at an interface slot the Item does not
//!   implement is rejected
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

public interface IHasN
  method N(): int;
end IHasN;

public class Program
  public static method Main()
    var f: (Item) -> IHasN := lambda(x) => x;
  end Main;
end Program;
