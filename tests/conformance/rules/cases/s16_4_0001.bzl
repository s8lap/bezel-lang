//! case: s16_4_0001
//! doc: spec
//! section: 16.4
//! title: a constant-folded zero step is rejected at the range expression
//! rule: "A step of zero is rejected at construction: a literal (or constant-
//!   folded) step of zero is a compile error at the range expression."
//! cites: 16.4
//! expect: reject
//! diagnostic: zero
module Conf;

public class Program
  public static method Main()
    var r := 5..10 step 0;
  end Main;
end Program;
