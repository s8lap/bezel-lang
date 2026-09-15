//! case: s7_11_0007
//! doc: spec
//! section: 7.11
//! title: there is no three-valued logic — a bool? operand at and is rejected
//! rule: There are no `bool?` logical operators. `and`, `or`, `not` operate
//!   only on `bool`; a `bool?` operand is a compile error.
//! expect: reject
//! diagnostic: logical operators
module Conf;

public class Program
  public static method Main()
    var flag: bool? := true;
    if flag and true then
      Console.WriteLine("yes");
    end;
  end Main;
end Program;
