//! case: s5_1_0162
//! doc: spec
//! section: 5.1
//! title: a long argument at the DateTime int day parameter is not implicitly converted: rejected
//! rule: | `long` | `float`, `double`, `decimal` |
//! cites: 5.1
//! expect: reject
//! diagnostic: not implicitly convertible to the target type
module Conf;

public class Program
  public static method Main()
    var n: long := 5;
    var d := new DateTime(2020, 1, n);
    Console.WriteLine(d.Day);
  end Main;
end Program;
