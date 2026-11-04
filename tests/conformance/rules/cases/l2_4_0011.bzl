//! case: l2_4_0011
//! doc: stdlib
//! section: 2.4
//! title: GetBool returns null for the value yes
//! rule: GetBool accepts exactly Boolean.Parse's forms
//! expect: run
//! stdout: True
//! issue: 7949
//! notes: The same declaration comment states: "1"/"yes" are null.
module Conf;

public class Program
  public static method Main()
    Environment.Set("BZ_H2_BOOL_D", "yes");
    Console.WriteLine(Environment.GetBool("BZ_H2_BOOL_D") = null);
  end Main;
end Program;
