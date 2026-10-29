//! case: l2_2_0038
//! doc: stdlib
//! section: 2.2
//! title: Math.Abs of a short is ambiguous and refused
//! rule: stdlib §2.2 declares only Abs(int), Abs(long), Abs(double), Abs(decimal). §5.1 admits a `short` at more than one of them and it matches none exactly, so the candidates are equally ranked (§22.6: "two candidates that both require an implicit conversion at the same position are equally ranked there") and the call is ambiguous, a compile error.
//! cites: 2.2, 5.1, 22.6
//! expect: reject
//! diagnostic: ambiguous
module Conf;

public class Program
  
  public static method Main()
    var b: short := short(-200);
    Console.WriteLine(Math.Abs(b));
  end Main;
end Program;
