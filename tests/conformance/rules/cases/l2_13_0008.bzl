//! case: l2_13_0008
//! doc: stdlib
//! section: 2.13
//! title: a static readonly field of type Type is warning-free
//! rule: `Type` joins language spec §22.12 rule 4's warning-suppression set
//!   — a `static readonly` field or auto-property of type `Type` is
//!   warning-free.
//! cites: 2.13, 22.12, 8.2
//! expect: quiet
//! issue: 511
module Conf;

public class Registry
  public static readonly Kind: Type := "token".GetType();
end Registry;
