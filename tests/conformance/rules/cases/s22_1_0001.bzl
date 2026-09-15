//! case: s22_1_0001
//! doc: spec
//! section: 22.1
//! title: a method declares a return type after the colon
//! rule: A method has a return type after `:`, or no `:` clause for a
//!   procedure (return type `()`).
//! cites: 22.1
//! expect: accept
module Conf;

public class MathOps
  public constructor()
  end constructor;

  public method Add(a: int, b: int): int
    return a + b;
  end Add;
end MathOps;
