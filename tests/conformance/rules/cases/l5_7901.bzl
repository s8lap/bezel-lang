//! case: l5_7901
//! doc: stdlib
//! section: 5
//! title: Sum with a double method-group selector folds at the selector's result type double
//! rule: public method Sum<U>(selector: (T) -> U): owned U where U: INumber<U>;
//! cites: 5, 27.6
//! expect: run
//! stdout: 3.75
module Conf;
import System;

public class Program
  static method Id(x: double): double => x;
  public static method Main()
    var a := new Array<double>();
    a.Add(1.5);
    a.Add(2.25);
    Console.WriteLine(a.Sum(Id));
  end Main;
end Program;
