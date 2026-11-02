//! case: s26_5_0056
//! doc: spec
//! section: 26.5
//! title: an undeclared method called on a captured task inside a lambda is rejected
//! rule: If the receiver's type declares no instance member of that name, the
//!   compiler searches the current module and all imported modules
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
