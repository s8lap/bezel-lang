//! case: s27_5_7211
//! doc: spec
//! section: 27.5
//! title: a closure stored from a generic method shares its captured local
//!   with the method body
//! rule: Modifications to a captured variable inside the lambda are visible
//!   outside, and vice versa.
//! cites: 27.5, 14.1
//! expect: run
//! stdout: 62100
module Conf;

public class Program
  public static method Fill<T>(z: T, l: Array<() -> int>, i: int): int
    var s := i;
    l.Add(lambda() => do
      s := s + 2;
      return s;
    end);
    s := s + 1;
    return s;
  end Fill;

  public static method Main()
    var total := 0;
    for var i in 1..200 do
      var l := new Array<() -> int>();
      total := total + Program.Fill<long>(7L, l, i);
      total := total + l[0]() + l[0]();
    end for;
    Console.WriteLine(total);
  end Main;
end Program;
