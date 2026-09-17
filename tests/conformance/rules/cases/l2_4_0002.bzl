//! case: l2_4_0002
//! doc: stdlib
//! section: 2.4
//! title: Set(name, null) removes the variable
//! rule: Environment.Set(name, value) with value = null removes the variable.
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    Environment.Set("BEZEL_CONFORMANCE_TMP_7c1e", "1");
    Environment.Set("BEZEL_CONFORMANCE_TMP_7c1e", null);
    Console.WriteLine(Environment.Get("BEZEL_CONFORMANCE_TMP_7c1e") = null);
  end Main;
end Program;
