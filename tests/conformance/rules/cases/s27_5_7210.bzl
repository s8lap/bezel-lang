//! case: s27_5_7210
//! doc: spec
//! section: 27.5
//! title: a string a closure returned from a generic method extends keeps
//!   its value across calls
//! rule: Modifications to a captured variable inside the lambda are visible
//!   outside, and vice versa.
//! cites: 27.5, 14.1
//! expect: run
//! stdout: 1984
module Conf;

public class Program
  public static method Mk<T>(z: T, i: int): () -> string
    var s := "a" + i.ToString();
    var f := lambda() => do
      s := s + "x";
      return s;
    end;
    return f;
  end Mk;

  public static method Main()
    var total := 0;
    for var i in 1..200 do
      var g := Program.Mk<long>(5L, i);
      var a := g();
      var b := g();
      total := total + a.Length + b.Length;
    end for;
    Console.WriteLine(total);
  end Main;
end Program;
