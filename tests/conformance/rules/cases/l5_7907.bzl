//! case: l5_7907
//! doc: stdlib
//! section: 5
//! title: Average over an int selector returns the double mean
//! rule: public method Average(selector: (T) -> int): double;
//! cites: 5
//! expect: run
//! stdout: 1.5
module Conf;
import System;

public enum Color
  Red,
  Green;
end Color;

public class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(1);
    a.Add(2);
    Console.WriteLine(a.Average(lambda(x) => x));
  end Main;
end Program;
