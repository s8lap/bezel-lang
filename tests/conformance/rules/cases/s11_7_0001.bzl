//! case: s11_7_0001
//! doc: spec
//! section: 11.7
//! title: a static class may contain only static members
//! rule: A class declared static may contain only static members.
//! cites: 11.7
//! expect: accept
module Conf;

public static class Utils
  public static method Max(a: int, b: int): int => (if a > b then a else b);
end Utils;
