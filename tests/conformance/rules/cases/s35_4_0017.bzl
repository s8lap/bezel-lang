//! case: s35_4_0017
//! doc: spec
//! section: 35.4
//! title: a choice whose payload is a Guid is value-like, so `=` is generated
//! rule: Otherwise the choice is **value-like**: every payload is a value, a `string`, a
//!   runtime-managed type, or (transitively) a value-like record or choice.
//! cites: 35.4, 35.7
//! expect: run
//! output: True
//!   False
module Conf;

public choice Ev At(p: Guid), Never; end Ev;

public class Program
  public static method Main()
    var x := Ev.At(Guid.Parse("00000000-0000-0000-0000-000000000001"));
    var y := Ev.At(Guid.Parse("00000000-0000-0000-0000-000000000002"));
    var z := x;
    Console.WriteLine(x = z);
    Console.WriteLine(x = y);
  end Main;
end Program;
