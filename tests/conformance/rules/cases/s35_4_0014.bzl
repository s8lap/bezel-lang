//! case: s35_4_0014
//! doc: spec
//! section: 35.4
//! title: a choice whose payload is a TimeSpan is value-like, so `=` is generated
//! rule: Otherwise the choice is **value-like**: every payload is a value, a `string`, a
//!   runtime-managed type, or (transitively) a value-like record or choice.
//! cites: 35.4, 35.7
//! expect: run
//! output: True
//!   False
module Conf;

public choice Ev At(p: TimeSpan), Never; end Ev;

public class Program
  public static method Main()
    var x := Ev.At(TimeSpan.FromTicks(10L));
    var y := Ev.At(TimeSpan.FromTicks(20L));
    var z := x;
    Console.WriteLine(x = z);
    Console.WriteLine(x = y);
  end Main;
end Program;
