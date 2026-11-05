//! case: s27_3_2001
//! doc: spec
//! section: 27.3
//! title: a lambda returning an enum value at a slot of a different enum is
//!   rejected
//! rule: its inferred return type must equal the target's return slot or
//!   implicitly convert to it
//! cites: 5.1, 10.5
//! expect: reject
//! diagnostic: implicitly convertible
//! notes: Failed on the compiler before the #8168 fix and passes on the
//!   staging tip.
module Conf;

enum Pri
  Low, High;
end Pri;

enum Col
  Red, Blue;
end Col;

public class Program
  public static method Main()
    var f: () -> Pri := lambda() => Col.Red;
  end Main;
end Program;
