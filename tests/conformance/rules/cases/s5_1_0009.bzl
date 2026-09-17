//! case: s5_1_0009
//! doc: spec
//! section: 5.1
//! title: decimal does not implicitly convert from double
//! rule: The following are never implicit: between decimal and binary floats
//!   (float, double).
//! cites: 5.1
//! expect: reject
//! diagnostic: binary floats
module Conf;

public class Program
  public static method Convert(d: double)
    var m: decimal := d;
  end Convert;
end Program;
