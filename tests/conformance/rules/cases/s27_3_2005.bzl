//! case: s27_3_2005
//! doc: spec
//! section: 27.3
//! title: a lambda returning a base-class value at a derived-class slot is
//!   rejected as a downcast
//! rule: its inferred return type must equal the target's return slot or
//!   implicitly convert to it
//! cites: 5.1, 5.2
//! expect: reject
//! diagnostic: implicitly convertible
//! notes: Failed on the compiler before the #8168 fix and passes on the
//!   staging tip.
module Conf;

class Base
  public method V(): int => 1;
end Base;

class Derived: Base
  public method W(): int => 2;
end Derived;

public class Program
  public static method Main()
    var f: (Base) -> Derived := lambda(x) => x;
  end Main;
end Program;
