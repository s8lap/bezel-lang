//! case: s7_7_4701
//! doc: spec
//! section: 7.7
//! title: long? ??= ulong is rejected because ulong does not convert implicitly to long
//! rule: for a left operand of type `T?`, the right operand must be implicitly convertible
//! cites: 6.6, 5.1
//! expect: reject
//! diagnostic: not implicitly convertible
module Conf;

public class Program
  public static method Main()
    var x: long? := null;
    var u: ulong := 5UL;
    x ??= u;
    Console.WriteLine(x ?? 0L);
  end Main;
end Program;
