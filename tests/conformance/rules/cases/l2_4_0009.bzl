//! case: l2_4_0009
//! doc: stdlib
//! section: 2.4
//! title: GetBool returns false, not null, for the value false
//! rule: GetBool accepts exactly Boolean.Parse's forms
//! expect: run
//! output: True
//!   False
//! issue: 7949
//! notes: Separates the value false from the null default: b ?? true would print True if the result were null.
module Conf;

public class Program
  public static method Main()
    Environment.Set("BZ_H2_BOOL_B", "false");
    var b := Environment.GetBool("BZ_H2_BOOL_B");
    Console.WriteLine(b <> null);
    Console.WriteLine(b ?? true);
  end Main;
end Program;
