//! case: s5_1_0164
//! doc: spec
//! section: 5.1
//! title: a uint argument at the DateTime int day parameter is rejected
//! rule: Between signed and unsigned types of the same or narrower width
//! cites: 5.1
//! expect: reject
//! diagnostic: not implicitly convertible to the target type
module Conf;

public class Program
  public static method Main()
    var v: uint := 5;
    var d := new DateTime(2020, 1, v);
    Console.WriteLine(d.Day);
  end Main;
end Program;
