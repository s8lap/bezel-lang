//! case: s5_1_0003
//! doc: spec
//! section: 5.1
//! title: int does not implicitly convert to uint (same-width signed/unsigned)
//! rule: The following are never implicit: between signed and unsigned types of
//!   the same or narrower width (e.g. int -> uint, uint -> int, short -> ushort).
//! cites: 5.1
//! expect: reject
//! diagnostic: same or narrower width
module Conf;

public class Program
  public static method Convert(n: int)
    var u: uint := n;
  end Convert;
end Program;
