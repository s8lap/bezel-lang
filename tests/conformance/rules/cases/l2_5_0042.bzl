//! case: l2_5_0042
//! doc: stdlib
//! section: 2.5
//! title: TimeOnly binds trailing named arguments and a mix of positional and named arguments
//! rule: public constructor(hour: int, minute: int, second: int := 0, millisecond: int := 0);
//! cites: 2.5, 22.5
//! expect: run
//! output: 3
//!   9
//!   4
module Conf;

public class Program
  public static method Main()
    var a := new TimeOnly(1, 2, second := 3);
    Console.WriteLine(a.Second);
    var b := new TimeOnly(1, 2, millisecond := 9);
    Console.WriteLine(b.Millisecond);
    var c := new TimeOnly(1, minute := 2, second := 3, millisecond := 4);
    Console.WriteLine(c.Millisecond);
  end Main;
end Program;
