//! case: l5_7904
//! doc: stdlib
//! section: 5
//! title: Average with a decimal method-group selector returns the decimal mean
//! rule: public method Average(selector: (T) -> decimal): decimal;
//! cites: 5, 27.6
//! expect: run
//! stdout: 1
module Conf;
import System;

public class Program
  static method Half(x: decimal): decimal => x / 2m;
  public static method Main()
    var a := new Array<decimal>();
    a.Add(1.5m);
    a.Add(2.5m);
    var avg := a.Average(Half);
    Console.WriteLine(avg);
  end Main;
end Program;
