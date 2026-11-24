//! case: l5_7906
//! doc: stdlib
//! section: 5
//! title: Average over a selector returning an enum is rejected (no overload takes an enum
//!   result)
//! rule: public method Average(selector: (T) -> int): double;
//! cites: 5, 22.6
//! expect: reject
//! diagnostic: average
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
    Console.WriteLine(a.Average(lambda(x) => Color.Red));
  end Main;
end Program;
