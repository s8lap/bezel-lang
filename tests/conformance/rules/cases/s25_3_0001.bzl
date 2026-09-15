//! case: s25_3_0001
//! doc: spec
//! section: 25.3
//! title: a block-bodied operator closes with end operator and its token
//! rule: When an operator overload is block-bodied, the optional name in the
//!   closing `end` is written as `end operator OpToken;`.
//! cites: 25.3, 1.6
//! expect: accept
module Conf;

public record Vec(X: int)
  public static method operator +(a: Vec, b: Vec): Vec
    return new Vec(a.X + b.X);
  end operator +;
end Vec;
