//! case: s22_13_0002
//! doc: spec
//! section: 22.13
//! title: a static readonly field may not read a static readonly field of the same type declared after it
//! rule: If the initializer expression for a `static readonly` field F
//!   reads a `static readonly` field G of the same type declared after F
//!   in the source — directly — the reference is rejected at compile
//!   time.
//! cites: 22.13
//! expect: reject
//! diagnostic: forward reference
module Conf;

public class Config
  private static readonly _a: int := _b;
  private static readonly _b: int := 5;
end Config;
