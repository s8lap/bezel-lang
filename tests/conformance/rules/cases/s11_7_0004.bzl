//! case: s11_7_0004
//! doc: spec
//! section: 11.7
//! title: a static class may not be inherited from another class
//! rule: A static class may not be inherited from and does not implement
//!   interfaces.
//! cites: 11.7
//! expect: reject
//! diagnostic: static
module Conf;

public class Base
  public constructor()
  end constructor;
end Base;

public static class Utils: Base
  public static method Max(a: int, b: int): int => (if a > b then a else b);
end Utils;
