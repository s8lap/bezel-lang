//! case: s5_1_0167
//! doc: spec
//! section: 5.1
//! title: a long argument at the TimeOnly int second parameter is rejected
//! rule: | `long` | `float`, `double`, `decimal` |
//! cites: 5.1
//! expect: reject
//! diagnostic: not implicitly convertible to the target type
module Conf;

public class Program
  public static method Main()
    var v: long := 5;
    var t := new TimeOnly(1, 2, v);
    Console.WriteLine(t.Hour);
  end Main;
end Program;
