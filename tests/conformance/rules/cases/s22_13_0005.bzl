//! case: s22_13_0005
//! doc: spec
//! section: 22.13
//! title: forward references between const declarations are not subject to the ordering rule
//! rule: Forward references between `const` declarations are not subject
//!   to this rule: a `const` initializer must be a compile-time-constant
//!   expression, so forward references between consts are either
//!   resolved at compile time or are themselves a compile error for
//!   non-constant operands.
//! cites: 22.13, 17.14
//! expect: accept
module Conf;

public class Config
  public const A: int := B;
  public const B: int := 5;
end Config;
