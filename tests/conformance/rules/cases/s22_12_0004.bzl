//! case: s22_12_0004
//! doc: spec
//! section: 22.12
//! title: const requires a primitive value type or string
//! rule: `const` declarations require a compile-time-constant expression
//!   and a primitive value type or `string`.
//! cites: 22.12, 17.14
//! expect: reject
//! diagnostic: primitive
module Conf;

public record Point(X: int, Y: int);

public class Constants
  public const Origin: Point := new Point(0, 0);
end Constants;
