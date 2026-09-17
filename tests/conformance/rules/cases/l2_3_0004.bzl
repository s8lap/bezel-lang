//! case: l2_3_0004
//! doc: stdlib
//! section: 2.3
//! title: ToBoolean accepts "true"/"false" case-insensitively
//! rule: Convert.ToBoolean(value) accepts exactly "true"/"false",
//!   case-insensitively -- Boolean.Parse's contract.
//! cites: 9.4
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Convert.ToBoolean("TRUE"));
  end Main;
end Program;
