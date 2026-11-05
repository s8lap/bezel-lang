//! case: s12_7_1301
//! doc: spec
//! section: 12.7
//! title: is-tests at an Object carrier answer true for a Stopwatch and a Random and false for other
//!   objects
//! rule: `is T` tests whether a value is an instance of `T` and produces `bool`
//! cites: 8.2, 19.3
//! expect: run
//! output: Stopwatch
//!   Random
//!   other
module Conf;

import System.Diagnostics;

public class Program
  static method Kind(o: Object): string
    if o is Stopwatch then
      return "Stopwatch";
    end;
    if o is Random then
      return "Random";
    end;
    return "other";
  end Kind;

  public static method Main()
    using var sw := new Stopwatch();
    using var r := new Random(1);
    Console.WriteLine(Kind(sw));
    Console.WriteLine(Kind(r));
    Console.WriteLine(Kind("text"));
  end Main;
end Program;
