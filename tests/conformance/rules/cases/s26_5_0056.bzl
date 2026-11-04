//! case: s26_5_0056
//! doc: spec
//! section: 26.5
//! title: an undeclared method called on a captured task inside a lambda is rejected
//! rule: Where neither rule 1 nor this search supplies the name, nothing
//!   supplies it: the access names no member and is a compile error at the
//!   name, the diagnostic naming the receiver's static type.
//! cites: 26.5, 30.3
//! expect: reject
//! diagnostic: Bogus
module Conf;

public class Program
  public static method Main(): int
    using var t := Task.FromResult(2);
    var f := lambda() => t.Bogus();
    return 0;
  end Main;
end Program;
