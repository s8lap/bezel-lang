//! case: s31_1_0002
//! doc: spec
//! section: 31.1
//! title: throwing a primitive-typed operand is rejected
//! rule: `throw expr` requires `expr`'s static type, nullability not admitted,
//!   to be `Exception` or a class deriving from it; values of primitive type
//!   cannot be thrown — a compile error at the throw site.
//! cites: 31.1
//! expect: reject
//! diagnostic: exception
module Conf;

public class Program
  public static method Main()
    throw 42;
  end Main;
end Program;
