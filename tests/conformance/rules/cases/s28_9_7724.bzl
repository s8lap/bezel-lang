//! case: s28_9_7724
//! doc: spec
//! section: 28.9
//! title: two lambdas in one arrow body each capture the parameter
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5
//! expect: run
//! stdout: 9
module Conf;
import System;
public class Program
  public static method Apply(f: () -> int): int => f();
  public static method M(p: int): int => Apply(lambda() => p) + Apply(lambda() => p + 1);
  public static method Main(): int
    Console.WriteLine(Program.M(4));
    return 0;
  end Main;
end Program;
