//! case: s22_12_0005
//! doc: spec
//! section: 22.12
//! title: const requires a compile-time-constant expression
//! rule: `const` declarations require a compile-time-constant expression
//!   and a primitive value type or `string`.
//! cites: 22.12, 17.14
//! expect: reject
//! diagnostic: constant expression
module Conf;

public class Constants
  // The declared type IS a primitive value type, so the only ground for
  // rejection is that a static-method call is not a constant expression.
  public const Limit: int := Compute();

  private static method Compute(): int => 5;
end Constants;
