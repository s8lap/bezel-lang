//! case: s27_5_7212
//! doc: spec
//! section: 27.5
//! title: two closures returned from generic methods observe the same
//!   captured local as their method
//! rule: Modifications to a captured variable inside the lambda are visible
//!   outside, and vice versa.
//! cites: 27.5, 14.1
//! expect: run
//! stdout: 61400
module Conf;

public class Program
  public static method D<T>(z: T, i: int): () -> int
    var s := i;
    s := s + 1;
    return lambda() => do
      s := s + 1;
      return s;
    end;
  end D;

  public static method C<T>(z: T, i: int): () -> int
    var s := i;
    var f := lambda() => do
      s := s + 1;
      return s;
    end;
    var g := lambda() => do
      return s;
    end;
    if i mod 2 = 0 then
      return f;
    end if;
    return g;
  end C;

  public static method Main()
    var total := 0;
    for var i in 1..200 do
      var d := Program.D<long>(1L, i);
      var c := Program.C<string>("a", i);
      total := total + d() + d() + c();
    end for;
    Console.WriteLine(total);
  end Main;
end Program;
