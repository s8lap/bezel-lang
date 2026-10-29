//! case: s5_1_0165
//! doc: spec
//! section: 5.1
//! title: an enum argument at the DateTime int day parameter is rejected
//! rule: Between an enum and its underlying integer.
//! cites: 5.1
//! expect: reject
//! diagnostic: not implicitly convertible to the target type
module Conf;

enum Pri
  Lo, Hi;
end Pri;

public class Program
  public static method Main()
    var e := Pri.Hi;
    var d := new DateTime(2020, 1, e);
    Console.WriteLine(d.Day);
  end Main;
end Program;
