//! case: s11_7_0002
//! doc: spec
//! section: 11.7
//! title: new on a static class is rejected
//! rule: A static class may not be instantiated: new on a static class is a
//!   compile error.
//! cites: 11.7
//! expect: reject
//! diagnostic: static
module Conf;

public static class Utils
  public static method Max(a: int, b: int): int => (if a > b then a else b);
end Utils;

public class Program
  public static method Test()
    var u := new Utils();
  end Test;
end Program;
