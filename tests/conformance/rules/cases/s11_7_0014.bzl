//! case: s11_7_0014
//! doc: spec
//! section: 11.7
//! title: abstract and static are mutually exclusive
//! rule: The `abstract` and `static` modifiers are mutually exclusive, and
//!   `sealed` and `static` are mutually exclusive; combining either pair is a
//!   compile error.
//! cites: 11.7
//! expect: reject
//! diagnostic: static
module Conf;

public abstract static class Utils
  public static method Max(a: int, b: int): int => (if a > b then a else b);
end Utils;
