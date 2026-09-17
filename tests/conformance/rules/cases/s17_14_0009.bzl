//! case: s17_14_0009
//! doc: spec
//! section: 17.14
//! title: default(T) is not a constant expression — rejected as a class-level const initializer
//! rule: "Nothing else is constant. In particular: ... `default(T)` ...
//!   (declined-liftable ...) ... are not constant expressions." The type
//!   restriction on `const` — a constant-expression initializer — "remain[s]
//!   mandatory for class-level and `static const` declarations."
//! cites: 17.14, 22.12
//! expect: reject
//! diagnostic: constant expression
module Conf;

public class Holder
  public const Z: int := default(int);
end Holder;
