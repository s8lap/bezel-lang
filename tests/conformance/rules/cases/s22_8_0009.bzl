//! case: s22_8_0009
//! doc: spec
//! section: 22.8
//! title: a static readonly field assigned in an instance constructor is rejected
//! rule: a `static readonly` field is assignable only in its own static field
//!   initializer or in the declaring type's **static** constructor body,
//!   spelled as the bare name or `TypeName.F` ... and an instance constructor
//!   of the declaring type is not an assignment site for it, so no `new C()`
//!   writes `C._s`.
//! cites: 22.8, 7.4, 22.13
//! expect: reject
//! diagnostic: static constructor
//! issue: 5638
module Conf;

public class Registry
  private static readonly _capacity: int;

  public constructor()
    _capacity := 64;
  end constructor;

  public static method Capacity(): int => _capacity;
end Registry;
