//! case: s28_9_7723
//! doc: spec
//! section: 28.9
//! title: nested expression-bodied lambdas capture the enclosing parameter
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5
//! expect: run
//! stdout: 6
module Conf;
import System;
public class Program
  public static method Apply(f: () -> int): int => f();
  public static method M(p: int): int => Apply(lambda() => Apply(lambda() => p + 1));
  public static method Main(): int
    Console.WriteLine(Program.M(5));
    return 0;
  end Main;
end Program;
