//! case: s28_9_7722
//! doc: spec
//! section: 28.9
//! title: an expression-bodied lambda returned from a method keeps its captured string parameter
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5
//! expect: run
//! stdout: 3
module Conf;
import System;
public class Program
  public static method Id(p: string): () -> string => lambda() => p;
  public static method Main(): int
    var f := Program.Id("hi" + "x");
    Console.WriteLine(f().Length);
    return 0;
  end Main;
end Program;
