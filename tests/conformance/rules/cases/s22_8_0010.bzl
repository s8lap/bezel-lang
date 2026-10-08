//! case: s22_8_0010
//! doc: spec
//! section: 22.8
//! title: a static readonly field assigned in the static constructor is admitted
//! rule: a `static readonly` field is assignable only in its own static field
//!   initializer or in the declaring type's **static** constructor body,
//!   spelled as the bare name or `TypeName.F`
//! cites: 22.8, 22.13
//! expect: accept
module Conf;

public class Registry
  private static readonly _capacity: int;

  static constructor()
    _capacity := 64;
  end constructor;

  public static method Capacity(): int => _capacity;
end Registry;
