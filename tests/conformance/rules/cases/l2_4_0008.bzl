//! case: l2_4_0008
//! doc: stdlib
//! section: 2.4
//! title: GetBool accepts the form TRUE case-insensitively
//! rule: GetBool accepts exactly Boolean.Parse's forms
//! expect: run
//! stdout: True
//! issue: 7949
//! notes: Boolean.Parse's forms are true/false case-insensitive, per the continuation line of the same declaration comment.
module Conf;

public class Program
  public static method Main()
    Environment.Set("BZ_H2_BOOL_A", "TRUE");
    Console.WriteLine(Environment.GetBool("BZ_H2_BOOL_A") ?? false);
  end Main;
end Program;
