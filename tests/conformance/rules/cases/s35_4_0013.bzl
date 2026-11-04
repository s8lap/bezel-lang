//! case: s35_4_0013
//! doc: spec
//! section: 35.4
//! title: a choice whose payload is a DateTime is value-like, so `=` is generated
//! rule: Otherwise the choice is **value-like**: every payload is a value, a `string`, a
//!   runtime-managed type, or (transitively) a value-like record or choice.
//! cites: 35.4, 35.7
//! expect: run
//! output: True
//!   False
module Conf;

public choice Ev At(p: DateTime), Never; end Ev;

public class Program
  public static method Main()
    var x := Ev.At(new DateTime(2020, 1, 2, 3, 4, 5));
    var y := Ev.At(new DateTime(2021, 1, 2, 3, 4, 5));
    var z := x;
    Console.WriteLine(x = z);
    Console.WriteLine(x = y);
  end Main;
end Program;
