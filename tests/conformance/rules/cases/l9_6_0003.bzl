//! case: l9_6_0003
//! doc: stdlib
//! section: 9.6
//! title: StartsWith("") is true — the vacuous-match convention
//! rule: Empty needle (v0.230): Contains("") = true; StartsWith("") =
//!   true; EndsWith("") = true.
//! cites: 9.6
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    Console.WriteLine("abc".StartsWith(""));
  end Main;
end Program;
