//! case: s17_14_0006
//! doc: spec
//! section: 17.14
//! title: division by zero at a constant expression is a compile error at the declaration
//! rule: "An evaluation fault ... division or `mod` by zero ... is a compile
//!   error at the constant expression, never a deferred runtime fault:
//!   ... `const D := 5 / 0;` ... rejected where they are written."
//! cites: 17.14, 6.1
//! expect: reject
//! diagnostic: zero
module Conf;

public class Program
  public static method Main()
    const d := 5 / 0;
  end Main;
end Program;
