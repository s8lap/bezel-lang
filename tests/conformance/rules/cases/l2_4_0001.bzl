//! case: l2_4_0001
//! doc: stdlib
//! section: 2.4
//! title: Get returns null when the variable is absent
//! rule: Environment.Get(name) returns null when the variable is absent.
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Environment.Get("BEZEL_CONFORMANCE_UNSET_9f3a2b") = null);
  end Main;
end Program;
