//! case: s5_1_0004
//! doc: spec
//! section: 5.1
//! title: int implicitly converts to long (admitted widening)
//! rule: int implicitly converts to long, float, double, decimal (the §5.1
//!   implicit-conversion table). Twin of s5_1_0003: same source type, a target
//!   the table admits instead of one it excludes.
//! cites: 5.1
//! expect: accept
module Conf;

public class Program
  public static method Convert(n: int)
    var l: long := n;
  end Convert;
end Program;
