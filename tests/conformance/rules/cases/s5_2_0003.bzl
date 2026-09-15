//! case: s5_2_0003
//! doc: spec
//! section: 5.2
//! title: an explicit conversion from int to bool is rejected
//! rule: bool(n) remains an error in both the implicit and explicit forms — a
//!   numeric-to-bool reading is partial and truthiness-shaped; write the
//!   comparison (n <> 0) instead.
//! cites: 5.2
//! expect: reject
//! diagnostic: remains an error
module Conf;

public class Program
  public static method Convert(n: int)
    var b := bool(n);
  end Convert;
end Program;
