//! case: s17_7_0017
//! doc: spec
//! section: 17.7
//! title: a string cannot stand for the capacity of an OwningSet: no constructor takes it
//! rule: `new T(...)` resolves only against a declared constructor, never against a
//!   compiler-supplied fallback
//! cites: 17.7, 5.1, 4.10
//! expect: reject
//! diagnostic: constructor
module Conf;

public class Box
  public constructor()
  end constructor;
end Box;

public class Program
  public static method Main()
    using var s := new OwningSet<Box>("x");
  end Main;
end Program;
