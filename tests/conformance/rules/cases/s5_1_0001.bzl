//! case: s5_1_0001
//! doc: spec
//! section: 5.1
//! title: sbyte does not implicitly convert to byte (same-width signed/unsigned)
//! rule: The following are never implicit: between signed and unsigned types of
//!   the same or narrower width (e.g. int -> uint, uint -> int, short -> ushort).
//! cites: 5.1
//! expect: reject
//! diagnostic: same or narrower width
module Conf;

public class Program
  public static method Convert(s: sbyte)
    var b: byte := s;
  end Convert;
end Program;
