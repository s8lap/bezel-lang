//! case: s5_1_0015
//! doc: spec
//! section: 5.1
//! title: a compile-time constant whose value does not fit the target's range is a compile error
//! rule: A constant whose value does not fit the target's range is a compile
//!   error at the conversion site — never a silent truncation. Twin of
//!   s5_1_0014: same target, a value outside byte's range.
//! cites: 5.1
//! expect: reject
//! diagnostic: does not fit
module Conf;

public class Program
  public static method Make()
    var b: byte := 300;
  end Make;
end Program;
