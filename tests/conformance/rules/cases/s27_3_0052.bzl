//! case: s27_3_0052
//! doc: spec
//! section: 27.3
//! title: an expression-bodied lambda whose body is a lambda yields the inner lambda as its value
//! rule: Expression-bodied lambdas need no inference rule: the body expression's
//!   type is the return type. (v0.338 §27.3)
//! cites: 27.3, 27.5
//! expect: run
//! output: 5
module Conf;

public class Program
  public static method Main()
    var k: int := 4;
    var g := lambda() => lambda() => k + 1;
    Console.WriteLine(g()());
  end Main;
end Program;
