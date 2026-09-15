//! case: s5_1_0002
//! doc: spec
//! section: 5.1
//! title: sbyte implicitly converts to short (admitted widening)
//! rule: sbyte implicitly converts to short, int, long, float, double, decimal
//!   (the §5.1 implicit-conversion table). Twin of s5_1_0001: same source type,
//!   a target the table admits instead of one it excludes.
//! cites: 5.1
//! expect: accept
module Conf;

public class Program
  public static method Convert(s: sbyte)
    var v: short := s;
  end Convert;
end Program;
