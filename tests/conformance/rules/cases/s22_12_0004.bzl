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

enum Level
  Low, Medium, High;
end Level;

public class Constants
  // The initializer IS a compile-time constant expression (§17.14 admits an
  // enum constant), so the only ground for rejection is the type requirement.
  public const Default: Level := Level.Medium;
end Constants;
