//! case: s35_4_0018
//! doc: spec
//! section: 35.4
//! title: a value-like choice copy is independent of a later reassignment of the source
//! rule: Otherwise the choice is **value-like**: every payload is a value, a `string`, a
//!   runtime-managed type, or (transitively) a value-like record or choice.
//! cites: 35.4, 35.7
//! expect: run
//! output: False
//!   True
//!   2020
//!   2021
module Conf;

public choice Ev At(p: DateTime), Never; end Ev;

public class Program
  static method Yr(e: Ev): int
    return (case e of At(var p) => p.Year, _ => 0);
  end Yr;

  public static method Main()
    var x := Ev.At(new DateTime(2020, 1, 2, 3, 4, 5));
    var y := x;
    x := Ev.At(new DateTime(2021, 1, 2, 3, 4, 5));
    Console.WriteLine(x = y);
    Console.WriteLine(y = Ev.At(new DateTime(2020, 1, 2, 3, 4, 5)));
    Console.WriteLine(Program.Yr(y));
    Console.WriteLine(Program.Yr(x));
  end Main;
end Program;
