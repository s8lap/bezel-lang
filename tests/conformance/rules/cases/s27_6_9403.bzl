//! case: s27_6_9403
//! doc: spec
//! section: 27.6
//! title: a method called through a bare method group local inside its own body recurses
//! rule: Method references work for static methods, instance methods (the instance is captured at
//!   formation
//! cites: 27.6, 27.7
//! expect: run
//! output: 120
module Conf;

public class Program
  static method Fact(n: int): int
    if n <= 1 then
      return 1;
    end if;
    var f := Fact;
    return n * f(n - 1);
  end Fact;
  public static method Main(): int
    Console.WriteLine(Fact(5));
    return 0;
  end Main;
end Program;
