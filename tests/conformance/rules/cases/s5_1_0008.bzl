//! case: s5_1_0008
//! doc: spec
//! section: 5.1
//! title: char implicitly converts to int
//! rule: char implicitly converts to int, uint, long, ulong, float, double,
//!   decimal (the §5.1 implicit-conversion table).
//! cites: 5.1
//! expect: accept
module Conf;

public class Program
  public static method Convert(c: char)
    var n: int := c;
  end Convert;
end Program;
