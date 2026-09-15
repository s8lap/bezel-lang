//! case: s15_1_0003
//! doc: spec
//! section: 15.1
//! title: an inline array initializer with too few elements is a compile error
//! rule: The list must contain exactly N elements; mismatch is a compile
//!   error (§15.1).
//! cites: 15.1
//! expect: reject
//! diagnostic: elements
module Conf;

public class Program
  public static method Main()
    var rgb: [3]byte := [255, 128];
  end Main;
end Program;
