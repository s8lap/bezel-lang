//! case: s7_11_0008
//! doc: spec
//! section: 7.11
//! title: and operates on a non-nullable bool operand
//! rule: `and`, `or`, `not` operate on `bool`; a non-nullable `bool` operand
//!   is admitted.
//! expect: run
//! stdout: yes
module Conf;

public class Program
  public static method Main()
    var flag: bool := true;
    if flag and true then
      Console.WriteLine("yes");
    end;
  end Main;
end Program;
