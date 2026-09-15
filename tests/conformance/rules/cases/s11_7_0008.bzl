//! case: s11_7_0008
//! doc: spec
//! section: 11.7
//! title: a static class may not declare an instance method
//! rule: A static class may not have instance fields, instance methods,
//!   instance properties, indexers, constructors (other than a static
//!   constructor), or destructors.
//! cites: 11.7
//! expect: reject
//! diagnostic: static
module Conf;

public static class Utils
  public static method Max(a: int, b: int): int => (if a > b then a else b);

  public method Describe(): string => "utils";
end Utils;
