//! case: s15_1_1_0002
//! doc: spec
//! section: 15.1.1
//! title: the owned slot marker is rejected on a value-typed element
//! rule: `[N owned]T` requires T to be a class or interface type; value types
//!   are not admitted with the `owned` marker (§15.1.1).
//! cites: 15.1.1
//! expect: reject
//! diagnostic: class or interface
module Conf;

public class Program
  public static method Foo()
    var x: [3 owned]int;
  end Foo;
end Program;
