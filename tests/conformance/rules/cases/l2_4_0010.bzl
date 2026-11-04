//! case: l2_4_0010
//! doc: stdlib
//! section: 2.4
//! title: GetBool returns null for the value 1
//! rule: GetBool accepts exactly Boolean.Parse's forms
//! expect: run
//! stdout: True
//! issue: 7949
//! notes: The same declaration comment states: "1"/"yes" are null.
module Conf;

public class Program
  public static method Main()
    Environment.Set("BZ_H2_BOOL_C", "1");
    Console.WriteLine(Environment.GetBool("BZ_H2_BOOL_C") = null);
  end Main;
end Program;
