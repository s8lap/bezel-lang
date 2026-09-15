//! case: s5_1_0016
//! doc: spec
//! section: 5.1
//! title: constant narrowing crosses signedness when the constant's value fits the target's range
//! rule: The rule is value-based, not type-based: the source may be a
//!   compile-time constant of any integer type, and the target any integer
//!   type whose range contains the value, across signedness: const N: long :=
//!   8; var b: byte := N; is well-typed.
//! cites: 5.1
//! expect: accept
module Conf;

public class Program
  public static method Make()
    const N: long := 8;
    var b: byte := N;
  end Make;
end Program;
