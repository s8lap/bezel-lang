//! case: s27_5_7209
//! doc: spec
//! section: 27.5
//! title: a mutated captured local of a generic method survives in a
//!   closure returned from it
//! rule: Captured variables are heap-allocated in a closure object
//! cites: 27.5, 14.1
//! expect: run
//! stdout: 40800
module Conf;

public class Program
  public static method Mk<T>(z: T, i: int): () -> int
    var s := i;
    var f := lambda() => do
      s := s + 1;
      return s;
    end;
    return f;
  end Mk;

  public static method Main()
    var total := 0;
    for var i in 1..200 do
      var g := Program.Mk<long>(5L, i);
      total := total + g() + g();
    end for;
    Console.WriteLine(total);
  end Main;
end Program;
