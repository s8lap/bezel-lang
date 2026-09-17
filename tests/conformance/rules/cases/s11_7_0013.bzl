//! case: s11_7_0013
//! doc: spec
//! section: 11.7
//! title: a static class may not implement an interface
//! rule: A static class may not be inherited from and does not implement
//!   interfaces.
//! cites: 11.7
//! expect: reject
//! diagnostic: static
module Conf;

public interface IMarker
end IMarker;

public static class Utils: IMarker
  public static method Max(a: int, b: int): int => (if a > b then a else b);
end Utils;
