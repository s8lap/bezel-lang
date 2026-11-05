//! case: s27_3_2010
//! doc: spec
//! section: 27.3
//! title: a lambda in a field initializer with an int body at a record slot
//!   is rejected
//! rule: its inferred return type must equal the target's return slot or
//!   implicitly convert to it
//! cites: 5.1
//! expect: reject
//! diagnostic: implicitly convertible
//! notes: Failed on the compiler before the #8168 fix and passes on the
//!   staging tip.
module Conf;

record Point(X: int, Y: int);

class Holder
  public F: (int) -> Point := lambda(x) => x;
end Holder;

public class Program
  public static method Main()
  end Main;
end Program;
