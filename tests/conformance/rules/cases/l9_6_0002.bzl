//! case: l9_6_0002
//! doc: stdlib
//! section: 9.6
//! title: IndexOf("") is 0 — the vacuous-match convention
//! rule: IndexOf("", start) = start, so IndexOf("") = 0.
//! cites: 9.6
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static method Main()
    Console.WriteLine("abc".IndexOf(""));
  end Main;
end Program;
