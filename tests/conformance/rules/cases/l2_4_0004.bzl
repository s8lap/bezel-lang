//! case: l2_4_0004
//! doc: stdlib
//! section: 2.4
//! title: GetInt conflates a malformed value with an absent one, returning null
//! rule: Environment.GetInt(name) returns null when the variable is absent OR
//!   malformed -- deliberately conflated.
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    Environment.Set("BEZEL_CONFORMANCE_TMP_2b91", "not-an-int");
    Console.WriteLine(Environment.GetInt("BEZEL_CONFORMANCE_TMP_2b91") = null);
  end Main;
end Program;
