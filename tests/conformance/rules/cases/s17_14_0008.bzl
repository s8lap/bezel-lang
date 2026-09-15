//! case: s17_14_0008
//! doc: spec
//! section: 17.14
//! title: an interpolated string is not a constant expression
//! rule: "Nothing else is constant. In particular: ... interpolated strings
//!   ... are not constant expressions. `const S := $\"v{H}\";` [is a]
//!   compile error: interpolated strings are not constant." The
//!   constant-expression initializer condition "remain[s] mandatory for
//!   class-level and `static const` declarations" — at a *local* `const` a
//!   non-constant initializer is a runtime const instead (s17_14_0010).
//! cites: 17.14, 22.12, 18.5
//! expect: reject
//! diagnostic: constant
module Conf;

public class Holder
  public const H: int := 5;
  public const S: string := $"v{H}";
end Holder;
