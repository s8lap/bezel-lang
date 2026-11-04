//! case: s35_7_0012
//! doc: spec
//! section: 35.7
//! title: a choice with a DateTime payload is value-like and its generated = compares the payloads by value
//! rule: The generated `=` is true exactly when both operands have the same active case and every corresponding payload compares equal by its type's `=`
//! cites: 35.7, 35.4, 3.2
//! expect: run
//! issue: 7845
//! output: False
//!   True
//!   2020
//!   2021

module Conf;

public choice Ev
  At(p: DateTime),
  Never;
end Ev;

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
    Console.WriteLine(Yr(y));
    Console.WriteLine(Yr(x));
  end Main;
end Program;
