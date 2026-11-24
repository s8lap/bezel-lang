//! case: l5_7903
//! doc: stdlib
//! section: 5
//! title: Sum with a long method-group selector does not truncate to int
//! rule: public method Sum<U>(selector: (T) -> U): owned U where U: INumber<U>;
//! cites: 5, 27.6
//! expect: run
//! stdout: 6000000000
module Conf;
import System;

public class Program
  static method Wide(x: int): long => x * 1000000000L;
  public static method Main()
    var a := new Array<int>();
    a.Add(3);
    a.Add(3);
    var t := a.Sum(Wide);
    Console.WriteLine(t);
  end Main;
end Program;
