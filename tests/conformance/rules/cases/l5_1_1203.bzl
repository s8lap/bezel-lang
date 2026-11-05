//! case: l5_1_1203
//! doc: stdlib
//! section: 5.1
//! title:no-selector Min/Max on a lazy Where return the element for int, double, decimal and char
//! rule:`Min` and `Max` without a selector are generic over comparable types:
//! cites: 5
//! expect: run
//! output: 2
//!   3.5
//!   1.25
//!   b
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [3, 1, 2];
    var d: Array<double> := [3.5, 1.25, 2.0];
    var m: Array<decimal> := [3.5m, 1.25m, 2.0m];
    var c: Array<char> := ['q', 'b', 'z'];
    Console.WriteLine(a.Where(lambda(x: int) => x > 1).Min());
    Console.WriteLine(d.Where(lambda(x: double) => x > 1.0).Max());
    Console.WriteLine(m.Where(lambda(x: decimal) => x > 1.0m).Min());
    Console.WriteLine(c.Where(lambda(x: char) => true).Min());
  end Main;
end Program;
