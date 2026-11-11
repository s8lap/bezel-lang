//! case: s7_7_4702
//! doc: spec
//! section: 7.7
//! title: byte? ??= sbyte is rejected because sbyte does not convert implicitly to byte
//! rule: for a left operand of type `T?`, the right operand must be implicitly convertible
//! cites: 6.6, 5.1
//! expect: reject
//! diagnostic: not implicitly convertible
module Conf;

public class Program
  public static method Main()
    var x: byte? := null;
    var s: sbyte := sbyte(5);
    x ??= s;
    Console.WriteLine(x ?? byte(0));
  end Main;
end Program;
